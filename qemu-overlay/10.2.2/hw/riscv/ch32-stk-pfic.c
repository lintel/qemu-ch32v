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
 * File:     ch32-stk-pfic.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     SysTick（STK @0xE000F000）与 PFIC IENR/IRER 窗口（与 EVT 向量 12 对齐）。
 */

#include "ch32-machine-internal.h"
#include "system/runstate.h"

static void ch32_stk_schedule(Ch32MachineState *m);

/* ch32_stk_schedule_wrap - 封装为匹配 resync 表函数签名的 wrapper */
static void ch32_stk_schedule_wrap(Ch32MachineState *m)
{
    ch32_stk_schedule(m);
}

/*
 * Software_IRQn = 14：
 * 须同步 MIP_MSIP 并在 WCH VTF 下映射到 EVT 字索引 14（见 target/riscv/cpu_helper.c）。
 */
#define CH32_PFIC_SW_IRQ_BIT 14u

static void ch32_pfic_sw_irq_miesync(Ch32MachineState *m)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;

    BQL_LOCK_GUARD();
    if ((m->pfic_ienr[0] >> CH32_PFIC_SW_IRQ_BIT) & 1u) {
        env->mie |= MIP_MSIP;
    } else {
        env->mie &= ~MIP_MSIP;
        riscv_cpu_update_mip(env, MIP_MSIP, 0);
    }
    riscv_cpu_interrupt(env);
}

static void ch32_pfic_sw_irq_set_pending(Ch32MachineState *m)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;

    if (!((m->pfic_ienr[0] >> CH32_PFIC_SW_IRQ_BIT) & 1u)) {
        return;
    }
    BQL_LOCK_GUARD();
    riscv_cpu_update_mip(env, MIP_MSIP, MIP_MSIP);
    riscv_cpu_interrupt(env);
}

/*
 * ch32_pfic_inject_meip_for_irq - 将指定 word 中 enabled 最低 IRQ 为候选，
 * 若 wch_evt_mcause_override 槽空闲则注入 MEIP。
 * 用于 NVIC_SetPendingIRQ（外设 IRQ）触发中断的通用路径。
 * 对应 EVT INT/Interrupt_Nest 示例中的 NVIC_SetPendingIRQ(WWDG_IRQn) 等。
 *
 * PFIC 真实行为：CPU 接受中断时自动清除 IPR 挂起位、置位 IACTR 激活位。
 * QEMU 近似：注入时立即 pre-clear IPR（模拟接受）。
 */
static void ch32_pfic_inject_meip_for_irq(Ch32MachineState *m,
                                           unsigned wd, uint32_t enabled)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;
    unsigned bit   = (unsigned)__builtin_ctz(enabled);
    unsigned irq_n = wd * 32u + bit;

    BQL_LOCK_GUARD();
    if (env->wch_evt_mcause_override == 0) {
        /*
         * 模拟 PFIC 硬件接受行为：
         * 1. 清除 IPR 挂起位（CPU 进入 ISR 后硬件自动清除）
         * 2. 置位 IACTR 激活位（ISR 执行期间中断活跃）
         * mret 时由 ch32_pfic_irq_active_clear 清除 IACTR，
         * 并由 wch_vtf_exit 清除 wch_evt_mcause_override + 降低 MEIP。
         */
        m->pfic_ipr[wd] &= ~(1u << bit);
        ch32_pfic_irq_active_set(m, irq_n);
        env->mie |= MIP_MEIP;
        env->wch_evt_mcause_override = irq_n;
        riscv_cpu_update_mip(env, MIP_MEIP, MIP_MEIP);
        riscv_cpu_interrupt(env);
    }
}

static uint32_t ch32_stk_hz(const Ch32MachineState *m)
{
    /*
     * STK_CTLR.STCLK (bit 2)：
     *   1 → 直达 HCLK；0 → HCLK/8。
     * 以前这里写死为 96 MHz / 12 MHz，忽略了 RCC CFGR0 的实际配置，
     * 在固件配置 HCLK=48/72/144 MHz 时会导致 SysTick 节拍与真机偏离。
     * 现在改为向 ch32-rcc.c 索取实时 HCLK。
     */
    uint32_t hclk = ch32_rcc_get_hclk_hz(m);
    return (m->stk.ctlr & 4u) ? hclk : (hclk / 8u);
}

static uint64_t ch32_stk_now_cnt(Ch32MachineState *m)
{
    uint32_t hz = ch32_stk_hz(m);
    uint64_t ns;

    if (hz == 0) {
        return m->stk_cnt_at_epoch;
    }
    ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (ns < m->stk_epoch_ns) {
        return m->stk_cnt_at_epoch;
    }
    return m->stk_cnt_at_epoch + muldiv64(ns - m->stk_epoch_ns, hz, 1000000000ull);
}

static bool ch32_pfic_systick_enabled(const Ch32MachineState *m)
{
    return (m->pfic_ienr[0] >> CH32_SYSTICK_NVIC_BIT) & 1u;
}

static void ch32_stk_set_cnt(Ch32MachineState *m, uint64_t cnt)
{
    m->stk_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    m->stk_cnt_at_epoch = cnt;
}

static void ch32_stk_mtie_ensure(Ch32MachineState *m)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;

    BQL_LOCK_GUARD();
    env->mie |= MIP_MTIP;
    riscv_cpu_interrupt(env);
}

static void ch32_stk_clear_mtip(Ch32MachineState *m)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;

    BQL_LOCK_GUARD();
    riscv_cpu_update_mip(env, MIP_MTIP, 0);
}

void ch32_stk_tick(void *opaque)
{
    Ch32MachineState *m = opaque;
    Ch32StkState *stk = &m->stk;
    const uint64_t cmp = ((uint64_t)stk->cmph << 32) | stk->cmpl;
    /* 计数器使能：只需 STE(bit0)=1 且 CMP!=0 即可触发 CNTIF，不依赖 STIE 或 PFIC */
    const bool count_en = (stk->ctlr & 1u) && cmp != 0 && ch32_stk_hz(m) != 0;
    /* 中断使能：需要额外的 STIE(bit1) 且 PFIC 中 SysTick 中断通道已使能 */
    const bool irq_en = count_en && (stk->ctlr & 2u) && ch32_pfic_systick_enabled(m);

    if (!count_en) {
        ch32_stk_schedule(m);
        return;
    }

    /* 无论是否启用中断，计数到达 CMP 时总设置 CNTIF 标志 */
    stk->sr |= 1u;

    if (irq_en) {
        ch32_stk_mtie_ensure(m);
        {
            RISCVCPU *cpu = &m->cpus.harts[0];

            riscv_cpu_update_mip(&cpu->env, MIP_MTIP, MIP_MTIP);
        }
    }
    ch32_stk_schedule(m);
}

static void ch32_stk_schedule(Ch32MachineState *m)
{
    Ch32StkState *stk = &m->stk;
    const uint32_t hz = ch32_stk_hz(m);
    const uint64_t cmp = ((uint64_t)stk->cmph << 32) | stk->cmpl;
    /* 定时器调度只需 STE(bit0)=1 且 CMP!=0，不依赖 STIE 或 PFIC 中断使能 */
    const bool run = (stk->ctlr & 1u) && cmp != 0 && hz != 0;

    if (!m->stk_timer) {
        return;
    }
    if (!run) {
        timer_del(m->stk_timer);
        ch32_stk_clear_mtip(m);
        return;
    }

    {
        /*
         * 不论递增还是递减模式，下一次溢出的展期均为
         *   ticks_to_wrap = cmp - (elapsed_ticks % cmp)
         * 两种模式公式相同，无需分支。
         */
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t c   = m->stk_cnt_at_epoch +
                       muldiv64(now - m->stk_epoch_ns, hz, 1000000000ull);
        uint64_t mod  = c % cmp;
        uint64_t ticks = (mod == 0) ? cmp : (cmp - mod);
        uint64_t delay_ns = muldiv64(ticks, 1000000000ull, hz);

        timer_mod_ns(m->stk_timer, now + delay_ns);
    }
}

/* =====================================================================
 * 统一 PFIC IO handler
 *
 * 寄存器局部（base=CH32_PFIC_BASE=0xE000E000，size=0x1000）：
 *   0x000~0x01F  ISR[8]   只读  中断使能状态（=pfic_ienr）
 *   0x020~0x03F  IPR[8]   只读  中断挂起状态（=pfic_ipr）
 *   0x040        ITHRESDR 读写  中断阈值
 *   0x048        CFGR     只写  SystemReset（KEY3|bit7）
 *   0x04C        GISR     只读  全局中断状态
 *   0x050~0x053  VTFIDR   读写  VTF 中断 ID（字节粒度，4通道各 1 字节）
 *   0x060~0x06F  VTFADDR  读写  VTF 中断地址（4路，每路 4 字节）
 *   0x100~0x11F  IENR[8]  只写  中断使能（写 1 置位）
 *   0x180~0x19F  IRER[8]  只写  中断禁用（写 1 清位）
 *   0x200~0x21F  IPSR[8]  只写  中断挂起（写 1 置位）
 *   0x280~0x29F  IPRR[8]  只写  中断清挂起（写 1 清位）
 *   0x300~0x31F  IACTR[8] 只读  中断激活状态
 *   0x400~0x4FF  IPRIOR   读写  中断优先级（字节粒度）
 *   0xD10        SCTLR    读写  系统控制
 * ==================================================================== */

#define CH32_PFIC_CFGR_SYSRESET_KEY  0xBEEF0000u
#define CH32_PFIC_CFGR_SYSRESET_BIT  (1u << 7)

/*
 * PFIC VTF 寄存器：4路 VTF 通道，每路由一个 ID（VTFIDR）和一个地址（VTFADDR）组成。
 * VTFIDR@0x50~0x53: VTFIDR[0..3]，字节粒度（uint8 数组，core_riscv.h: uint8_t VTFIDR[4]）
 * VTFADDRx@0x60+4*n: [31:1]=地址, [0]=VTFxEN 使能位（core_riscv.h: uint32_t VTFADDR[4]）
 */
#define CH32_PFIC_VTF_NUM  4u

/*
 * PFIC SCTLR 偏移地址：0xD10（不是 0xE00！）
 *   bit31: SYSRST (WO) - 系统复位
 *   bit5:  SETEVENT (WO) - 设置事件
 *   bit4:  SEVONPEND (RW)
 *   bit3:  WFITOWFE (RW)
 *   bit2:  SLEEPDEEP (RW)
 *   bit1:  SLEEPONEXIT (RW)
 */
#define CH32_PFIC_SCTLR_OFF  0xD10u
#define CH32_PFIC_SCTLR_SYSRST    (1u << 31)
#define CH32_PFIC_SCTLR_SETEVENT  (1u << 5)
#define CH32_PFIC_SCTLR_SEVONPEND (1u << 4)
#define CH32_PFIC_SCTLR_WFITOWFE  (1u << 3)
#define CH32_PFIC_SCTLR_SLEEPDEEP  (1u << 2)
#define CH32_PFIC_SCTLR_SLEEPONEXIT (1u << 1)

/* IRQ bit-ranges for selective resync (word/bit within pfic_ienr[]) */
/* SW IRQ 14: word 0 bit 14 */
#define CH32_PFIC_SW_IRQ_WD       0u
#define CH32_PFIC_SW_IRQ_MASK     (1u << CH32_PFIC_SW_IRQ_BIT)
/* STK IRQ 12: word 0 bit 12 */
#define CH32_PFIC_STK_WD          0u
#define CH32_PFIC_STK_MASK        (1u << CH32_SYSTICK_NVIC_BIT)
/* USART1 IRQ 53: word 1 bit 21 */
#define CH32_PFIC_USART1_WD       1u
#define CH32_PFIC_USART1_MASK     (1u << (CH32_USART1_NVIC_IRQ % 32u))
/* USART2/3 IRQ 54/55: word 1 bit 22/23 */
#define CH32_PFIC_USART23_MASK    ((1u << (CH32_USART2_NVIC_IRQ % 32u)) | \
                                   (1u << (CH32_USART3_NVIC_IRQ % 32u)))
/* UART4~8 IRQ 68/69/87/88/89: word 2 */
#define CH32_PFIC_UART48_MASK     ((1u << (CH32_UART4_NVIC_IRQ % 32u)) | \
                                   (1u << (CH32_UART5_NVIC_IRQ % 32u)) | \
                                   (1u << (CH32_UART6_NVIC_IRQ % 32u)) | \
                                   (1u << (CH32_UART7_NVIC_IRQ % 32u)) | \
                                   (1u << (CH32_UART8_NVIC_IRQ % 32u)))
/* ETH IRQ 77: word 2 bit 13 */
#define CH32_PFIC_ETH_WD          2u
#define CH32_PFIC_ETH_MASK        (1u << (CH32_ETH_IRQ_N % 32u))
/* ETH10M IRQ 61: word 1 bit 29 (CH32V20x_D8 / ch32v203rb) */
#define CH32_PFIC_ETH10M_WD       1u
#define CH32_PFIC_ETH10M_MASK     (1u << (CH32_ETH_10M_IRQ_N % 32u))
/* TIM2~TIM7 masks */
#define CH32_PFIC_TIM2_WD         1u
#define CH32_PFIC_TIM2_MASK       (1u << (CH32_TIM2_IRQ_N % 32u))
#define CH32_PFIC_TIM3_MASK       (1u << (CH32_TIM3_IRQ_N % 32u))
#define CH32_PFIC_TIM4_MASK       (1u << (CH32_TIM4_IRQ_N % 32u))
/* TIM5/6/7: IRQ 66/70/71 in word 2 */
#define CH32_PFIC_TIM567_WD       2u
#define CH32_PFIC_TIM5_MASK       (1u << (CH32_TIM5_IRQ_N % 32u))
#define CH32_PFIC_TIM6_MASK       (1u << (CH32_TIM6_IRQ_N % 32u))
#define CH32_PFIC_TIM7_MASK       (1u << (CH32_TIM7_IRQ_N % 32u))
/* USB host IRQ 85: word 2 bit 21 */
#define CH32_PFIC_USBHOST_WD      2u
#define CH32_PFIC_USBHOST_MASK    (1u << (85u % 32u))
/* USB FS host IRQ 83: word 2 bit 19 */
#define CH32_PFIC_USBFSHOST_WD   2u
#define CH32_PFIC_USBFSHOST_MASK  (1u << (83u % 32u))
/*
 * DMA IRQ 分布（CH32V307/317 D8C 变体）：
 *   DMA1 Ch1~5 : IRQ 27~31  -> wd=0 bits 27~31
 *   DMA1 Ch6~7 : IRQ 32~33  -> wd=1 bits 0~1
 *   DMA2 Ch1~5 : IRQ 72~76  -> wd=2 bits 8~12
 *   DMA2 Ch6~7 : IRQ 98~99  -> wd=3 bits 2~3
 *   DMA2 Ch8~11: IRQ 100~103-> wd=3 bits 4~7
 */
#define CH32_PFIC_DMA_WD0_MASK    (0x1Fu << 27)              /* bits 27~31 */
#define CH32_PFIC_DMA_WD1_MASK    (0x03u)                    /* bits 0~1   */
#define CH32_PFIC_DMA_WD2_MASK    (0x1Fu << 8)               /* bits 8~12  */
#define CH32_PFIC_DMA_WD3_MASK    (0xFCu)                    /* bits 2~7   */

/*
 * ch32_pfic_resync_changed - 在 IENR/IRER 写入后，仅对 changed 中
 * 有位变化的外设调用对应 resync，减少不必要的 BQL lock 和 MIP 操作。
 *
 * 采用描述符表驱动：新增外设时只需在表中添加一条记录，无需修改函数体。
 */
typedef struct Ch32PficResyncEntry {
    unsigned wd;          /* pfic_ienr word 索引 */
    uint32_t mask;        /* 关注的 bit mask */
    void (*resync)(Ch32MachineState *m);
} Ch32PficResyncEntry;

static const Ch32PficResyncEntry pfic_resync_table[] = {
    /* word 0 外设 */
    { 0u, CH32_PFIC_SW_IRQ_MASK,  ch32_pfic_sw_irq_miesync  },
    { 0u, CH32_PFIC_STK_MASK,     ch32_stk_schedule_wrap    },
    /* word 1 外设: USART1(53)、USART2/3(54/55)、TIM2/3/4(44/45/46)、ETH10M(61) */
    { 1u, CH32_PFIC_USART1_MASK,  ch32_usart1_meip_resync   },
    { 1u, CH32_PFIC_USART23_MASK, ch32_usart_lite_meip_resync },
    { 1u, CH32_PFIC_TIM2_MASK | CH32_PFIC_TIM3_MASK | CH32_PFIC_TIM4_MASK,
                                   ch32_tim_meip_resync_all  },
    { 1u, CH32_PFIC_ETH10M_MASK,  ch32_eth_10m_meip_resync   },
    /* word 2 外设: UART4~8(68/69/87/88/89)、ETH(77)、TIM5/6/7(66/70/71)、USBHS(85)、USBFS(83) */
    { 2u, CH32_PFIC_UART48_MASK, ch32_usart_lite_meip_resync },
    { 2u, CH32_PFIC_ETH_MASK,     ch32_eth_meip_resync      },
    { 2u, CH32_PFIC_TIM5_MASK | CH32_PFIC_TIM6_MASK | CH32_PFIC_TIM7_MASK,
                                   ch32_tim_meip_resync_all  },
    { 2u, CH32_PFIC_USBHOST_MASK, ch32_usbhost_meip_resync  },
    { 2u, CH32_PFIC_USBFSHOST_MASK, ch32_usbfs_host_meip_resync },
    /* DMA1/DMA2 跨 wd0~wd3，override 繁忙时挂 pfic_ipr 软件 pending */
    { 0u, CH32_PFIC_DMA_WD0_MASK, ch32_dma_meip_resync      },
    { 1u, CH32_PFIC_DMA_WD1_MASK, ch32_dma_meip_resync      },
    { 2u, CH32_PFIC_DMA_WD2_MASK, ch32_dma_meip_resync      },
    { 3u, CH32_PFIC_DMA_WD3_MASK, ch32_dma_meip_resync      },
};

static void ch32_pfic_resync_changed(Ch32MachineState *m,
                                     unsigned wd, uint32_t changed)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(pfic_resync_table); i++) {
        const Ch32PficResyncEntry *e = &pfic_resync_table[i];

        if (e->wd == wd && (changed & e->mask)) {
            e->resync(m);
        }
    }
}

/*
 * ch32_pfic_resync_pending_meip - 当 MEIP owner 释放后，重新扫描所有外设的中断状态。
 *
 * 遵历 pfic_resync_table 全表，顺序与表中第一个匹配项优先级一致：
 * ETH10M 在首位，保证网络包延迟最小。
 */
void ch32_pfic_resync_pending_meip(Ch32MachineState *m)
{
    size_t i;

    if (!m) {
        return;
    }
    for (i = 0; i < ARRAY_SIZE(pfic_resync_table); i++) {
        pfic_resync_table[i].resync(m);
    }
}

/*
 * ch32_pfic_irq_active_set - 在 IACTR 中标记指定 IRQ 为激活状态。
 * 当 CPU 响应中断（进入 IRQ handler）时调用。
 * 真实 PFIC 硬件在进入中断时自动置位 IACTR 对应位。
 */
void ch32_pfic_irq_active_set(Ch32MachineState *m, unsigned irq_n)
{
    unsigned wd = irq_n / 32u;
    unsigned bit = irq_n % 32u;
    if (wd < 8u) {
        m->pfic_iactr[wd] |= (1u << bit);
    }
}

/*
 * ch32_pfic_irq_active_clear - 在 IACTR 中清除指定 IRQ 的激活状态。
 * 当 mret 返回时调用（真实 PFIC 硬件在 mret 时自动清除 IACTR 位）。
 */
void ch32_pfic_irq_active_clear(Ch32MachineState *m, unsigned irq_n)
{
    unsigned wd = irq_n / 32u;
    unsigned bit = irq_n % 32u;
    if (wd < 8u) {
        m->pfic_iactr[wd] &= ~(1u << bit);
    }
}

/*
 * ch32_pfic_resync_software_pending - 扫描 pfic_ipr 中的纯软件挂起中断。
 *
 * 对于通过 NVIC_SetPendingIRQ 设置的挂起位（没有对应的外设 resync 入口），
 * 在 mret 后重新扫描并触发。
 * 排除已有 resync 条目涵盖的外设中断和系统中断（SW_IRQ / SysTick）。
 */
static void ch32_pfic_resync_software_pending(Ch32MachineState *m)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;

    if (env->wch_evt_mcause_override != 0) {
        return; /* 已有活跨 override，不重复注入 */
    }

    /* 排除已有 resync 的外设撩码（SW_IRQ bit14 + SysTick bit24） */
#define CH32_PFIC_RESYNC_MASK_WD0 \
    ((1u << CH32_PFIC_SW_IRQ_BIT) | (1u << CH32_SYSTICK_NVIC_BIT) | \
     CH32_PFIC_DMA_WD0_MASK)
#define CH32_PFIC_RESYNC_MASK_WD1 \
    (CH32_PFIC_USART1_MASK | CH32_PFIC_USART23_MASK | CH32_PFIC_TIM2_MASK | \
     CH32_PFIC_TIM3_MASK | CH32_PFIC_TIM4_MASK | CH32_PFIC_ETH10M_MASK | \
     CH32_PFIC_DMA_WD1_MASK)
#define CH32_PFIC_RESYNC_MASK_WD2 \
    (CH32_PFIC_UART48_MASK | CH32_PFIC_ETH_MASK | CH32_PFIC_TIM5_MASK | \
     CH32_PFIC_TIM6_MASK | CH32_PFIC_TIM7_MASK | CH32_PFIC_USBHOST_MASK | \
     CH32_PFIC_USBFSHOST_MASK | \
     CH32_PFIC_DMA_WD2_MASK)
#define CH32_PFIC_RESYNC_MASK_WD3 \
    (CH32_PFIC_DMA_WD3_MASK)

    for (unsigned wd = 0; wd < 8u; wd++) {
        uint32_t ipr = m->pfic_ipr[wd] & m->pfic_ienr[wd];

        /* 排除已有 resync 涵盖的外设中断 */
        if (wd == 0u) ipr &= ~CH32_PFIC_RESYNC_MASK_WD0;
        if (wd == 1u) ipr &= ~CH32_PFIC_RESYNC_MASK_WD1;
        if (wd == 2u) ipr &= ~CH32_PFIC_RESYNC_MASK_WD2;
        if (wd == 3u) ipr &= ~CH32_PFIC_RESYNC_MASK_WD3;

        if (ipr) {
            ch32_pfic_inject_meip_for_irq(m, wd, ipr);
            return; /* 注入一个即返回 */
        }
    }
}

/*
 * ch32_pfic_on_mret - VTF mret 回调（由 op_helper.c helper_mret 调用）。
 *
 * 当 wch_vtf_in_isr 从 1 降到 0 时（最外层 ISR mret），进行：
 *  1. 清除 IACTR 中当前 IRQ 的激活位
 *  2. 清除 wch_evt_mcause_override
 *  3. 降低 MEIP
 *  4. 重新扫描外设 + 软件挂起中断，触发下一个挂起中断
 *
 * 注：这里不检查 wch_vtf_in_isr == 0，调用旹（op_helper）已经確保
 * 只在 override 非零时调用。
 */
void ch32_pfic_on_mret(CPURISCVState *env, uint32_t irq_n, void *opaque)
{
    Ch32MachineState *m = (Ch32MachineState *)opaque;

    BQL_LOCK_GUARD();
    /* 1. 清除 IACTR */
    ch32_pfic_irq_active_clear(m, irq_n);
    /* 2. 清除 override */
    env->wch_evt_mcause_override = 0;
    /* 3. 降低 MEIP */
    riscv_cpu_update_mip(env, MIP_MEIP, 0);
    /* 4. 重新扫描：先硬件外设，册软件挂起 */
    ch32_pfic_resync_pending_meip(m);
    ch32_pfic_resync_software_pending(m);
    riscv_cpu_interrupt(env);
}

/*
 * ch32_pfic_register_mret_cb - 将 mret 回调注册到 CPU env。
 * 在 machine init 时调用。
 */
void ch32_pfic_register_mret_cb(Ch32MachineState *m)
{
    CPURISCVState *env = &m->cpus.harts[0].env;
    env->wch_pfic_mret_cb  = ch32_pfic_on_mret;
    env->wch_pfic_opaque   = m;
}

static uint64_t ch32_pfic_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;

    /* ISR[8]: 中断使能状态（反映 pfic_ienr） */
    if (addr < 0x020u) {
        unsigned idx = (unsigned)addr / 4u;
        uint32_t v = (idx < 8u) ? m->pfic_ienr[idx] : 0u;
        if (size == 1) return (v >> (8u * ((unsigned)addr & 3u))) & 0xffu;
        if (size == 2) return (v >> (8u * ((unsigned)addr & 2u))) & 0xffffu;
        return v;
    }
    /* IPR[8]: 中断挂起状态 */
    if (addr >= 0x020u && addr < 0x040u) {
        unsigned idx = (unsigned)(addr - 0x020u) / 4u;
        uint32_t v = (idx < 8u) ? m->pfic_ipr[idx] : 0u;
        if (size == 1) return (v >> (8u * ((unsigned)(addr - 0x020u) & 3u))) & 0xffu;
        if (size == 2) return (v >> (8u * ((unsigned)(addr - 0x020u) & 2u))) & 0xffffu;
        return v;
    }
    /* ITHRESDR@0x40: 中断优先级阈值 */
    if (addr == 0x040u) {
        return m->pfic_ithresdr;
    }
    /* GISR@0x4C: 全局中断状态寄存器 */
    if (addr == 0x04Cu) {
        uint32_t gisr = 0;
        /*
         * bit9: GPENDSTA - 是否有中断处于挂起状态
         * bit8: GACTSTA - 是否有中断正在执行
         * bit[7:0]: NESTSTA - 中断嵌套深度
         *
         * NESTSTA 编码：0x01=第1级, 0x03=第2级, 0x07=第3级, ... 0xFF=第8级
         * 在 QEMU 中，用 wch_vtf_in_isr 近似嵌套深度。
         */
        bool has_pending = false;
        bool has_active = false;
        RISCVCPU *cpu;
        CPURISCVState *env;
        unsigned nest;
        uint32_t nest_mask;
        for (unsigned i = 0; i < 8; i++) {
            if (m->pfic_ipr[i]) has_pending = true;
            if (m->pfic_iactr[i]) has_active = true;
        }
        if (has_pending) gisr |= (1u << 9);
        if (has_active)  gisr |= (1u << 8);
        /* NESTSTA: V4F 最大8级嵌套，编码为掩码 */
        cpu = &m->cpus.harts[0];
        env = &cpu->env;
        nest = env->wch_vtf_in_isr;
        if (nest > 8) nest = 8;
        nest_mask = (nest > 0) ? ((1u << nest) - 1u) : 0u;
        gisr |= (nest_mask & 0xFFu);
        return gisr;
    }
    /* VTFIDR[4]@0x50~0x53: VTF 通道 IRQ ID（字节粒度） */
    if (addr >= 0x050u && addr <= 0x053u) {
        unsigned off = (unsigned)(addr - 0x050u);
        uint32_t v32 = (uint32_t)m->pfic_vtfidr[0] |
                       ((uint32_t)m->pfic_vtfidr[1] << 8u) |
                       ((uint32_t)m->pfic_vtfidr[2] << 16u) |
                       ((uint32_t)m->pfic_vtfidr[3] << 24u);
        if (size == 1) return (v32 >> (8u * off)) & 0xffu;
        if (size == 2) return (v32 >> (8u * (off & 2u))) & 0xffffu;
        return v32;
    }
    /* VTFADDR[4]@0x60~0x6F: VTF 通道地址（uint32 数组，步长 4） */
    if (addr >= 0x060u && addr < 0x070u) {
        unsigned ch = (unsigned)(addr - 0x060u) / 4u;
        if (ch < CH32_PFIC_VTF_NUM) {
            return m->pfic_vtfaddr[ch];
        }
        return 0;
    }
    /* IACTR[8]@0x300~0x31F: 中断激活状态（只读） */
    if (addr >= 0x300u && addr < 0x320u) {
        unsigned idx = (unsigned)(addr - 0x300u) / 4u;
        uint32_t v = (idx < 8u) ? m->pfic_iactr[idx] : 0u;
        if (size == 1) return (v >> (8u * ((unsigned)(addr - 0x300u) & 3u))) & 0xffu;
        if (size == 2) return (v >> (8u * ((unsigned)(addr - 0x300u) & 2u))) & 0xffffu;
        return v;
    }
    /* IPRIOR[256]: 中断优先级（字节） */
    if (addr >= 0x400u && addr < 0x500u) {
        unsigned off = (unsigned)(addr - 0x400u);
        if (size == 1) return m->pfic_iprior[off];
        if (size == 2 && off + 1u < 256u)
            return (uint32_t)m->pfic_iprior[off] |
                   ((uint32_t)m->pfic_iprior[off + 1u] << 8u);
        if (size == 4 && off + 3u < 256u)
            return (uint32_t)m->pfic_iprior[off] |
                   ((uint32_t)m->pfic_iprior[off + 1u] << 8u) |
                   ((uint32_t)m->pfic_iprior[off + 2u] << 16u) |
                   ((uint32_t)m->pfic_iprior[off + 3u] << 24u);
        return 0;
    }
    /* SCTLR@0xD10: 系统控制寄存器 */
    if (addr == CH32_PFIC_SCTLR_OFF) {
        /* 只读位：SYSRST(31)/SETEVENT(5) 是 WO，读回 0 */
        return m->pfic_sctlr & (CH32_PFIC_SCTLR_SEVONPEND |
                                 CH32_PFIC_SCTLR_WFITOWFE |
                                 CH32_PFIC_SCTLR_SLEEPDEEP |
                                 CH32_PFIC_SCTLR_SLEEPONEXIT);
    }
    return 0;
}

static void ch32_pfic_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v = (uint32_t)val;
    /* ITHRESDR@0x40: 中断优先级阈値 */
    if (addr == 0x040u && size == 4) {
        /* [7:0] THRESHOLD: 中断优先级阈值；[31:8] 保留 */
        m->pfic_ithresdr = v & 0xFFu;
        return;
    }
    /* CFGR@0x48: NVIC_SystemReset() 写入 KEY3|(1<<7) */
    if (addr == 0x048u) {
        if ((v & 0xFFFF0000u) == CH32_PFIC_CFGR_SYSRESET_KEY &&
            (v & CH32_PFIC_CFGR_SYSRESET_BIT)) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        return;
    }
    /* VTFIDR[4]@0x50~0x53: VTF 通道 IRQ ID（字节粒度） */
    if (addr >= 0x050u && addr <= 0x053u) {
        unsigned off = (unsigned)(addr - 0x050u);
        if (size == 1) {
            m->pfic_vtfidr[off] = (uint8_t)v;
        } else if (size == 2 && off <= 2u) {
            m->pfic_vtfidr[off]     = (uint8_t)v;
            m->pfic_vtfidr[off + 1] = (uint8_t)(v >> 8u);
        } else if (size == 4 && off == 0) {
            m->pfic_vtfidr[0] = (uint8_t)v;
            m->pfic_vtfidr[1] = (uint8_t)(v >> 8u);
            m->pfic_vtfidr[2] = (uint8_t)(v >> 16u);
            m->pfic_vtfidr[3] = (uint8_t)(v >> 24u);
        }
        return;
    }
    /* VTFADDR[4]@0x60~0x6F: VTF 通道地址（uint32 数组，步长 4） */
    if (addr >= 0x060u && addr < 0x070u) {
        unsigned ch = (unsigned)(addr - 0x060u) / 4u;
        if (ch < CH32_PFIC_VTF_NUM && size == 4) {
            m->pfic_vtfaddr[ch] = v;
            return;
        }
    }
    /* IENR[8]@0x100~0x11F: 中断使能（写 1 置位） */
    if (addr >= 0x100u && addr < 0x120u && size == 4 && !(addr & 3u)) {
        unsigned idx = (unsigned)(addr - 0x100u) / 4u;
        uint32_t old = m->pfic_ienr[idx];
        m->pfic_ienr[idx] |= v;
        ch32_pfic_resync_changed(m, idx, m->pfic_ienr[idx] ^ old);
        return;
    }
    /* IRER[8]@0x180~0x19F: 中断禁用（写 1 清位） */
    if (addr >= 0x180u && addr < 0x1a0u && size == 4 && !(addr & 3u)) {
        unsigned idx = (unsigned)(addr - 0x180u) / 4u;
        uint32_t old = m->pfic_ienr[idx];
        m->pfic_ienr[idx] &= ~v;
        ch32_pfic_resync_changed(m, idx, m->pfic_ienr[idx] ^ old);
        return;
    }
    /* IPSR[8]@0x200~0x21F: 中断挂起（写 1 置位） */
    if (addr >= 0x200u && addr < 0x220u && size == 4 && !(addr & 3u)) {
        unsigned idx = (unsigned)(addr - 0x200u) / 4u;
        uint32_t enabled;
        m->pfic_ipr[idx] |= v;
        /*
         * Software_IRQn=14 需要额外触发 MSIP（RISC-V 软中断通道）。
         * 其他外设 IRQ 都进入通用 MEIP 路径。
         */
        if (idx == 0u && (v & (1u << CH32_PFIC_SW_IRQ_BIT))) {
            ch32_pfic_sw_irq_set_pending(m);
        }
        /* 通用路径：若有 IENR 已使能的非-Software bit，注入 MEIP
         * 对应 Interrupt_Nest 示例： NVIC_SetPendingIRQ(WWDG_IRQn/PVD_IRQn/...) */
        enabled = m->pfic_ienr[idx] & v;
        /* 排除 Software_IRQn=14（已由 MSIP 处理） */
        if (idx == 0u) {
            enabled &= ~(1u << CH32_PFIC_SW_IRQ_BIT);
        }
        if (enabled) {
            ch32_pfic_inject_meip_for_irq(m, idx, enabled);
        }
        return;
    }
    /* IPRR[8]@0x280~0x29F: 中断清挂起（写 1 清位） */
    if (addr >= 0x280u && addr < 0x2a0u && size == 4 && !(addr & 3u)) {
        unsigned idx = (unsigned)(addr - 0x280u) / 4u;
        m->pfic_ipr[idx] &= ~v;
        if (idx == 0u && (v & (1u << CH32_PFIC_SW_IRQ_BIT))) {
            RISCVCPU *cpu = &m->cpus.harts[0];
            CPURISCVState *env = &cpu->env;

            BQL_LOCK_GUARD();
            riscv_cpu_update_mip(env, MIP_MSIP, 0);
            riscv_cpu_interrupt(env);
        }
        return;
    }
    /* IPRIOR[256]@0x400~0x4FF: 中断优先级（支持字节/半字/字写入） */
    if (addr >= 0x400u && addr < 0x500u) {
        unsigned off = (unsigned)(addr - 0x400u);
        if (size == 1) {
            m->pfic_iprior[off] = (uint8_t)v;
        } else if (size == 2 && off + 1u < 256u) {
            m->pfic_iprior[off]     = (uint8_t)v;
            m->pfic_iprior[off + 1] = (uint8_t)(v >> 8u);
        } else if (size == 4 && off + 3u < 256u) {
            m->pfic_iprior[off]     = (uint8_t)v;
            m->pfic_iprior[off + 1] = (uint8_t)(v >> 8u);
            m->pfic_iprior[off + 2] = (uint8_t)(v >> 16u);
            m->pfic_iprior[off + 3] = (uint8_t)(v >> 24u);
        }
        return;
    }
    /* SCTLR@0xD10: 系统控制寄存器 */
    if (addr == CH32_PFIC_SCTLR_OFF) {
        /* SYSRST(bit31): WO, 系统复位 */
        if (v & CH32_PFIC_SCTLR_SYSRST) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            return;
        }
        /* SETEVENT(bit5): WO, 设置事件，不存储 */
        /* RW 位: SEVONPEND(4), WFITOWFE(3), SLEEPDEEP(2), SLEEPONEXIT(1) */
        m->pfic_sctlr = v & (CH32_PFIC_SCTLR_SEVONPEND |
                              CH32_PFIC_SCTLR_WFITOWFE |
                              CH32_PFIC_SCTLR_SLEEPDEEP |
                              CH32_PFIC_SCTLR_SLEEPONEXIT);
        return;
    }
    /* 其余未实现寄存器：silently ignore */
    (void)m;
}

const MemoryRegionOps ch32_pfic_ops = {
    .read      = ch32_pfic_read,
    .write     = ch32_pfic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t ch32_stk_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;
    Ch32StkState *stk = &m->stk;
    uint64_t ticks = ch32_stk_now_cnt(m);
    uint32_t v;

    switch (addr) {
    case 0x00:
        v = stk->ctlr;
        break;
    case 0x04:
        v = stk->sr;
        break;
    case 0x08:
        v = (uint32_t)ticks;
        break;
    case 0x0c:
        v = (uint32_t)(ticks >> 32);
        break;
    case 0x10:
        v = stk->cmpl;
        break;
    case 0x14:
        v = stk->cmph;
        break;
    default:
        return 0;
    }
    if (size == 1) {
        return v & 0xff;
    }
    if (size == 2) {
        return v & 0xffff;
    }
    if (size == 4) {
        return v;
    }
    return 0;
}

static void ch32_stk_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Ch32MachineState *m = opaque;
    Ch32StkState *stk = &m->stk;

    if (size != 4) {
        return;
    }
    switch (addr) {
    case 0x00:
        /*
         * STK CTLR bit31 = SWTRG（Software Trigger）：写 1 触发 SW_Handler（Software_IRQn=14）。
         * RT-Thread 的 sw_setpend() 通过此位触发任务切换软件中断；
         * sw_clearpend() 将 bit31 清零（左移1去 bit31 再写回）来取消挂起。
         * 注意：SWTRG 位本身不保存在 ctlr（写 1 触发一次性事件，读回为 0）。
         */
        if ((uint32_t)val & (1u << 31)) {
            ch32_pfic_sw_irq_set_pending(m);
        }
        stk->ctlr = (uint32_t)val & ~(1u << 31);  /* 不保存 SWTRG 位 */
        /*
         * STK CTLR bit5 = INIT: 写 1 时重载 CNT（一次性操作，硬件不保留该位）。
         * 递减模式（bit4=1）下：CNT 被重置为 CMP（即从 CMP 向下计数）。
         * 递增模式（bit4=0）下：CNT 被重置为 0。
         * 两种情况下均重新锤定 epoch 。
         */
        if ((uint32_t)val & (1u << 5)) {
            ch32_stk_set_cnt(m, 0);
            stk->ctlr &= ~(1u << 5);  /* INIT 写 1 后清零（模拟硬件行为） */
        }
        ch32_stk_schedule(m);
        break;
    case 0x04:
        stk->sr = (uint32_t)val;
        if ((stk->sr & 1u) == 0) {
            ch32_stk_clear_mtip(m);
        }
        ch32_stk_schedule(m);
        break;
    case 0x08:
        ch32_stk_set_cnt(m, (uint64_t)(uint32_t)val);
        ch32_stk_schedule(m);
        break;
    case 0x0c:
        ch32_stk_set_cnt(m, (ch32_stk_now_cnt(m) & 0xffffffffull) |
                         ((uint64_t)(uint32_t)val << 32));
        ch32_stk_schedule(m);
        break;
    case 0x10:
        stk->cmpl = (uint32_t)val;
        ch32_stk_schedule(m);
        break;
    case 0x14:
        stk->cmph = (uint32_t)val;
        ch32_stk_schedule(m);
        break;
    default:
        break;
    }
}

/*
 * ch32_stk_pfic_reset - guest reset 时调用（ch32-v.c 的 machine reset 回调）。
 * 清零 pfic_ienr/STK 寄存器、停止 stk_timer、撤销 MTIP/MSIP，使复位后
 * SysTick 不会在 mscratch 尚未初始化时立即重新触发。
 */
void ch32_stk_pfic_reset(Ch32MachineState *m)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;

    /* 停止 SysTick 定时器 */
    if (m->stk_timer) {
        timer_del(m->stk_timer);
    }
    /* 清零 STK 寄存器 */
    memset(&m->stk, 0, sizeof(m->stk));
    m->stk_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    m->stk_cnt_at_epoch = 0;

    /* 清零 pfic_ienr（使能寄存器）、pfic_ipr（挂起）、pfic_iactr（激活）、pfic_iprior（优先级），撤销 MTIP/MSIP */
    memset(m->pfic_ienr, 0, sizeof(m->pfic_ienr));
    /*
     * NMI（IRQ 2）和 Exception（IRQ 3）在 WCH PFIC 中始终使能（硬件不可关闭）。
     * reset 后预置 bit2+bit3 保持与真机 ISR G0=0x0000500c 一致。
     */
    m->pfic_ienr[0] |= (1u << 2) | (1u << 3);
    memset(m->pfic_ipr, 0, sizeof(m->pfic_ipr));
    memset(m->pfic_iactr, 0, sizeof(m->pfic_iactr));
    memset(m->pfic_iprior, 0, sizeof(m->pfic_iprior));
    m->pfic_ithresdr = 0;
    m->pfic_sctlr = 0;
    memset(m->pfic_vtfidr, 0, sizeof(m->pfic_vtfidr));
    memset(m->pfic_vtfaddr, 0, sizeof(m->pfic_vtfaddr));
    {
        BQL_LOCK_GUARD();
        riscv_cpu_update_mip(env, MIP_MTIP | MIP_MSIP, 0);
        env->mie &= ~(MIP_MTIP | MIP_MSIP);
        riscv_cpu_interrupt(env);
    }
}

/*
 * ch32_stk_rcc_clock_changed - RCC 写 CFGR0 导致 HCLK 改变时的重锚钩子。
 * 步骤：
 *   1. 以旧频率读出当前 SysTick 逻辑 CNT。
 *   2. 将该 CNT 作为新 epoch 的起点（epoch_ns = 现在，cnt_at_epoch = 旧频率 CNT）。
 *   3. ch32_stk_schedule 会按新频率重算到 CMP 的时延。
 * 防止频率跳变后 CNT 突变近 8 倍引起固件时间乱跳。
 */
void ch32_stk_rcc_clock_changed(Ch32MachineState *m)
{
    uint64_t cnt;

    if (!m) {
        return;
    }
    cnt = ch32_stk_now_cnt(m);
    ch32_stk_set_cnt(m, cnt);
    ch32_stk_schedule(m);
}

const MemoryRegionOps ch32_stk_ops = {
    .read = ch32_stk_read,
    .write = ch32_stk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
