/*
 * Copyright (c) 2026 Oray Inc. All rights reserved.
 * Copyright (C) 2022-2026 OraySZ / OrayOS-Team.
 *
 * This work is licensed under CC BY-NC-SA 4.0.
 * To view a copy of this license, visit:
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *
 * File:     ch32-rtc.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 RTC（实时时钟）仿真。
 *
 *     RTC：16-bit 寄存器影子 + CNTH/CNTL 基于 QEMU 虚拟时钟实时递增。
 *     CTLRL 合成 RTOFF/RSF/SECF/ALRF 等粘滞标志。
 */

#include "ch32-machine-internal.h"

/*
 * 计算当前秒计数，包含自写入以来的迎头时间。
 * rtc_cnt_set 为 false 时返回 0。
 */
static uint32_t ch32_rtc_current_cnt(Ch32MachineState *m)
{
    uint64_t elapsed_ns;

    if (!m->rtc_cnt_set) {
        return 0;
    }
    elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - m->rtc_base_ns;
    return m->rtc_cnt0 + (uint32_t)(elapsed_ns / 1000000000ULL);
}

/*
 * 计算当前 CTLRL 合成值：
 *   RTOFF(5) 常为 1
 *   RSF(3)   常为 1
 *   SECF(0)  一旦 rtc_cnt_set 就置1（粘滞标志，需软件写 0 清除）
 *   ALRF(1)  当前 CNT >= rtc_alr（已配置闹钟）时置1
 */
static uint16_t ch32_rtc_read_ctlrl(Ch32MachineState *m)
{
    uint16_t base = m->rtc_shadow.hwords[CH32_RTC_CTLRL_OFF / 2]
                    | (CH32_RTC_FLAG_RTOFF | CH32_RTC_FLAG_RSF);

    if (m->rtc_cnt_set) {
        uint32_t cnt = ch32_rtc_current_cnt(m);

        /* SECF：粘滞标志，一旦时钟已处于运行状态即置位，需软件写 0 清除 */
        base |= CH32_RTC_FLAG_SECF;
        /*
         * ALRF：粘滞标志，一旦配置闹钟（rtc_alr != 0）且 CNT 已达到闹钟点即置位。
         * QEMU 紧凑循环中虚拟时钟不必推进1秒，考虑当 ALR 写入时 CNT已经达到
         * （即写入时已被访问的 cnt >= alr）时置位。
         * 此外也处理实际时钟推进后 cnt >= alr 的情况。
         */
        if (m->rtc_alr != 0 && cnt >= m->rtc_alr) {
            base |= CH32_RTC_FLAG_ALRF;
        }
    }
    return base;
}

static uint64_t ch32_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;

    if (addr >= CH32_RTC_SIZE) {
        return 0;
    }
    if (size == 2 && (addr & 1) == 0) {
        if (addr == CH32_RTC_CTLRL_OFF) {
            return ch32_rtc_read_ctlrl(m);
        }
        /* CNTH@0x18, CNTL@0x1C 动态返回实时秒计数 */
        if (m->rtc_cnt_set) {
            uint32_t cnt = ch32_rtc_current_cnt(m);

            if (addr == CH32_RTC_CNTH_OFF) { /* CNTH */
                return (uint16_t)(cnt >> 16);
            }
            if (addr == CH32_RTC_CNTL_OFF) { /* CNTL */
                return (uint16_t)(cnt & 0xffffu);
            }
        }
        return m->rtc_shadow.hwords[addr / 2];
    }
    if (size == 1) {
        hwaddr a = addr & ~1ull;
        uint16_t hv;

        if (a == CH32_RTC_CTLRL_OFF) {
            hv = ch32_rtc_read_ctlrl(m);
        } else if (m->rtc_cnt_set && (a == CH32_RTC_CNTH_OFF || a == CH32_RTC_CNTL_OFF)) {
            uint32_t cnt = ch32_rtc_current_cnt(m);

            hv = (a == CH32_RTC_CNTH_OFF) ? (uint16_t)(cnt >> 16) : (uint16_t)(cnt & 0xffffu);
        } else {
            hv = m->rtc_shadow.hwords[a / 2];
        }
        if (addr & 1) {
            return (hv >> 8) & 0xff;
        }
        return hv & 0xff;
    }
    if (size == 4 && (addr & 3) == 0 && addr + 4 <= CH32_RTC_SIZE) {
        uint32_t lo, hi;

        if (addr == CH32_RTC_CTLRL_OFF) {
            lo = ch32_rtc_read_ctlrl(m);
        } else if (m->rtc_cnt_set && addr == CH32_RTC_CNTH_OFF) {
            /* CNTH/CNTL 合并为 32-bit（size=4 读 0x18） */
            uint32_t cnt = ch32_rtc_current_cnt(m);

            return cnt; /* lo=CNTL in low16, hi=CNTH in high16 */
        } else {
            lo = m->rtc_shadow.hwords[addr / 2];
        }
        hi = m->rtc_shadow.hwords[addr / 2 + 1];
        return (uint64_t)lo | ((uint64_t)hi << 16);
    }
    return 0;
}

static void ch32_rtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Ch32MachineState *m = opaque;

    if (addr >= CH32_RTC_SIZE) {
        return;
    }
    if (size == 2 && (addr & 1) == 0) {
        m->rtc_shadow.hwords[addr / 2] = (uint16_t)val;
        /*
         * CNTH/CNTL 写入：记录当前 QEMU 时钟和秒计数基准。
         * CNTH=0x18, CNTL=0x1C，两个写入顺序任意，最后一次写入时重算全 32-bit。
         */
        if (addr == CH32_RTC_CNTH_OFF || addr == CH32_RTC_CNTL_OFF) {
            uint16_t cnth = m->rtc_shadow.hwords[CH32_RTC_CNTH_OFF / 2];
            uint16_t cntl = m->rtc_shadow.hwords[CH32_RTC_CNTL_OFF / 2];
            uint32_t cnt  = ((uint32_t)cnth << 16) | cntl;

            m->rtc_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            m->rtc_cnt0    = cnt;
            m->rtc_cnt_set = true;
        }
        /* ALR 写入：更新闹钟比较值 */
        if (addr == CH32_RTC_ALRMH_OFF || addr == CH32_RTC_ALRML_OFF) {
            uint16_t alrmh = m->rtc_shadow.hwords[CH32_RTC_ALRMH_OFF / 2];
            uint16_t alrml = m->rtc_shadow.hwords[CH32_RTC_ALRML_OFF / 2];
            m->rtc_alr = ((uint32_t)alrmh << 16) | alrml;
        }
        return;
    }
    if (size == 1) {
        hwaddr a = addr & ~1ull;
        uint16_t cur = m->rtc_shadow.hwords[a / 2];

        if (addr & 1) {
            cur = (cur & 0x00ff) | (((uint16_t)val & 0xff) << 8);
        } else {
            cur = (cur & 0xff00) | ((uint16_t)val & 0xff);
        }
        m->rtc_shadow.hwords[a / 2] = cur;
        return;
    }
    if (size == 4 && (addr & 3) == 0 && addr + 4 <= CH32_RTC_SIZE) {
        m->rtc_shadow.hwords[addr / 2] = (uint16_t)val;
        m->rtc_shadow.hwords[addr / 2 + 1] = (uint16_t)(val >> 16);
        /* CNTH/CNTL 32-bit 写（利用 size=4 覆盖两个16-bit） */
        if (addr == CH32_RTC_CNTH_OFF) {
            uint16_t cnth = (uint16_t)(val >> 16);
            uint16_t cntl = (uint16_t)val;
            uint32_t cnt  = ((uint32_t)cnth << 16) | cntl;

            m->rtc_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            m->rtc_cnt0    = cnt;
            m->rtc_cnt_set = true;
        }
        /* ALR 32-bit 写 */
        if (addr == CH32_RTC_ALRMH_OFF) {
            m->rtc_alr = ((uint32_t)(uint16_t)(val >> 16) << 16) | (uint16_t)val;
        }
    }
}

const MemoryRegionOps ch32_rtc_ops = {
    .read = ch32_rtc_read,
    .write = ch32_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
