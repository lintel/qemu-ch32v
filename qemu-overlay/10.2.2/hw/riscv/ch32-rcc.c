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
 * File:     ch32-rcc.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 RCC 影子寄存器与读侧合成（PLL/HSE/LSI/LSE 就绪等）。
 *     支持 8/16/32-bit 访问（兼容 RCC_ClearITPendingBit 等字节操作）。
 *     补充 BDCTLR（addr=0x20）LSERDY 合成和 BDRST 对 BKP 域的重置语义。
 */

#include "ch32-machine-internal.h"

/* RCC CTLR 只读就绪位掩码 */
#define RCC_RDY_MASK  0x2A020002u  /* HSIRDY|HSERDY|PLLRDY|PLL2RDY|PLL3RDY */
/* RCC CFGR0 只读 SWS 位掩码 */
#define RCC_SWS_MASK  0x0000000Cu

/*
 * CH32V30x 典型内置 HSI 与外部 HSE 晶振频率（fallback）。
 * 优先读取 board 描述中的 hsi_hz / hse_hz，board 未填充时回退到 8 MHz。
 */
#define CH32_RCC_HSI_HZ_DEFAULT  8000000u
#define CH32_RCC_HSE_HZ_DEFAULT  8000000u

static uint32_t ch32_rcc_hsi_hz(const Ch32MachineState *m)
{
    if (m->board && m->board->hsi_hz) {
        return m->board->hsi_hz;
    }
    return CH32_RCC_HSI_HZ_DEFAULT;
}

static uint32_t ch32_rcc_hse_hz(const Ch32MachineState *m)
{
    if (m->board && m->board->hse_hz) {
        return m->board->hse_hz;
    }
    return CH32_RCC_HSE_HZ_DEFAULT;
}

/*
 * CFGR0 字段提取：
 *   SW      [1:0]     0=HSI  1=HSE  2=PLL
 *   HPRE    [7:4]     0b0xxx=/1; 0b1000=/2 ... 0b1111=/512
 *   PPRE1   [10:8]    0b0xx=/1; 0b100=/2 ... 0b111=/16
 *   PLLSRC  [16]      0=HSI/2  1=HSE·PREDIV1（QEMU 简化为 HSE 直达）
 *   PLLMUL  [21:18]   PLL 倍频（采用 STM32F10x 近似：x2..x16）
 */
static uint32_t ch32_rcc_hpre_div(uint32_t hpre)
{
    static const uint16_t tbl[16] = {
        1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u,   /* 0b0xxx: /1 */
        2u, 4u, 8u, 16u, 64u, 128u, 256u, 512u,
    };
    return tbl[hpre & 0xfu];
}

static uint32_t ch32_rcc_ppre_div(uint32_t ppre)
{
    static const uint16_t tbl[8] = {
        1u, 1u, 1u, 1u,   /* 0b0xx: /1 */
        2u, 4u, 8u, 16u,  /* 0b100..0b111: /2../16 */
    };
    return tbl[ppre & 0x7u];
}

static uint32_t ch32_rcc_pll_mul(uint32_t pllmul)
{
    /*
     * 简化实现（STM32F10x 兼容语义）：
     *   0b0000..0b1110 → x2 .. x16；0b1111 → x16
     * CH32V30x 手册 0b1111 为 x18、但主流固件 72 MHz / 96 MHz / 144 MHz 配置
     * 均不遭遇该分支，暂以 x16 封顶。
     */
    uint32_t v = pllmul & 0xfu;
    return (v <= 14u) ? (v + 2u) : 16u;
}

static uint32_t ch32_rcc_calc_sysclk(const Ch32MachineState *m)
{
    uint32_t cfgr0 = m->rcc_shadow.words[1];   /* 0x04 / 4 = 1 */
    uint32_t sw = cfgr0 & 0x3u;
    uint32_t hsi_hz = ch32_rcc_hsi_hz(m);
    uint32_t hse_hz = ch32_rcc_hse_hz(m);

    if (sw == 1u) {
        return hse_hz;
    }
    if (sw == 2u) {
        uint32_t pllsrc = (cfgr0 >> 16) & 1u;
        uint32_t pllmul = (cfgr0 >> 18) & 0xfu;
        uint32_t pll_in = pllsrc ? hse_hz : (hsi_hz / 2u);
        return pll_in * ch32_rcc_pll_mul(pllmul);
    }
    /* SW=0 HSI，或 SW=3 保留 */
    return hsi_hz;
}

uint32_t ch32_rcc_get_hclk_hz(const Ch32MachineState *m)
{
    uint32_t cfgr0 = m->rcc_shadow.words[1];
    uint32_t hpre = (cfgr0 >> 4) & 0xfu;
    uint32_t sysclk = ch32_rcc_calc_sysclk(m);
    uint32_t div = ch32_rcc_hpre_div(hpre);

    return div ? (sysclk / div) : sysclk;
}

uint32_t ch32_rcc_get_pclk1_hz(const Ch32MachineState *m)
{
    uint32_t cfgr0 = m->rcc_shadow.words[1];
    uint32_t ppre1 = (cfgr0 >> 8) & 0x7u;
    uint32_t hclk = ch32_rcc_get_hclk_hz(m);
    uint32_t div = ch32_rcc_ppre_div(ppre1);

    return div ? (hclk / div) : hclk;
}

/* CFGR0 中会影响 HCLK/PCLK1 的位：SW[1:0] | HPRE[7:4] | PPRE1[10:8] | PLLSRC[16] | PLLMUL[21:18] */
#define RCC_CFGR0_CLOCK_MASK  (0x003D07F3u)


static uint32_t ch32_rcc_read_reg(Ch32MachineState *m, uint32_t aligned_addr)
{
    Ch32RccShadow *s = &m->rcc_shadow;
    uint32_t v;

    v = s->words[aligned_addr / 4];

    if (aligned_addr == 0) {
        /* CTLR：所有时钟就绪位始终置 1（防止固件轮询死循环） */
        v |= RCC_RDY_MASK;
    } else if (aligned_addr == 4) {
        /* CFGR0：SWS 根据 SW 的值推断 */
        uint32_t sw = v & 0x03u;
        uint32_t base = v & ~RCC_SWS_MASK;

        if (sw == 2u) {
            v = base | 8u;   /* SWS = PLL */
        } else if (sw == 1u) {
            v = base | 4u;   /* SWS = HSE */
        } else {
            v = base;        /* SWS = HSI */
        }
    } else if (aligned_addr == 0x20) {
        /* BDCTLR：LSERDY 始终就绪 */
        v |= 0x00000002u;
    } else if (aligned_addr == 0x24) {
        /* RSTSCKR：LSIRDY 始终就绪 */
        v |= 0x00000002u;
    }

    return v;
}

static uint64_t ch32_rcc_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t aligned_addr = (uint32_t)(addr & ~3u);
    uint32_t v;

    if (addr >= CH32_RCC_SIZE) {
        ch32_mmio_log_bad_offset("rcc", addr, size, false);
        return 0;
    }
    if (aligned_addr >= CH32_RCC_SIZE) {
        ch32_mmio_log_bad_offset("rcc", addr, size, false);
        return 0;
    }
    if (size != 1 && size != 2 && size != 4) {
        ch32_mmio_log_bad_width("rcc", addr, size, false);
        return 0;
    }

    v = ch32_rcc_read_reg(m, aligned_addr);

    /* 按 addr/size 提取对应字节/半字 */
    switch (size) {
    case 1:
        return (v >> ((addr & 3u) * 8u)) & 0xFFu;
    case 2:
        return (v >> ((addr & 3u) * 8u)) & 0xFFFFu;
    case 4:
    default:
        return v;
    }
}

static void ch32_rcc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Ch32MachineState *m = opaque;
    Ch32RccShadow *s = &m->rcc_shadow;
    uint32_t aligned_addr = (uint32_t)(addr & ~3u);
    uint32_t v, new_val, mask;

    if (addr >= CH32_RCC_SIZE || aligned_addr >= CH32_RCC_SIZE) {
        ch32_mmio_log_bad_offset("rcc", addr, size, true);
        return;
    }
    if (size != 1 && size != 2 && size != 4) {
        ch32_mmio_log_bad_width("rcc", addr, size, true);
        return;
    }

    v = s->words[aligned_addr / 4];

    /* 清除当前 shadow 中的只读位（防止读-修改-写时累积） */
    if (aligned_addr == 0) {
        v &= ~RCC_RDY_MASK;
    } else if (aligned_addr == 4) {
        v &= ~RCC_SWS_MASK;
    }

    /* 根据 addr/size 构建掩码并合并 */
    switch (size) {
    case 1:
        mask = 0xFFu << ((addr & 3u) * 8u);
        new_val = (v & ~mask) | (((uint32_t)val << ((addr & 3u) * 8u)) & mask);
        break;
    case 2:
        mask = 0xFFFFu << ((addr & 3u) * 8u);
        new_val = (v & ~mask) | (((uint32_t)val << ((addr & 3u) * 8u)) & mask);
        break;
    case 4:
    default:
        new_val = (uint32_t)val;
        break;
    }

    /* 再次清除只读位，确保不会被固件写入 */
    if (aligned_addr == 0) {
        new_val &= ~RCC_RDY_MASK;
    } else if (aligned_addr == 4) {
        new_val &= ~RCC_SWS_MASK;
    }

    s->words[aligned_addr / 4] = new_val;

    /*
     * CFGR0 写入可能改变 HCLK/PCLK1。旧值与新值在频率相关位上不同时，
     * 需触发 STK / TIM 以旧频率结算 cnt 后重锚 epoch，再按新频率 reschedule。
     * 注意：shadow 存的是寄存器字面量（已在前面 RCC_SWS_MASK 清除只读位），
     * 与 read_reg 合成 SWS 的路径无关，直接对比 v 与 new_val 即可。
     */
    if (aligned_addr == 4 &&
        ((v ^ new_val) & RCC_CFGR0_CLOCK_MASK) != 0u) {
        ch32_stk_rcc_clock_changed(m);
        ch32_tim_rcc_clock_changed(m);
    }

    /*
     * BDCTLR（addr=0x20）：bit16=BDRST 置位时重置 BKP 备份域。
     */
    if (aligned_addr == 0x20 && (new_val & (1u << 16))) {
        ch32_bkp_reset(m);
    }
}

const MemoryRegionOps ch32_rcc_ops = {
    .read = ch32_rcc_read,
    .write = ch32_rcc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
