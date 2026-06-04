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
 * File:     ch32-iwdt.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     IWDG（独立看门狗 @0x40003000）：CTLR 密钥、PSCR/RLDR、STATR PVU/RVU；
 *     写 0xCCCC 使能后以 LSI≈32kHz/(4<<PR) 递减，到期触发整机复位。
 */

#include "ch32-machine-internal.h"
#include "system/runstate.h"

#define IWDG_KEY_RELOAD   0xaaaau
#define IWDG_KEY_ENABLE   0xccccu
#define IWDG_KEY_UNLOCK   0x5555u
#define IWDG_FLAG_PVU     0x0001u
#define IWDG_FLAG_RVU     0x0002u

#define CH32_IWDG_LSI_HZ  32000u
/*
 * 固件在 main 里于 vTaskStartScheduler() 之前即 IWDG_Enable()，喂狗任务要等调度器
 * 运行后才执行；QEMU 上虚拟时钟与大量 MMIO/TCG 路径下，名义 ~6s 窗口可能偏紧，
 * 表现为反复 guest reset（用户见为「自动重启」）。整体拉长 IWDG 等效周期。
 */
#define CH32_IWDG_QEMU_TIMEOUT_MULT 24u

static uint32_t ch32_iwdt_prescale_div(uint32_t pscr)
{
    unsigned pr = pscr & 7u;

    if (pr > 6u) {
        pr = 6u;
    }
    return 4u << pr;
}

static uint64_t ch32_iwdt_timeout_ns(Ch32MachineState *m)
{
    uint32_t div = ch32_iwdt_prescale_div(m->iwdt_pscr);
    uint32_t rl = (m->iwdt_rldr & 0xfffu) + 1u;

    return muldiv64(rl, (uint64_t)div * 1000000000ull, CH32_IWDG_LSI_HZ) *
           CH32_IWDG_QEMU_TIMEOUT_MULT;
}

static void ch32_iwdt_reload_timer(Ch32MachineState *m)
{
    if (!m->iwdt_timer || !m->iwdt_enabled) {
        return;
    }
    {
        uint64_t ns = ch32_iwdt_timeout_ns(m);
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        timer_mod_ns(m->iwdt_timer, now + (ns ? ns : 1000));
    }
}

static void ch32_iwdt_fire(void *opaque)
{
    (void)opaque;
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void ch32_iwdt_flags_clear(void *opaque)
{
    Ch32MachineState *m = opaque;

    m->iwdt_statr = 0;
}

static void ch32_iwdt_arm_stat_clear(Ch32MachineState *m)
{
    if (!m->iwdt_flag_timer) {
        m->iwdt_flag_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ch32_iwdt_flags_clear, m);
    }
    timer_mod_ns(m->iwdt_flag_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
}

void ch32_iwdt_reset(Ch32MachineState *m)
{
    m->iwdt_rldr = 0xfffu;
    m->iwdt_pscr = 0u;
    m->iwdt_enabled = false;
    m->iwdt_unlock = false;
    m->iwdt_statr = 0;
    if (m->iwdt_timer) {
        timer_del(m->iwdt_timer);
    }
    if (m->iwdt_flag_timer) {
        timer_del(m->iwdt_flag_timer);
    }
}

void ch32_iwdt_init_mr(Ch32MachineState *m, Object *owner)
{
    if (!m->iwdt_timer) {
        m->iwdt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ch32_iwdt_fire, m);
    }
    memory_region_init_io(&m->iwdt_iomem, owner, &ch32_iwdt_ops, m,
                          "ch32-iwdg", CH32_IWDG_SIZE);
    ch32_iwdt_reset(m);
}

static uint64_t ch32_iwdt_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;

    if (addr >= CH32_IWDG_SIZE) {
        return 0;
    }
    switch (addr) {
    case 0:
        return 0;
    case 4:
        return m->iwdt_pscr & 0x7u;
    case 8:
        if (size == 1) {
            return m->iwdt_rldr & 0xffu;
        }
        if (size == 2) {
            return m->iwdt_rldr & 0xffffu;
        }
        return m->iwdt_rldr & 0xfffu;
    case 0x0c:
        return m->iwdt_statr;
    default:
        return 0;
    }
}

static void ch32_iwdt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v = (uint32_t)val;

    if (addr >= CH32_IWDG_SIZE) {
        return;
    }
    switch (addr) {
    case 0:
        if (size == 1) {
            v &= 0xffu;
        } else if (size == 2) {
            v &= 0xffffu;
        }
        if ((v & 0xffffu) == IWDG_KEY_UNLOCK) {
            m->iwdt_unlock = true;
        } else if ((v & 0xffffu) == IWDG_KEY_RELOAD) {
            ch32_iwdt_reload_timer(m);
        } else if ((v & 0xffffu) == IWDG_KEY_ENABLE) {
            m->iwdt_enabled = true;
            m->iwdt_unlock = false;
            ch32_iwdt_reload_timer(m);
        }
        return;
    case 4:
        if (!m->iwdt_unlock || m->iwdt_enabled) {
            return;
        }
        m->iwdt_pscr = v & 7u;
        m->iwdt_statr |= IWDG_FLAG_PVU;
        ch32_iwdt_arm_stat_clear(m);
        return;
    case 8:
        if (!m->iwdt_unlock || m->iwdt_enabled) {
            return;
        }
        m->iwdt_rldr = v & 0xfffu;
        m->iwdt_statr |= IWDG_FLAG_RVU;
        ch32_iwdt_arm_stat_clear(m);
        return;
    default:
        return;
    }
}

const MemoryRegionOps ch32_iwdt_ops = {
    .read = ch32_iwdt_read,
    .write = ch32_iwdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
