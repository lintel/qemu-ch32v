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
 * File:     ch32-tim.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32V30x 通用定时器（TIM2～TIM7）MMIO 仿真。
 *
 *     实现固件所需的最小寄存器集：
 *       - CTLR1（CEN、OPM、DIR、CMS、ARPE、CKD）
 *       - DMAINTENR（UIE：更新中断使能）
 *       - INTFR（UIF：更新中断标志，W0C 写清）
 *       - SWEVGR（UG：产生更新事件）
 *       - CNT、PSC、ATRLR（自动重装载）
 *
 *     定时器时钟：由 RCC 动态推导 APB1/TIMxCLK（不再写死为固定频率）。
 *     中断路由：PFIC MEIP + wch_evt_mcause_override，与 ch32-usart.c / ch32-eth-dwmac.c 相同模式。
 *
 *     TIM 中断号（CH32V307/V317 表 6-2）：
 *       TIM2 = 44，TIM3 = 45，TIM4 = 46，TIM5 = 66，TIM6 = 70，TIM7 = 71
 */

#include "ch32-machine-internal.h"

/* -----------------------------------------------------------------------
 * 寄存器偏移（相对 TIMx 基址；每个逻辑寄存器为 16 位 + 16 位填充）
 * ----------------------------------------------------------------------- */
#define TIM_OFF_CTLR1      0x00u
#define TIM_OFF_CTLR2      0x04u
#define TIM_OFF_SMCFGR     0x08u
#define TIM_OFF_DMAINTENR  0x0Cu
#define TIM_OFF_INTFR      0x10u
#define TIM_OFF_SWEVGR     0x14u
#define TIM_OFF_CHCTLR1    0x18u
#define TIM_OFF_CHCTLR2    0x1Cu
#define TIM_OFF_CCER       0x20u
#define TIM_OFF_CNT        0x24u
#define TIM_OFF_PSC        0x28u
#define TIM_OFF_ATRLR      0x2Cu
/* offset 0x30: RPTCR 仅在 ADTM(TIM1/TIM8) 中存在，GPTM/BCTM 该偏移无效 */
#define TIM_OFF_RPTCR      0x30u
#define TIM_OFF_CH1CVR     0x34u
#define TIM_OFF_CH2CVR     0x38u
#define TIM_OFF_CH3CVR     0x3Cu
#define TIM_OFF_CH4CVR     0x40u
/* offset 0x44: BDTR 仅在 ADTM(TIM1/TIM8) 中存在，GPTM/BCTM 该偏移无效 */
#define TIM_OFF_BDTR       0x44u
#define TIM_OFF_DMACFGR    0x48u
#define TIM_OFF_DMAADR     0x4Cu
/* offset 0x50: AUX 仅在 GPTM(TIM2-5) 中存在，WCH 双边沿捕获扩展，BCTM 无效 */
#define TIM_OFF_AUX        0x50u

/* CTLR1 位 */
#define TIM_CTLR1_CEN      (1u << 0)   /* 计数器使能 */
#define TIM_CTLR1_UDIS     (1u << 1)   /* 禁止更新 */
#define TIM_CTLR1_URS      (1u << 2)   /* 更新请求源 */
#define TIM_CTLR1_OPM      (1u << 3)   /* 单脉冲模式 */
#define TIM_CTLR1_ARPE     (1u << 7)   /* 自动重装载预装载使能 */

/* DMAINTENR 位 */
#define TIM_DMAINTENR_UIE  (1u << 0)   /* 更新中断使能 */
#define TIM_DMAINTENR_CC1IE (1u << 1)
#define TIM_DMAINTENR_CC2IE (1u << 2)
#define TIM_DMAINTENR_CC3IE (1u << 3)
#define TIM_DMAINTENR_CC4IE (1u << 4)

/* INTFR 位 */
#define TIM_INTFR_UIF      (1u << 0)   /* 更新中断标志 */

/* SWEVGR 位 */
#define TIM_SWEVGR_UG      (1u << 0)   /* 产生更新事件 */

/* TIM2～TIM7 MMIO 区域大小（每个定时器 1KB） */
#define CH32_TIM_SIZE      0x400u

/*
 * APB1 定时器时钟（动态，STM32/CH32 TIM 倍频规则）：
 *   TIMxCLK = (PPRE1 == /1) ? PCLK1 : PCLK1 * 2
 * 旧实现写死为 CH32_APB1_CLK_HZ=96 MHz（仅适用默认 PLL 配置），
 * 固件配置 HCLK=48/72/144 MHz 时会出现对不上的节拍。
 * 现改为读 RCC CFGR0 后按实际 HCLK/PCLK1 动态推导。
 */

/* 仿真的 TIM 个数（TIM2～TIM7 共 6 个） */
/* CH32_TIM_COUNT 定义见 ch32-machine-internal.h */

/* TIM2～TIM7 的中断号 */
static const unsigned ch32_tim_irqn[CH32_TIM_COUNT] = {
    CH32_TIM2_IRQ_N, CH32_TIM3_IRQ_N, CH32_TIM4_IRQ_N,
    CH32_TIM5_IRQ_N, CH32_TIM6_IRQ_N, CH32_TIM7_IRQ_N,
};

/* TIM2～TIM7 基址（定义见 ch32-machine-internal.h） */
static const uint64_t ch32_tim_bases[CH32_TIM_COUNT] = {
    CH32_TIM2_BASE, CH32_TIM3_BASE, CH32_TIM4_BASE,
    CH32_TIM5_BASE, CH32_TIM6_BASE, CH32_TIM7_BASE,
};

/*
 * 定时器类型判断：
 *   GPTM（通用定时器）： idx=0..3 （TIM2~TIM5），有捕获比较＋ AUX 扩展寄存器
 *   BCTM（基本定时器）： idx=4..5 （TIM6~TIM7），无捕获比较、无 AUX
 *
 * ADTM（TIM1/TIM8）不在此实现范围，不需判断。
 * RPTCR/BDTR 仅 ADTM 有，本实现中 GPTM/BCTM 均返回0/忽略写。
 */
static bool ch32_tim_is_gptm(const Ch32TimState *t)
{
    return t->idx <= 3u;  /* TIM2～TIM5 */
}

static bool ch32_tim_is_bctm(const Ch32TimState *t)
{
    return t->idx >= 4u;  /* TIM6～TIM7 */
}

/* ---------------------------------------------------------------------- */

/*
 * 预分频后的计数时钟频率。
 * f_cnt = TIMxCLK / (PSC + 1)
 * TIMxCLK = (PPRE1 == /1) ? PCLK1 : PCLK1 * 2（STM32/CH32 通用规则）
 */
static uint32_t ch32_tim_cnt_hz(const Ch32TimState *t)
{
    uint32_t div = (uint32_t)t->psc + 1u;
    uint32_t hclk, pclk1, tim_clk;

    if (!t->machine) {
        /* 早期 init 阶段，machine 未关联：安全回退到 HSI=8 MHz。*/
        return 8000000u / div;
    }
    hclk  = ch32_rcc_get_hclk_hz(t->machine);
    pclk1 = ch32_rcc_get_pclk1_hz(t->machine);
    /* PPRE1 == /1 即 PCLK1 == HCLK 时 TIMxCLK = PCLK1；否则倍频到2× */
    tim_clk = (pclk1 == hclk) ? pclk1 : (pclk1 * 2u);
    return div ? (tim_clk / div) : tim_clk;
}

/*
 * 读取当前 CNT（16 位，在 ATRLR 处回绕）。
 * 采用惰性推进计数，而非每个虚拟周期步进。
 * 若 now_ns 非 0，直接使用（调用方已持有当前时刻时可避免二次读时钟）。
 */
static uint16_t ch32_tim_cnt_now_at(const Ch32TimState *t, uint64_t now_ns)
{
    uint32_t hz;
    uint64_t elapsed_ns, ticks;
    uint16_t reload;

    if (!(t->ctlr1 & TIM_CTLR1_CEN)) {
        return (uint16_t)t->cnt_base;
    }
    hz = ch32_tim_cnt_hz(t);
    if (hz == 0) {
        return (uint16_t)t->cnt_base;
    }
    if (now_ns == 0) {
        now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    if (now_ns <= t->epoch_ns) {
        return (uint16_t)t->cnt_base;
    }
    elapsed_ns = now_ns - t->epoch_ns;
    ticks = muldiv64(elapsed_ns, hz, 1000000000ull);
    reload = (t->atrlr == 0) ? 0xFFFFu : t->atrlr;
    return (uint16_t)((t->cnt_base + ticks) % ((uint32_t)reload + 1u));
}

static uint16_t ch32_tim_cnt_now(const Ch32TimState *t)
{
    return ch32_tim_cnt_now_at(t, 0);
}

/* 为本定时器 IRQ 注入或撤销 MEIP。 */
static void ch32_tim_meip_update(Ch32TimState *t)
{
    Ch32MachineState *m = t->machine;
    RISCVCPU *cpu;
    CPURISCVState *env;
    unsigned n, wd, bit;
    bool ienr_on, uif_active;

    if (!m) {
        return;
    }
    cpu = &m->cpus.harts[0];
    env = &cpu->env;
    n   = ch32_tim_irqn[t->idx];
    wd  = n / 32u;
    bit = n % 32u;

    ienr_on    = (m->pfic_ienr[wd] >> bit) & 1u;
    uif_active = (t->intfr & TIM_INTFR_UIF) &&
                 (t->dmaintenr & TIM_DMAINTENR_UIE);

    BQL_LOCK_GUARD();
    if (!ienr_on || !uif_active) {
        if (env->wch_evt_mcause_override == n) {
            riscv_cpu_update_mip(env, MIP_MEIP, 0);
            env->wch_evt_mcause_override = 0;
            env->mie &= ~MIP_MEIP;
            ch32_pfic_irq_active_clear(m, n);
            /*
             * TIM MEIP 撤销：通知其他外设重新评估其中断请求
             * （与 ch32-eth-10m.c 的 MEIP 撤销路径一致）。
             */
            ch32_pfic_resync_pending_meip(m);
        }
        t->irq_active = false;
        riscv_cpu_interrupt(env);
        return;
    }
    if (t->irq_active) {
        return;
    }
    /*
     * MEIP 仲裁：如果其他外设已持有 MEIP，则不覆盖。
     * 等对方 IRQ 处理完后，对方会调用 ch32_pfic_resync_pending_meip
     * 重新触发我们的注入。
     */
    if (env->wch_evt_mcause_override != 0u && env->wch_evt_mcause_override != n) {
        return;
    }
    env->mie |= MIP_MEIP;
    env->wch_evt_mcause_override = n;
    t->irq_active = true;
    ch32_pfic_irq_active_set(m, n);
    riscv_cpu_update_mip(env, MIP_MEIP, MIP_MEIP);
    riscv_cpu_interrupt(env);
}

/*
 * 调度下一次定时器到期（下一次 UIF 事件）。
 * 在 CEN、PSC、ATRLR 或 CNT 变化时调用。
 * now_ns：当前虚拟时钟；传 0 表示在函数内部读取。
 */
static void ch32_tim_schedule(Ch32TimState *t)
{
    uint32_t hz;
    uint16_t reload, cnt_now;
    uint32_t ticks_left;
    uint64_t delay_ns, now_ns;

    if (!t->timer) {
        return;
    }
    if (!(t->ctlr1 & TIM_CTLR1_CEN)) {
        timer_del(t->timer);
        return;
    }
    hz = ch32_tim_cnt_hz(t);
    if (hz == 0) {
        timer_del(t->timer);
        return;
    }
    /*
     * 只读一次时钟：将 now_ns 传给 ch32_tim_cnt_now_at，
     * 使 timer_mod_ns 与 CNT 计算使用同一时间戳。
     * 原先先 ch32_tim_cnt_now 再 qemu_clock_get_ns 两次调用会产生微小偏差。
     */
    now_ns    = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    reload = (t->atrlr == 0) ? 0xFFFFu : t->atrlr;
    cnt_now = ch32_tim_cnt_now_at(t, now_ns);
    ticks_left = (uint32_t)reload - cnt_now + 1u;
    if (ticks_left == 0) {
        ticks_left = (uint32_t)reload + 1u;
    }
    delay_ns  = muldiv64(ticks_left, 1000000000ull, hz);
    timer_mod_ns(t->timer, now_ns + delay_ns);
}

/* 定时器回调：在计数器溢出（UIF 事件）时触发。 */
static void ch32_tim_tick(void *opaque)
{
    Ch32TimState *t = opaque;

    if (!(t->ctlr1 & TIM_CTLR1_CEN)) {
        return;
    }

    /* 将 CNT 历元重置为 0（超过 ATRLR 后计数器回绕到 0） */
    t->cnt_base  = 0;
    t->epoch_ns  = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* 若未禁止更新则置位 UIF */
    if (!(t->ctlr1 & TIM_CTLR1_UDIS)) {
        t->intfr |= TIM_INTFR_UIF;
        ch32_tim_meip_update(t);
    }

    /* 单脉冲模式：首次溢出后停止 */
    if (t->ctlr1 & TIM_CTLR1_OPM) {
        t->ctlr1 &= (uint16_t)~TIM_CTLR1_CEN;
        return;
    }

    /* 若 ARPE 置位则锁存预装载 */
    if (t->ctlr1 & TIM_CTLR1_ARPE) {
        t->atrlr = t->atrlr_preload;
    }

    ch32_tim_schedule(t);
}

/* ---------------------------------------------------------------------- */
/* MMIO 读/写                                                              */
/* ---------------------------------------------------------------------- */

static uint64_t ch32_tim_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32TimState *t = opaque;
    uint16_t val = 0;

    /* 所有寄存器为 16 位；32 位读返回低 16 位 */
    switch (addr & ~3u) {
    case TIM_OFF_CTLR1:     val = t->ctlr1;     break;
    case TIM_OFF_CTLR2:     val = t->ctlr2;     break;
    case TIM_OFF_SMCFGR:    val = t->smcfgr;    break;
    case TIM_OFF_DMAINTENR: val = t->dmaintenr; break;
    case TIM_OFF_INTFR:     val = t->intfr;     break;
    case TIM_OFF_SWEVGR:    val = 0;            break;  /* 只写 */
    case TIM_OFF_CHCTLR1:
        /* BCTM(TIM6/7) 无捕获比较寄存器，读返回0 */
        val = ch32_tim_is_bctm(t) ? 0u : t->chctlr1;
        break;
    case TIM_OFF_CHCTLR2:
        val = ch32_tim_is_bctm(t) ? 0u : t->chctlr2;
        break;
    case TIM_OFF_CCER:
        val = ch32_tim_is_bctm(t) ? 0u : t->ccer;
        break;
    case TIM_OFF_CNT:       val = ch32_tim_cnt_now(t); break;
    case TIM_OFF_PSC:       val = t->psc;       break;
    case TIM_OFF_ATRLR:
        val = (t->ctlr1 & TIM_CTLR1_ARPE) ? t->atrlr_preload : t->atrlr;
        break;
    case TIM_OFF_RPTCR:
        /*
         * RPTCR(0x30) 仅 ADTM(TIM1/TIM8) 有，GPTM/BCTM 该偏移无效。
         * 返回0 防止固件等待非零。
         */
        val = 0;
        break;
    case TIM_OFF_CH1CVR:
        val = ch32_tim_is_bctm(t) ? 0u : t->ch1cvr;
        break;
    case TIM_OFF_CH2CVR:
        val = ch32_tim_is_bctm(t) ? 0u : t->ch2cvr;
        break;
    case TIM_OFF_CH3CVR:
        val = ch32_tim_is_bctm(t) ? 0u : t->ch3cvr;
        break;
    case TIM_OFF_CH4CVR:
        val = ch32_tim_is_bctm(t) ? 0u : t->ch4cvr;
        break;
    case TIM_OFF_BDTR:
        /*
         * BDTR(0x44) 仅 ADTM(TIM1/TIM8) 有，GPTM/BCTM 该偏移无效。
         * 返回0。
         */
        val = 0;
        break;
    case TIM_OFF_DMACFGR:   val = t->dmacfgr;   break;
    case TIM_OFF_DMAADR:    val = t->dmaadr;    break;
    case TIM_OFF_AUX:
        /*
         * AUX(0x50) 仅 GPTM(TIM2-5) 有，WCH 双边沿捕获扩展。
         * BCTM(TIM6/7) 该偏移无效，返回0。
         */
        val = ch32_tim_is_gptm(t) ? t->aux : 0u;
        break;
    default:                val = 0;            break;
    }

    if (size == 4) {
        return (uint32_t)val;
    }
    if (size == 2) {
        return (addr & 2) ? 0u : (uint32_t)val;
    }
    /* 按字节访问 */
    return (val >> ((addr & 1u) * 8u)) & 0xffu;
}

static void ch32_tim_write(void *opaque, hwaddr addr, uint64_t val64,
                           unsigned size)
{
    Ch32TimState *t = opaque;
    uint16_t v16 = (uint16_t)(val64 & 0xffffu);

    /* 仅接受对低半字的 16 位或 32 位对齐写入 */
    if (size == 4) {
        v16 = (uint16_t)(val64 & 0xffffu);
    } else if (size == 2) {
        if (addr & 2) {
            return;   /* 高半字为填充，忽略 */
        }
        v16 = (uint16_t)(val64 & 0xffffu);
    } else {
        /* 字节写：拼回当前 16 位寄存器值 */
        unsigned shift = (addr & 1u) * 8u;
        hwaddr roff = addr & ~1u;
        uint16_t cur = (uint16_t)ch32_tim_read(t, roff & ~3u, 2);
        cur &= (uint16_t)~(0xffu << shift);
        cur |= (uint16_t)((val64 & 0xffu) << shift);
        v16 = cur;
    }

    switch (addr & ~3u) {
    case TIM_OFF_CTLR1: {
        bool was_en = (t->ctlr1 & TIM_CTLR1_CEN) != 0;
        bool now_en = (v16 & TIM_CTLR1_CEN) != 0;
        t->ctlr1 = v16;
        if (!was_en && now_en) {
            /* 重新锚定 CNT 历元 */
            t->cnt_base = 0;
            t->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        ch32_tim_schedule(t);
        break;
    }
    case TIM_OFF_CTLR2:
        t->ctlr2 = v16;
        break;
    case TIM_OFF_SMCFGR:
        t->smcfgr = v16;
        break;
    case TIM_OFF_DMAINTENR:
        t->dmaintenr = v16;
        ch32_tim_meip_update(t);
        break;
    case TIM_OFF_INTFR:
        /* W0C：固件写 0 清除对应标志位 */
        t->intfr &= v16;
        if (!(t->intfr & TIM_INTFR_UIF)) {
            t->irq_active = false;
        }
        ch32_tim_meip_update(t);
        break;
    case TIM_OFF_SWEVGR:
        /* UG：强制更新事件（重载 CNT、锁存预装载、置位 UIF） */
        if (v16 & TIM_SWEVGR_UG) {
            t->cnt_base = 0;
            t->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            if (t->ctlr1 & TIM_CTLR1_ARPE) {
                t->atrlr = t->atrlr_preload;
            }
            if (!(t->ctlr1 & TIM_CTLR1_URS)) {
                t->intfr |= TIM_INTFR_UIF;
                ch32_tim_meip_update(t);
            }
            ch32_tim_schedule(t);
        }
        break;
    case TIM_OFF_CHCTLR1:
        if (!ch32_tim_is_bctm(t)) {
            t->chctlr1 = v16;
        }
        break;
    case TIM_OFF_CHCTLR2:
        if (!ch32_tim_is_bctm(t)) {
            t->chctlr2 = v16;
        }
        break;
    case TIM_OFF_CCER:
        if (!ch32_tim_is_bctm(t)) {
            t->ccer = v16;
        }
        break;
    case TIM_OFF_CNT:
        t->cnt_base = v16;
        t->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ch32_tim_schedule(t);
        break;
    case TIM_OFF_PSC:
        t->psc = v16;
        /* 预分频在下次 UG 或溢出时生效；此处重新锚定历元 */
        t->cnt_base = ch32_tim_cnt_now(t);
        t->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ch32_tim_schedule(t);
        break;
    case TIM_OFF_ATRLR:
        if (t->ctlr1 & TIM_CTLR1_ARPE) {
            t->atrlr_preload = v16;
        } else {
            t->atrlr = v16;
            t->atrlr_preload = v16;
            ch32_tim_schedule(t);
        }
        break;
    case TIM_OFF_RPTCR:
        /* RPTCR(0x30) 仅 ADTM 有，GPTM/BCTM 则忽略写 */
        break;
    case TIM_OFF_CH1CVR:
        if (!ch32_tim_is_bctm(t)) {
            t->ch1cvr = v16;
        }
        break;
    case TIM_OFF_CH2CVR:
        if (!ch32_tim_is_bctm(t)) {
            t->ch2cvr = v16;
        }
        break;
    case TIM_OFF_CH3CVR:
        if (!ch32_tim_is_bctm(t)) {
            t->ch3cvr = v16;
        }
        break;
    case TIM_OFF_CH4CVR:
        if (!ch32_tim_is_bctm(t)) {
            t->ch4cvr = v16;
        }
        break;
    case TIM_OFF_BDTR:
        /* BDTR(0x44) 仅 ADTM 有，GPTM/BCTM 则忽略写 */
        break;
    case TIM_OFF_DMACFGR: t->dmacfgr = v16; break;
    case TIM_OFF_DMAADR:  t->dmaadr  = v16; break;
    case TIM_OFF_AUX:
        /* AUX(0x50) 仅 GPTM(TIM2-5) 有，BCTM 则忽略写 */
        if (ch32_tim_is_gptm(t)) {
            t->aux = v16;
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ch32_tim_ops = {
    .read  = ch32_tim_read,
    .write = ch32_tim_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* ---------------------------------------------------------------------- */
/* 初始化 / 复位                                                            */
/* ---------------------------------------------------------------------- */

/*
 * ch32_tim_reset_one - 将单个 TIM 实例复位为上电默认值。
 */
static void ch32_tim_reset_one(Ch32TimState *t)
{
    if (t->timer) {
        timer_del(t->timer);
    }
    t->ctlr1     = 0;
    t->ctlr2     = 0;
    t->smcfgr    = 0;
    t->dmaintenr = 0;
    t->intfr     = 0;
    t->chctlr1   = 0;
    t->chctlr2   = 0;
    t->ccer      = 0;
    t->psc       = 0;
    t->atrlr     = 0xFFFFu;
    t->atrlr_preload = 0xFFFFu;
    t->rptcr     = 0;  /* 仅 ADTM 有，此处为 GPTM/BCTM 占位字段 */
    t->ch1cvr = t->ch2cvr = t->ch3cvr = t->ch4cvr = 0;
    t->bdtr      = 0;  /* 仅 ADTM 有，此处为 GPTM/BCTM 占位字段 */
    t->dmacfgr   = 0;
    t->dmaadr    = 0;
    t->aux       = 0;  /* GPTM AUX 复位值 0x0000 */
    t->cnt_base  = 0;
    t->epoch_ns  = 0;
    t->irq_active = false;
}

/* 各 TIM 状态现挂在 Ch32MachineState::tims[CH32_TIM_COUNT]，
 * 已移除静态全局数组，统一通过 m->tims[i] 访问。 */

/*
 * ch32_tim_reset - 复位所有 TIM 实例（由 machine reset 调用）。
 */
void ch32_tim_reset(Ch32MachineState *m)
{
    unsigned i;

    for (i = 0; i < CH32_TIM_COUNT; i++) {
        Ch32TimState *t = &m->tims[i];
        ch32_tim_reset_one(t);
        /* 复位后重新同步 IRQ 状态 */
        if (t->machine) {
            ch32_tim_meip_update(t);
        }
    }
}

/*
 * ch32_tim_init_all - 将所有 TIM 的 MMIO 区域挂入系统地址空间，
 * 并创建对应的 QEMU 虚拟定时器。
 * 由 ch32-v.c 的 ch32_machine_init 调用。
 */
void ch32_tim_init_all(Ch32MachineState *m, Object *owner)
{
    MemoryRegion *sysmem = get_system_memory();
    unsigned i;

    for (i = 0; i < CH32_TIM_COUNT; i++) {
        Ch32TimState *t = &m->tims[i];
        char name[32];

        ch32_tim_reset_one(t);
        t->machine = m;
        t->idx     = i;

        snprintf(name, sizeof(name), "ch32-tim%u", (unsigned)(i + 2));
        memory_region_init_io(&t->iomem, owner, &ch32_tim_ops, t,
                              name, CH32_TIM_SIZE);
        memory_region_add_subregion_overlap(sysmem, ch32_tim_bases[i],
                                            &t->iomem, 2);

        t->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ch32_tim_tick, t);
    }
}

/*
 * ch32_tim_meip_resync_all - 将所有 TIM 的 IRQ 线重新同步到 PFIC。
 * 在 IENR 变化后由 ch32-stk-pfic.c 的 ch32_pfic_resync_all 调用。
 */
void ch32_tim_meip_resync_all(Ch32MachineState *m)
{
    unsigned i;

    for (i = 0; i < CH32_TIM_COUNT; i++) {
        ch32_tim_meip_update(&m->tims[i]);
    }
}

/*
 * ch32_tim_rcc_clock_changed - RCC 写 CFGR0 导致 PCLK1/HCLK 改变时的重锚钩子。
 * 对所有已启用的 TIMx：
 *   1. 以旧 TIMxCLK 结算出当前 CNT、存到 cnt_base。
 *   2. epoch_ns 更新为当前虚拟时钟。
 *   3. ch32_tim_schedule 按新 TIMxCLK 重算到 ATRLR 的剩余时延。
 * CEN=0 的 TIM 不在计时，跳过。
 */
void ch32_tim_rcc_clock_changed(Ch32MachineState *m)
{
    unsigned i;
    uint64_t now_ns;

    if (!m) {
        return;
    }
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (i = 0; i < CH32_TIM_COUNT; i++) {
        Ch32TimState *t = &m->tims[i];

        if (!(t->ctlr1 & TIM_CTLR1_CEN)) {
            continue;
        }
        t->cnt_base = ch32_tim_cnt_now_at(t, now_ns);
        t->epoch_ns = now_ns;
        ch32_tim_schedule(t);
    }
}
