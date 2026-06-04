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
 * File:     ch32-usart.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-18
 *
 * Description:
 *     CH32 USART1 完整仿真（手册第 18 章，CH32FV2x_V3xRM）
 *
 *     寄存器布局（每项物理宽 16 位，32 位地址对齐）：
 *       +0x00  STATR  状态寄存器
 *       +0x04  DATAR  数据寄存器（读=RDR，写=TDR）
 *       +0x08  BRR    波特率寄存器
 *       +0x0C  CTLR1  控制寄存器1
 *       +0x10  CTLR2  控制寄存器2
 *       +0x14  CTLR3  控制寄存器3
 *       +0x18  GPR    保护时间/预分频
 *       +0x1C  CTRL4  MARK/SPACE 校验（批号限定）
 *
 *     实现要点：
 *       - RX FIFO 64 字节：缓冲从 char backend 收到的字节，逐字节呈现给固件。
 *       - TX FIFO 64 字节：固件写 DATAR 进 FIFO，由 tx_timer 异步 drain 到 backend。
 *       - STATR：TXE/TC 反映 TX FIFO 状态；RXNE 反映 RX FIFO 非空；
 *         软件写 0 可清 TC/RXNE/CTS/LBD（RW0 位）。
 *       - 中断路由：RXNEIE、TXEIE、TCIE 分别对应 CTLR1 位 5/7/6，
 *         通过 wch_evt_mcause_override=53 + MIP_MEIP 路由给固件 ISR。
 *       - 宽松模式：UE=0 时 TXE/TC 仍置 1，允许未初始化直接 putc。
 */

#include "ch32-machine-internal.h"

/* =========================================================
 * STATR 位掩码（手册表 5.1，偏移 0x00）
 * ========================================================= */
#define STATR_PE      (1u <<  0)  /* 奇偶校验错  RO */
#define STATR_FE      (1u <<  1)  /* 帧错误      RO */
#define STATR_NE      (1u <<  2)  /* 噪声        RO */
#define STATR_ORE     (1u <<  3)  /* 过载错误    RO */
#define STATR_IDLE    (1u <<  4)  /* 总线空闲    RO */
#define STATR_RXNE    (1u <<  5)  /* 接收非空    RW0 */
#define STATR_TC      (1u <<  6)  /* 发送完成    RW0 */
#define STATR_TXE     (1u <<  7)  /* TDR 空      RO（写 DATAR 清） */
#define STATR_LBD     (1u <<  8)  /* LIN Break   RW0 */
#define STATR_CTS     (1u <<  9)  /* nCTS 变化   RW0 */
/* bit10=RX_BUSY, bit11=MS_ERR（批号限定，忽略） */

/* 软件写 0 可清的位（RW0）：RXNE / TC / LBD / CTS */
#define STATR_W0C_MASK (STATR_RXNE | STATR_TC | STATR_LBD | STATR_CTS)

/* CTLR1 中断使能位 */
#define CTLR1_SBK     (1u <<  0)
#define CTLR1_RWU     (1u <<  1)
#define CTLR1_RE      (1u <<  2)
#define CTLR1_TE      (1u <<  3)
#define CTLR1_IDLEIE  (1u <<  4)
#define CTLR1_RXNEIE  (1u <<  5)
#define CTLR1_TCIE    (1u <<  6)
#define CTLR1_TXEIE   (1u <<  7)
#define CTLR1_PEIE    (1u <<  8)
#define CTLR1_UE      (1u << 13)

/* TX drain 间隔（纳秒）：约 1 个仿真字节的时间，足够快但不浪费 CPU */
#define CH32_USART1_TX_DRAIN_NS 10000LL

/* CTLR3 DMA 控制位（手册表 18-4） */
#define CTLR3_DMAR    (1u << 6)   /* RX DMA 使能 */
#define CTLR3_DMAT    (1u << 7)   /* TX DMA 使能 */

/*
 * CH32V307/317 USART DMA 通道映射（手册 Table 6-2）
 * USART1 TX: DMA1_Ch4  IRQ 30 (pfic_ienr[0] bit30)
 * USART1 RX: DMA1_Ch5  IRQ 31 (pfic_ienr[0] bit31)
 * USART2 TX: DMA1_Ch7  IRQ 33 (pfic_ienr[1] bit1)
 * USART2 RX: DMA1_Ch6  IRQ 32 (pfic_ienr[1] bit0)
 * USART3 TX: DMA1_Ch2  IRQ 28 (pfic_ienr[0] bit28)
 * USART3 RX: DMA1_Ch3  IRQ 29 (pfic_ienr[0] bit29)
 */
#define CH32_USART1_DMA_TX_CH   4u   /* DMA1 Ch4 索引在 ch1[]中 = ch-1 = 3 */
#define CH32_USART1_DMA_RX_CH   5u   /* DMA1 Ch5 索引 = 4 */
#define CH32_USART1_DMA_TX_IRQ  30u
#define CH32_USART1_DMA_RX_IRQ  31u
#define CH32_USART2_DMA_TX_CH   7u   /* DMA1 Ch7 索引 = 6 */
#define CH32_USART2_DMA_RX_CH   6u   /* DMA1 Ch6 索引 = 5 */
#define CH32_USART2_DMA_TX_IRQ  33u
#define CH32_USART2_DMA_RX_IRQ  32u
#define CH32_USART3_DMA_TX_CH   2u   /* DMA1 Ch2 索引 = 1 */
#define CH32_USART3_DMA_RX_CH   3u   /* DMA1 Ch3 索引 = 2 */
#define CH32_USART3_DMA_TX_IRQ  28u
#define CH32_USART3_DMA_RX_IRQ  29u

/* =========================================================
 * 内部工具函数
 * ========================================================= */

static bool ch32_usart1_rx_enabled(const Ch32MachineState *m)
{
    return (m->usart1.ctlr1 & CTLR1_UE) &&
           (m->usart1.ctlr1 & CTLR1_RE);
}

/* RX FIFO 辅助 */
static bool ch32_rxfifo_empty(const Ch32MachineState *m)
{
    return m->usart1.rx_head == m->usart1.rx_tail;
}

static bool ch32_rxfifo_full(const Ch32MachineState *m)
{
    return ((m->usart1.rx_tail + 1) % CH32_USART1_RXFIFO_DEPTH)
           == m->usart1.rx_head;
}

/* TX FIFO 辅助 */
static bool ch32_txfifo_empty(const Ch32MachineState *m)
{
    return m->usart1.tx_head == m->usart1.tx_tail;
}

static bool ch32_txfifo_full(const Ch32MachineState *m)
{
    return ((m->usart1.tx_tail + 1) % CH32_USART1_TXFIFO_DEPTH)
           == m->usart1.tx_head;
}

/* =========================================================
 * STATR 硬件当前值
 * ========================================================= */
static uint16_t ch32_usart1_statr_hw(const Ch32MachineState *m)
{
    uint16_t st = m->usart1.statr;

    /*
     * 动态位：RXNE / TXE / TC 由 FIFO 状态实时派生，
     * 其他位（ORE/FE/NE/PE 等）保存在 statr 字段中由事件置位。
     *
     * RXNE：RX FIFO 非空。
     * TXE ：TX FIFO 非满（可以再写一字节）或 UE=0（宽松模式）。
     * TC  ：TX FIFO 已空且最后一字节发完（tx_complete）。
     *
     * 宽松模式：UE=0 或 TE=0 时 TXE|TC 始终为 1，
     * 允许固件未配置 USART 时直接用轮询方式 putc（调试常见）。
     */
    bool ue = (m->usart1.ctlr1 & CTLR1_UE) != 0;
    bool te = (m->usart1.ctlr1 & CTLR1_TE) != 0;

    /* 清掉动态位，再根据当前状态重新置 */
    st &= (uint16_t)~(STATR_RXNE | STATR_TXE | STATR_TC);

    if (!ch32_rxfifo_empty(m)) {
        st |= STATR_RXNE;
    }
    if (!ue || (ue && te)) {
        /* UE=0（宽松）或 UE+TE 都使能：根据 TX FIFO 状态 */
        if (!ue || m->usart1.tx_empty) {
            st |= STATR_TXE;
        }
        if (!ue || m->usart1.tx_complete) {
            st |= STATR_TC;
        }
    }

    return st;
}

/* =========================================================
 * 中断同步：将 USART1 中断状态同步到 MEIP
 *
 * 触发条件（任一为真则置 MEIP）：
 *   RXNEIE && RXNE     — 接收非空
 *   TXEIE  && TXE      — 发送缓冲空
 *   TCIE   && TC       — 发送完成
 *
 * 重入保护：若 pfic_iactr 中 IRQ 53 已激活（正在被服务），
 * 则不重复触发。这模拟真实 PFIC 硬件在 IRQ handler 执行期间
 * 阻止同一 IRQ 重入的行为（类似 ARM Cortex-M IACTR 机制）。
 * ========================================================= */
void ch32_usart1_meip_resync(Ch32MachineState *m)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;
    const unsigned n   = CH32_USART1_NVIC_IRQ;   /* 53 */
    const unsigned wd  = n / 32u;                 /* 1  */
    const unsigned bit = n % 32u;                 /* 21 */
    bool ienr_on = (m->pfic_ienr[wd] >> bit) & 1u;

    uint16_t st = ch32_usart1_statr_hw(m);
    uint32_t c1 = m->usart1.ctlr1;

    bool irq =  ienr_on && (
                ((c1 & CTLR1_RXNEIE) && (st & STATR_RXNE)) ||
                ((c1 & CTLR1_TXEIE)  && (st & STATR_TXE))  ||
                ((c1 & CTLR1_TCIE)   && (st & STATR_TC)));

    BQL_LOCK_GUARD();

    if (irq) {
        /*
         * MEIP 仲裁：如果其他外设已持有 MEIP override，则不覆盖。
         * 等对方 IRQ 处理完调用 ch32_pfic_resync_pending_meip 时，
         * 会重新进入本函数完成注入（与 ETH/ETH10M/TIM 路径一致）。
         */
        if (env->wch_evt_mcause_override != 0u &&
            env->wch_evt_mcause_override != n) {
            return;
        }
        env->mie |= MIP_MEIP;  /* PFIC IENR 控制，不需固件写 mie CSR */
        env->wch_evt_mcause_override = n;
        ch32_pfic_irq_active_set(m, n);
        riscv_cpu_update_mip(env, MIP_MEIP, MIP_MEIP);
    } else {
        if (env->wch_evt_mcause_override == n) {
            /*
             * 判断 ISR 是否仍在执行（wch_vtf_in_isr > 0）。
             *
             * 若 ISR 尚未 mret（wch_vtf_in_isr > 0）：
             *   仅撤销 MIP.MEIP，防止 CPU 重入同一中断，但不清除
             *   wch_evt_mcause_override 也不清 iactr。
             *   原因：wch_vtf_async_vec_word() 在下次 MEIP 触发时需要用
             *   wch_evt_mcause_override 查 EVT 字表得到正确的 ISR 入口（索引53）；
             *   若此处提前清零，再次触发 MEIP 时将退化到 cause=IRQ_M_EXT=11，
             *   跳入错误的向量，导致 msh 命令输入的后续字节无法被正确处理。
             *
             * 若 ISR 已退出（wch_vtf_in_isr == 0，通常由 mret→wch_vtf_exit 触发）：
             *   正常清除所有状态，并重新扫描其他外设。
             */
            if (env->wch_vtf_in_isr > 0) {
                /* ISR 执行中：只撤 MEIP，保留 owner/iactr */
                riscv_cpu_update_mip(env, MIP_MEIP, 0);
            } else {
                /* ISR 已退出：完整清理并重新扫描 */
                riscv_cpu_update_mip(env, MIP_MEIP, 0);
                env->wch_evt_mcause_override = 0;
                ch32_pfic_irq_active_clear(m, n);
                /*
                 * USART1 中断处理完成（IRQ 降低）：
                 * 重新扫描其他外设以及 USART1 自身（若还有剩余字节）的中断请求。
                 */
                ch32_pfic_resync_pending_meip(m);
            }
        }
    }
    riscv_cpu_interrupt(env);
}

/* =========================================================
 * TX FIFO drain（由定时器驱动）
 *
 * 每次触发时尽量将 TX FIFO 中的所有字节一次性写入 char backend，
 * 写完后更新 tx_empty / tx_complete / STATR，并重新同步中断。
 * 若 backend 不可写（缓冲满），重新arm定时器稍后再试。
 * ========================================================= */
static void ch32_usart1_tx_drain(void *opaque)
{
    Ch32MachineState *m = opaque;
    bool changed = false;

    while (!ch32_txfifo_empty(m)) {
        uint8_t ch = m->usart1.tx_buf[m->usart1.tx_head];
        int n = qemu_chr_fe_write(&m->usart1.chr, &ch, 1);
        if (n <= 0) {
            /* backend 暂时不可写，稍后重试 */
            timer_mod(m->usart1.tx_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      CH32_USART1_TX_DRAIN_NS);
            goto done;
        }
        m->usart1.tx_head = (m->usart1.tx_head + 1) % CH32_USART1_TXFIFO_DEPTH;
        changed = true;
    }

    /* TX FIFO 已空 */
    if (changed || !m->usart1.tx_empty) {
        m->usart1.tx_empty    = true;
        m->usart1.tx_complete = true;
        changed = true;
    }

done:
    if (changed) {
        ch32_usart1_meip_resync(m);
    }
}

/* =========================================================
 * char backend 回调：can_receive
 * ========================================================= */
static int ch32_usart1_can_receive(void *opaque)
{
    Ch32MachineState *m = opaque;

    if (!qemu_chr_fe_backend_connected(&m->usart1.chr)) {
        return 0;
    }
    /*
     * 不以 rx_enabled (UE+RE) 作为门控。
     *
     * 原因：stdio backend 在 can_receive() 返回 0 后会撤销对 stdin fd 的监听，
     * 即使后续固件使能了 RE，除非有主动 accept_input 触发，否则 stdin
     * 将永久沉默。这导致在原生终端下键盘输入完全失效。
     *
     * 正确做法：只要 stage 缓冲有空间就告知 backend 可以接收；receive() 回调内
     * 再判断 rx_enabled，UE+RE=0 时把字节丢弃即可。
     *
     * 注意：这里检查的是 rx_stage_buf 的剩余容量，不是 rx_buf。
     * rx_stage_buf 是 backend -> QEMU 的消化缓冲，rx_buf 是 QEMU -> 固件的 FIFO。
     */
    {
        unsigned used = (m->usart1.rx_stage_tail + CH32_USART1_RXFIFO_DEPTH
                         - m->usart1.rx_stage_head) % CH32_USART1_RXFIFO_DEPTH;
        return (used < CH32_USART1_RXFIFO_DEPTH - 1) ? 1 : 0;
    }
}

/* =========================================================
 * char backend 回调：receive
 *
 * 所有字节先入 rx_stage_buf，由 rx_timer 逐字节投喂到 rx_buf。
 * 这模拟真实串口的字节间隔，确保每个字节都能单独触发一次 IRQ。
 * ========================================================= */
static void ch32_usart1_receive(void *opaque, const uint8_t *buf, int size)
{
    Ch32MachineState *m = opaque;
    int i;

    if (size < 1) {
        return;
    }
    /* UE+RE 未使能：丢弃字节但保持 backend 监听 */
    if (!ch32_usart1_rx_enabled(m)) {
        return;
    }
    /* 将字节导入 stage 缓冲 */
    for (i = 0; i < size; i++) {
        unsigned next = (m->usart1.rx_stage_tail + 1) % CH32_USART1_RXFIFO_DEPTH;
        if (next == m->usart1.rx_stage_head) {
            break;  /* stage 满，丢弃 */
        }
        m->usart1.rx_stage_buf[m->usart1.rx_stage_tail] = buf[i];
        m->usart1.rx_stage_tail = next;
    }
    /* 如果 rx_timer 还没在跑，立即启动第一次投喂 */
    if (!timer_pending(m->usart1.rx_timer)) {
        timer_mod(m->usart1.rx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

/* =========================================================
 * 唤醒 RX backend（FIFO 有空间时）
 * ========================================================= */
static void ch32_usart1_try_accept_rx(Ch32MachineState *m)
{
    if (qemu_chr_fe_backend_connected(&m->usart1.chr) &&
        ch32_usart1_rx_enabled(m) && !ch32_rxfifo_full(m)) {
        qemu_chr_fe_accept_input(&m->usart1.chr);
    }
}

/* =========================================================
 * DMA 辅助工具：向 DMA 内存目标地址写入一个字节
 * ========================================================= */
static void ch32_dma_write_byte(Ch32DmaChannel *ch, uint8_t byte)
{
    AddressSpace *as = &address_space_memory;
    uint8_t  buf8  = byte;
    uint16_t buf16 = byte;
    uint32_t buf32 = byte;
    unsigned msize = (ch->cfgr >> 10) & 3u;  /* MSIZE 字段 */

    switch (msize) {
    case 1:
        address_space_write(as, ch->maddr, MEMTXATTRS_UNSPECIFIED, &buf16, 2);
        if (ch->cfgr & (1u << 7)) { ch->maddr += 2; }  /* MINC */
        break;
    case 2:
        address_space_write(as, ch->maddr, MEMTXATTRS_UNSPECIFIED, &buf32, 4);
        if (ch->cfgr & (1u << 7)) { ch->maddr += 4; }  /* MINC */
        break;
    default: /* MSIZE=00: byte */
        address_space_write(as, ch->maddr, MEMTXATTRS_UNSPECIFIED, &buf8, 1);
        if (ch->cfgr & (1u << 7)) { ch->maddr += 1; }  /* MINC */
        break;
    }
}

/* =========================================================
 * ch32_dma_finish_channel - DMA 通道传输完成公共逻辑
 *
 * 置位 INTFR（TCIF+GIF）、清 EN、如果 TCIE=1 触发 IRQ。
 * intfr_reg  : 指向对应 DMA 的 INTFR 寄存器
 * ch_local   : 该通道在其 DMA 控制器内的编号（1-based）
 * irq_n      : 该通道对应的 PFIC IRQ 号
 * ========================================================= */
static void ch32_dma_finish_channel(Ch32MachineState *m, Ch32DmaChannel *ch,
                                    uint32_t *intfr_reg, unsigned ch_local,
                                    unsigned irq_n)
{
    unsigned gif_bit  = ((ch_local - 1u) * 4u);
    unsigned tcif_bit = ((ch_local - 1u) * 4u + 1u);

    ch->cntr = 0;
    ch->cfgr &= ~(1u << 0);  /* 清 EN */
    *intfr_reg |= (1u << gif_bit) | (1u << tcif_bit);

    if (ch->cfgr & (1u << 1)) {  /* TCIE */
        /* 复用 ch32_dma.c 中的 ch32_dma_fire_irq 需要外部调用——
         * 此处直接内联实现同样的逻辑 */
        RISCVCPU *cpu = &m->cpus.harts[0];
        CPURISCVState *env = &cpu->env;
        unsigned wd  = irq_n / 32u;
        unsigned bit = irq_n % 32u;
        bool ienr_on = (m->pfic_ienr[wd] >> bit) & 1u;

        BQL_LOCK_GUARD();
        if (ienr_on && env->wch_evt_mcause_override == 0) {
            env->mie |= MIP_MEIP;
            env->wch_evt_mcause_override = irq_n;
            riscv_cpu_update_mip(env, MIP_MEIP, MIP_MEIP);
            riscv_cpu_interrupt(env);
        }
    }
}

/* =========================================================
 * ch32_usart1_dma_rx_pump - USART1 RXNE 驱动 DMA RX 字节搬运
 *
 * 每当 USART1 RX FIFO 有新字节到达（rx_tick 投嗂后）即调用。
 * 若 DMAR=1 且 DMA1_Ch5 的 PADDR 指向 USART1 DATAR，则仿真 RXNE 驱动
 * DMA 搬运一个字节到内存（不再经过 USART IRQ 路径）。
 * ========================================================= */
void ch32_usart1_dma_rx_pump(Ch32MachineState *m)
{
    Ch32DmaChannel *ch;
    uint32_t datar_base;
    uint8_t byte;

    /* 检查 DMAR 位 */
    if (!(m->usart1.ctlr3 & CTLR3_DMAR)) {
        return;
    }
    /* DMA1_Ch5 索引 = CH32_USART1_DMA_RX_CH - 1 = 4 */
    ch = &m->dma.ch1[CH32_USART1_DMA_RX_CH - 1];

    if (!(ch->cfgr & (1u << 0))) {
        return;  /* EN 未置 */
    }
    if (ch->cntr == 0) {
        return;  /* 传输计数已退，无需搞运 */
    }
    /*
     * 验证 PADDR 是否指向 USART1 DATAR（限制在 USART1 地址范围内）。
     * 允许偏移 0~3，匹配 1/2/4 字节对齐讻取 DATAR。
     */
    datar_base = (uint32_t)CH32_USART1_BASE;
    if (ch->paddr < datar_base + 0x04u || ch->paddr > datar_base + 0x07u) {
        return;  /* PADDR 不指向 USART1 DATAR，跳过 */
    }

    /* USART1 RX FIFO 为空：无字节可搬运 */
    if (ch32_rxfifo_empty(m)) {
        return;
    }

    /* 从 RX FIFO 取出一个字节 */
    byte = m->usart1.rx_buf[m->usart1.rx_head];
    m->usart1.rx_head = (m->usart1.rx_head + 1) % CH32_USART1_RXFIFO_DEPTH;
    m->usart1.rx_pending = !ch32_rxfifo_empty(m);
    if (m->usart1.rx_pending) {
        m->usart1.rdr = m->usart1.rx_buf[m->usart1.rx_head];
    }
    /* 清 RXNE 模拟硬件被 DMA 读走 */
    m->usart1.statr &= (uint16_t)~STATR_RXNE;

    /* 将字节写入内存目标 */
    ch32_dma_write_byte(ch, byte);
    ch->cntr--;

    /* 若传输计数归零：完成传输 */
    if (ch->cntr == 0) {
        ch32_dma_finish_channel(m, ch, &m->dma.intfr[0],
                                CH32_USART1_DMA_RX_CH, CH32_USART1_DMA_RX_IRQ);
    }
}

/* =========================================================
 * RX 节拍定时器回调：每次投嗂一个字节到 rx_buf
 * ========================================================= */
static void ch32_usart1_rx_tick(void *opaque)
{
    Ch32MachineState *m = opaque;
    uint8_t ch;

    /* stage 空：无事可做 */
    if (m->usart1.rx_stage_head == m->usart1.rx_stage_tail) {
        return;
    }
    /* rx_buf 满：稍后重试 */
    if (ch32_rxfifo_full(m)) {
        timer_mod(m->usart1.rx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CH32_USART1_RX_BYTE_NS);
        return;
    }

    /* 取出一个字节 */
    ch = m->usart1.rx_stage_buf[m->usart1.rx_stage_head];
    m->usart1.rx_stage_head = (m->usart1.rx_stage_head + 1) % CH32_USART1_RXFIFO_DEPTH;

    /* 写入 rx_buf */
    m->usart1.rx_buf[m->usart1.rx_tail] = ch;
    m->usart1.rx_tail = (m->usart1.rx_tail + 1) % CH32_USART1_RXFIFO_DEPTH;
    m->usart1.rx_pending = true;
    m->usart1.rdr = m->usart1.rx_buf[m->usart1.rx_head];

    /* 触发 IRQ 或 DMA RX 握手
     *
     * 如果 DMAR=1 且 DMA1_Ch5 已配置，由 DMA pump 搞运字节（跳过 USART IRQ）；
     * 否则按常规触发 USART1 RXNE 中断。
     */
    if (m->usart1.ctlr3 & CTLR3_DMAR) {
        ch32_usart1_dma_rx_pump(m);
    } else {
        ch32_usart1_meip_resync(m);
    }

    /* 如果 stage 还有剩余字节，安排下一次投喂 */
    if (m->usart1.rx_stage_head != m->usart1.rx_stage_tail) {
        timer_mod(m->usart1.rx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CH32_USART1_RX_BYTE_NS);
    } else {
        /* stage 清空，通知 backend 可以再发 */
        ch32_usart1_try_accept_rx(m);
    }
}

/* =========================================================
 * MMIO 读
 * ========================================================= */
static uint64_t ch32_usart1_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;

    switch (addr) {
    case 0x00: /* STATR */
        {
            uint16_t st = ch32_usart1_statr_hw(m);
            return (size == 1) ? (st & 0xffu) : st;
        }

    case 0x04: /* DATAR 读 = RDR */
        {
            /*
             * 读 DATAR：弹出 RX FIFO 头字节，清 RXNE（当 FIFO 变空时），
             * 若 ORE=1 则同时清 ORE（读 STATR 再读 DATAR 清除序列）。
             * 若 FIFO 已空则返回 0（不改变任何状态）。
             */
            uint32_t ret = 0;

            if (m->usart1.rx_pending) {
                ret = m->usart1.rdr;
                m->usart1.rx_head = (m->usart1.rx_head + 1) % CH32_USART1_RXFIFO_DEPTH;
                m->usart1.rx_pending = !ch32_rxfifo_empty(m);
                if (m->usart1.rx_pending) {
                    m->usart1.rdr = m->usart1.rx_buf[m->usart1.rx_head];
                }
                /* 清 RXNE（已由 statr_hw 动态派生，但写 0 软件清 bit 需一致） */
                m->usart1.statr &= (uint16_t)~STATR_RXNE;
                /* 清 ORE（读 STATR+DATAR 序列清除） */
                m->usart1.statr &= (uint16_t)~STATR_ORE;
            }
            ch32_usart1_meip_resync(m);
            /* 尝试通知 backend 可以再发数据 */
            ch32_usart1_try_accept_rx(m);
            return (size == 1) ? (ret & 0xffu) : ret;
        }

    case 0x08: return m->usart1.brr;
    case 0x0c: return m->usart1.ctlr1;
    case 0x10: return m->usart1.ctlr2;
    case 0x14: return m->usart1.ctlr3;
    case 0x18: return m->usart1.gpr;
    case 0x1c: return m->usart1.ctlr4;
    default:   break;
    }
    ch32_mmio_log_bad_offset("usart1", addr, size, false);
    return 0;
}

/* =========================================================
 * MMIO 写
 * ========================================================= */
static void ch32_usart1_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v = (uint32_t)val;

    (void)size;

    switch (addr) {
    case 0x00: /* STATR 写：只允许对 RW0 位写 0 清标志 */
        /*
         * 手册：TC/RXNE/LBD/CTS 为 RW0，软件写 0 可清。
         * 其余位（TXE/IDLE/ORE 等）为 RO，写操作被忽略。
         * 实现：保留 statr 中非 RW0 的位不变，对 RW0 位按写入值清零。
         */
        {
            uint16_t clr = (uint16_t)(~v & STATR_W0C_MASK); /* 写 0 = 清 */
            m->usart1.statr &= (uint16_t)~clr;
            ch32_usart1_meip_resync(m);
        }
        return;

    case 0x04: /* DATAR 写 = TDR */
        {
            /*
             * 固件写 DATAR：
             *   1. 将字节放入 TX FIFO。
             *   2. TXE 清（FIFO 非空）；TC 清（尚未发完）。
             *   3. 如果 UE=0（宽松模式），直接同步写入 backend（不过 FIFO）。
             *   4. 启动 tx_timer 触发异步 drain。
             */
            bool ue = (m->usart1.ctlr1 & CTLR1_UE) != 0;
            uint8_t ch = (uint8_t)(v & 0xffu);

            if (!ue) {
                /* 宽松模式：直接写 backend，保持 TXE|TC=1 */
                (void)qemu_chr_fe_write_all(&m->usart1.chr, &ch, 1);
                /* TXE/TC 保持 1（由 statr_hw 宽松逻辑保证） */
                ch32_usart1_meip_resync(m);
                return;
            }

            if (!ch32_txfifo_full(m)) {
                m->usart1.tx_buf[m->usart1.tx_tail] = ch;
                m->usart1.tx_tail = (m->usart1.tx_tail + 1) % CH32_USART1_TXFIFO_DEPTH;
                m->usart1.tx_empty    = false;
                m->usart1.tx_complete = false;
            }
            /* 清 STATR.TC（软件写 DATAR 清，参见手册表 18-1） */
            m->usart1.statr &= (uint16_t)~STATR_TC;

            /* 启动 drain 定时器（若未 arm） */
            if (!timer_pending(m->usart1.tx_timer)) {
                timer_mod(m->usart1.tx_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          CH32_USART1_TX_DRAIN_NS);
            }
            ch32_usart1_meip_resync(m);
        }
        return;

    case 0x08:
        m->usart1.brr = v;
        return;

    case 0x0c: /* CTLR1 */
        m->usart1.ctlr1 = v;
        /*
         * UE/RE/TE/RXNEIE/TXEIE/TCIE 可能变化。
         * 若 TE 变为使能，从 tx_empty/tc 状态触发 TXEIE/TCIE。
         * 重新 accept RX input 并同步中断。
         */
        ch32_usart1_try_accept_rx(m);
        ch32_usart1_meip_resync(m);
        return;

    case 0x10:
        m->usart1.ctlr2 = v;
        return;

    case 0x14:
        m->usart1.ctlr3 = v;
        return;

    case 0x18:
        m->usart1.gpr = v;
        return;

    case 0x1c:
        m->usart1.ctlr4 = v;
        return;

    default:
        ch32_mmio_log_bad_offset("usart1", addr, size, true);
        return;
    }
}

/* =========================================================
 * MemoryRegion ops
 * ========================================================= */
static const MemoryRegionOps ch32_usart1_ops = {
    .read  = ch32_usart1_read,
    .write = ch32_usart1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* =========================================================
 * ch32_usart1_reset
 * ========================================================= */
void ch32_usart1_reset(Ch32MachineState *m)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;

    /* 手册复位值：STATR=0x00C0（TXE=1,TC=1），其余=0 */
    m->usart1.statr    = (uint16_t)(STATR_TXE | STATR_TC);
    m->usart1.brr      = 0;
    m->usart1.ctlr1    = 0;
    m->usart1.ctlr2    = 0;
    m->usart1.ctlr3    = 0;
    m->usart1.gpr      = 0;
    m->usart1.ctlr4    = 0;

    /* RX FIFO */
    m->usart1.rx_head    = 0;
    m->usart1.rx_tail    = 0;
    m->usart1.rx_pending = false;
    m->usart1.rdr        = 0;
    memset(m->usart1.rx_buf, 0, sizeof(m->usart1.rx_buf));

    /* RX stage 缓冲 */
    m->usart1.rx_stage_head = 0;
    m->usart1.rx_stage_tail = 0;
    memset(m->usart1.rx_stage_buf, 0, sizeof(m->usart1.rx_stage_buf));

    /* 停止 RX 节拍定时器 */
    if (m->usart1.rx_timer) {
        timer_del(m->usart1.rx_timer);
    }

    /* TX FIFO */
    m->usart1.tx_head     = 0;
    m->usart1.tx_tail     = 0;
    m->usart1.tx_empty    = true;
    m->usart1.tx_complete = true;
    memset(m->usart1.tx_buf, 0, sizeof(m->usart1.tx_buf));

    /* 停止 TX drain 定时器 */
    if (m->usart1.tx_timer) {
        timer_del(m->usart1.tx_timer);
    }

    /*
     * 撤销挂起的 USART1 MEIP。
     * reset 路径可能已持有 BQL，不能调用带 BQL_LOCK_GUARD 的 meip_resync。
     */
    if (env->wch_evt_mcause_override == CH32_USART1_NVIC_IRQ) {
        riscv_cpu_update_mip(env, MIP_MEIP, 0);
        env->wch_evt_mcause_override = 0;
        env->mie &= ~MIP_MEIP;
        ch32_pfic_irq_active_clear(m, CH32_USART1_NVIC_IRQ);
        riscv_cpu_interrupt(env);
    }
}

/* =========================================================
 * ch32_usart1_init_mr
 * ========================================================= */
void ch32_usart1_init_mr(Ch32MachineState *m, Object *owner)
{
    Chardev *cd = serial_hd(0);

    if (!cd) {
        error_report("ch32-usart1: no char backend (use -serial stdio or "
                     "-nographic)");
        exit(EXIT_FAILURE);
    }

    /* 初始状态：TXE=1, TC=1（手册复位值 0x00C0） */
    m->usart1.statr       = (uint16_t)(STATR_TXE | STATR_TC);
    m->usart1.tx_empty    = true;
    m->usart1.tx_complete = true;

    /* TX drain 定时器 */
    m->usart1.tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       ch32_usart1_tx_drain, m);
    /* RX 节拍定时器：逐字节投喂，模拟串口字节间隔 */
    m->usart1.rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       ch32_usart1_rx_tick, m);
    m->usart1.rx_stage_head = 0;
    m->usart1.rx_stage_tail = 0;

    memory_region_init_io(&m->usart1.iomem, owner, &ch32_usart1_ops, m,
                          "ch32-usart1", 0x400);
    qemu_chr_fe_init(&m->usart1.chr, cd, &error_fatal);
    qemu_chr_fe_set_handlers(&m->usart1.chr,
                             ch32_usart1_can_receive,
                             ch32_usart1_receive,
                             NULL, NULL, m, NULL, true);
}

/* =========================================================
 * USART2/3 + UART4~8 轻量级仿真（DMA1/DMA2 TX/RX）
 *
 * 无 char backend：TX 字节直接丢弃（仅记录 datar_last 供回路测试）。
 * RX DMA：固件写 DATAR 且 DMAR=1 时由 pump 将字节写入 DMA 指向的内存。
 * TX DMA：通用 DMA 引擎 Mem→Periph 写 DATAR；DMAT=1 由 CFGR.EN 自驱。
 *
 * UART4~8 的 DMA 请求映射：手册图 11-2 / 表 11-3（DMA2）。
 * ========================================================= */

typedef struct {
    uint8_t  dma_unit;   /* 1=DMA1，2=DMA2 */
    uint8_t  phys_rx_ch; /* 物理通道号 1~7（DMA1）或 1~11（DMA2） */
    uint32_t uart_base;
    unsigned dma_rx_irq;
} Ch32UartDmaRxCfg;

typedef struct {
    Ch32MachineState       *m;
    Ch32Usart23State       *u;
    const Ch32UartDmaRxCfg *rx_dma;
} Ch32UsartLiteOpaque;

/*
 * RX DMA 通道（手册表 11-2 DMA1；图 11-2 / 表 11-3 DMA2）：
 * USART2_RX = DMA1_Ch6；USART3_RX = DMA1_Ch3；
 * UART4_RX = DMA2_Ch3；UART5_RX = DMA2_Ch2；UART6_RX = DMA2_Ch7；
 * UART7_RX = DMA2_Ch9；UART8_RX = DMA2_Ch11。
 */
static const Ch32UartDmaRxCfg usart2_rx_dma = {
    1, 6, CH32_USART2_BASE, CH32_USART2_DMA_RX_IRQ,
};
static const Ch32UartDmaRxCfg usart3_rx_dma = {
    1, 3, CH32_USART3_BASE, CH32_USART3_DMA_RX_IRQ,
};
static const Ch32UartDmaRxCfg uart4_rx_dma = {
    2, 3, CH32_UART4_BASE, 74u,
};
static const Ch32UartDmaRxCfg uart5_rx_dma = {
    2, 2, CH32_UART5_BASE, 73u,
};
static const Ch32UartDmaRxCfg uart6_rx_dma = {
    2, 7, CH32_UART6_BASE, 99u,
};
static const Ch32UartDmaRxCfg uart7_rx_dma = {
    2, 9, CH32_UART7_BASE, 101u,
};
static const Ch32UartDmaRxCfg uart8_rx_dma = {
    2, 11, CH32_UART8_BASE, 103u,
};

static Ch32DmaChannel *ch32_usart_lite_dma_ch(Ch32MachineState *m,
                                              const Ch32UartDmaRxCfg *cfg)
{
    if (!cfg) {
        return NULL;
    }
    if (cfg->dma_unit == 1u) {
        if (cfg->phys_rx_ch < 1u || cfg->phys_rx_ch > 7u) {
            return NULL;
        }
        return &m->dma.ch1[cfg->phys_rx_ch - 1u];
    }
    if (cfg->dma_unit == 2u) {
        if (cfg->phys_rx_ch < 1u || cfg->phys_rx_ch > 11u) {
            return NULL;
        }
        return &m->dma.ch2[cfg->phys_rx_ch - 1u];
    }
    return NULL;
}

static void ch32_usart_lite_dma_rx_finish(Ch32MachineState *m, Ch32DmaChannel *ch,
                                          const Ch32UartDmaRxCfg *cfg)
{
    uint32_t *intfr_reg;
    unsigned ch_local_bm;

    if (cfg->dma_unit == 1u) {
        intfr_reg = &m->dma.intfr[0];
        ch_local_bm = cfg->phys_rx_ch;
    } else {
        if (cfg->phys_rx_ch <= 7u) {
            intfr_reg = &m->dma.intfr[1];
            ch_local_bm = cfg->phys_rx_ch;
        } else {
            intfr_reg = &m->dma.intfr[2];
            ch_local_bm = cfg->phys_rx_ch - 7u;
        }
    }
    ch32_dma_finish_channel(m, ch, intfr_reg, ch_local_bm, cfg->dma_rx_irq);
}

/*
 * ch32_usart_lite_dma_rx_pump — RX DMA 单字节搬运（DATAR 模拟源）
 */
static void ch32_usart_lite_dma_rx_pump(Ch32MachineState *m, Ch32Usart23State *u,
                                        const Ch32UartDmaRxCfg *cfg)
{
    Ch32DmaChannel *ch;
    uint32_t expected;
    uint8_t byte;

    if (!u->rx_pending || !cfg) {
        return;
    }
    ch = ch32_usart_lite_dma_ch(m, cfg);
    if (!ch) {
        return;
    }
    if (!(ch->cfgr & (1u << 0)) || ch->cntr == 0) {
        return;
    }
    expected = cfg->uart_base + 0x04u;
    if (ch->paddr < expected || ch->paddr > expected + 3u) {
        return;
    }

    byte = u->datar_last;
    u->rx_pending = false;

    ch32_dma_write_byte(ch, byte);
    ch->cntr--;

    if (ch->cntr == 0) {
        ch32_usart_lite_dma_rx_finish(m, ch, cfg);
    }
}

/* ---------- RX 小 FIFO（中断路径）；DMAR=1 时仍走 datar_last + rx_pending ---------- */

static bool lite_rxfifo_empty(const Ch32Usart23State *u)
{
    return u->rx_fifo_h == u->rx_fifo_t;
}

static bool lite_rxfifo_full(const Ch32Usart23State *u)
{
    return ((u->rx_fifo_t + 1u) % CH32_USART_LITE_RXFIFO_DEPTH) == u->rx_fifo_h;
}

static void lite_rxfifo_push(Ch32Usart23State *u, uint8_t b)
{
    unsigned next = (u->rx_fifo_t + 1u) % CH32_USART_LITE_RXFIFO_DEPTH;

    u->rx_fifo[u->rx_fifo_t] = b;
    u->rx_fifo_t = next;
}

static uint8_t lite_rxfifo_pop(Ch32Usart23State *u)
{
    uint8_t b = u->rx_fifo[u->rx_fifo_h];

    u->rx_fifo_h = (u->rx_fifo_h + 1u) % CH32_USART_LITE_RXFIFO_DEPTH;
    return b;
}

static uint16_t ch32_usart_lite_statr_hw(const Ch32Usart23State *u)
{
    /*
     * 轻量 TX：无移位寄存器时序，TXE|TC 恒 1（宽松）。
     * 为避免 TXEIE/TCIE 恒真导致 MEIP 风暴，USART2~UART8 的 PFIC 同步**仅考察 RXNE**。
     * RXNE 仅反映 RX FIFO（外部 push 或后续扩展）；DMAR 字节仍走 DMA，不占 RXNE。
     */
    uint16_t st = (uint16_t)(STATR_TXE | STATR_TC);

    if (!lite_rxfifo_empty(u)) {
        st |= STATR_RXNE;
    }
    return st;
}

/*
 * usart_lite_meip_one — 单实例 PFIC/MEIP（逻辑对齐 ch32_usart1_meip_resync，IRQ 号各异）。
 */
static void usart_lite_meip_one(Ch32MachineState *m, Ch32Usart23State *u,
                                unsigned irq_n)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;
    unsigned wd  = irq_n / 32u;
    unsigned bit = irq_n % 32u;
    bool ienr_on = (m->pfic_ienr[wd] >> bit) & 1u;

    uint16_t st = ch32_usart_lite_statr_hw(u);
    uint32_t c1 = u->ctlr1;

    bool irq = ienr_on && ((c1 & CTLR1_RXNEIE) && (st & STATR_RXNE));

    BQL_LOCK_GUARD();

    if (irq) {
        if (env->wch_evt_mcause_override != 0u &&
            env->wch_evt_mcause_override != irq_n) {
            return;
        }
        env->mie |= MIP_MEIP;
        env->wch_evt_mcause_override = irq_n;
        ch32_pfic_irq_active_set(m, irq_n);
        riscv_cpu_update_mip(env, MIP_MEIP, MIP_MEIP);
    } else {
        if (env->wch_evt_mcause_override == irq_n) {
            if (env->wch_vtf_in_isr > 0) {
                riscv_cpu_update_mip(env, MIP_MEIP, 0);
            } else {
                riscv_cpu_update_mip(env, MIP_MEIP, 0);
                env->wch_evt_mcause_override = 0;
                ch32_pfic_irq_active_clear(m, irq_n);
                ch32_pfic_resync_pending_meip(m);
            }
        }
    }
    riscv_cpu_interrupt(env);
}

void ch32_usart_lite_meip_resync(Ch32MachineState *m)
{
    static const unsigned lite_irq[7] = {
        CH32_USART2_NVIC_IRQ, CH32_USART3_NVIC_IRQ,
        CH32_UART4_NVIC_IRQ, CH32_UART5_NVIC_IRQ,
        CH32_UART6_NVIC_IRQ, CH32_UART7_NVIC_IRQ, CH32_UART8_NVIC_IRQ,
    };
    Ch32Usart23State *units[7];
    unsigned i;

    units[0] = &m->usart2;
    units[1] = &m->usart3;
    units[2] = &m->usart4;
    units[3] = &m->usart5;
    units[4] = &m->usart6;
    units[5] = &m->usart7;
    units[6] = &m->usart8;

    for (i = 0; i < 7u; i++) {
        usart_lite_meip_one(m, units[i], lite_irq[i]);
    }
}

void ch32_usart_lite_push_rx_byte(Ch32MachineState *m, unsigned idx, uint8_t b)
{
    Ch32Usart23State *u;

    switch (idx) {
    case 0: u = &m->usart2; break;
    case 1: u = &m->usart3; break;
    case 2: u = &m->usart4; break;
    case 3: u = &m->usart5; break;
    case 4: u = &m->usart6; break;
    case 5: u = &m->usart7; break;
    case 6: u = &m->usart8; break;
    default: return;
    }
    if (lite_rxfifo_full(u)) {
        return;
    }
    lite_rxfifo_push(u, b);
    ch32_usart_lite_meip_resync(m);
}

static uint64_t ch32_usart_lite_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32UsartLiteOpaque *op = opaque;
    Ch32MachineState *m = op->m;
    Ch32Usart23State *u = op->u;

    switch (addr) {
    case 0x00: /* STATR */
        {
            uint16_t st = ch32_usart_lite_statr_hw(u);
            return (size == 1) ? (st & 0xffu) : st;
        }
    case 0x04: /* DATAR 读 RDR：先消费 RX FIFO */
        {
            uint32_t ret = (uint32_t)u->datar_last;

            if (!lite_rxfifo_empty(u)) {
                ret = lite_rxfifo_pop(u);
                u->datar_last = (uint8_t)ret;
            }
            ch32_usart_lite_meip_resync(m);
            return (size == 1) ? (ret & 0xffu) : ret;
        }
    case 0x08:
        return u->brr;
    case 0x0c:
        return u->ctlr1;
    case 0x10:
        return u->ctlr2;
    case 0x14:
        return u->ctlr3;
    case 0x18:
        return u->gpr;
    case 0x1c:
        return u->ctlr4;
    default:
        return 0;
    }
}

static void ch32_usart_lite_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    Ch32UsartLiteOpaque *op = opaque;
    Ch32MachineState *m = op->m;
    Ch32Usart23State *u = op->u;
    uint32_t v = (uint32_t)val;

    (void)size;

    switch (addr) {
    case 0x00: /* STATR：RW0 位写 0 清（与 USART1 一致，仅处理 RXNE） */
        {
            uint16_t clr = (uint16_t)(~v & STATR_W0C_MASK);

            if ((clr & STATR_RXNE) && !lite_rxfifo_empty(u)) {
                (void)lite_rxfifo_pop(u);
            }
            ch32_usart_lite_meip_resync(m);
        }
        return;
    case 0x04: /* DATAR 写 = TDR */
        u->datar_last = (uint8_t)(v & 0xffu);
        if (u->ctlr3 & CTLR3_DMAR) {
            u->rx_pending = true;
            ch32_usart_lite_dma_rx_pump(m, u, op->rx_dma);
        }
        return;
    case 0x08:
        u->brr = v;
        return;
    case 0x0c:
        u->ctlr1 = v;
        ch32_usart_lite_meip_resync(m);
        return;
    case 0x10:
        u->ctlr2 = v;
        return;
    case 0x14:
        u->ctlr3 = v;
        ch32_usart_lite_meip_resync(m);
        return;
    case 0x18:
        u->gpr = v;
        return;
    case 0x1c:
        u->ctlr4 = v;
        return;
    default:
        return;
    }
}

static const MemoryRegionOps ch32_usart_lite_ops = {
    .read  = ch32_usart_lite_read,
    .write = ch32_usart_lite_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static Ch32UsartLiteOpaque usart_lite_op[7];

static void ch32_usart_lite_one_init(Ch32MachineState *m, Object *owner,
                                     Ch32UsartLiteOpaque *op, Ch32Usart23State *u,
                                     const Ch32UartDmaRxCfg *rx_dma,
                                     const char *name)
{
    op->m = m;
    op->u = u;
    op->rx_dma = rx_dma;
    u->statr = (uint16_t)(STATR_TXE | STATR_TC);
    u->brr = 0;
    u->ctlr1 = 0;
    u->ctlr2 = 0;
    u->ctlr3 = 0;
    u->gpr = 0;
    u->ctlr4 = 0;
    u->datar_last = 0;
    u->rx_pending = false;
    u->rx_fifo_h = 0;
    u->rx_fifo_t = 0;
    memset(u->rx_fifo, 0, sizeof(u->rx_fifo));
    memory_region_init_io(&u->iomem, owner, &ch32_usart_lite_ops, op, name,
                          CH32_USART_STUB_SIZE);
}

void ch32_usart_lite_init_mr(Ch32MachineState *m, Object *owner)
{
    ch32_usart_lite_one_init(m, owner, &usart_lite_op[0], &m->usart2,
                             &usart2_rx_dma, "ch32-usart2");
    ch32_usart_lite_one_init(m, owner, &usart_lite_op[1], &m->usart3,
                             &usart3_rx_dma, "ch32-usart3");
    ch32_usart_lite_one_init(m, owner, &usart_lite_op[2], &m->usart4,
                             &uart4_rx_dma, "ch32-uart4");
    ch32_usart_lite_one_init(m, owner, &usart_lite_op[3], &m->usart5,
                             &uart5_rx_dma, "ch32-uart5");
    ch32_usart_lite_one_init(m, owner, &usart_lite_op[4], &m->usart6,
                             &uart6_rx_dma, "ch32-uart6");
    ch32_usart_lite_one_init(m, owner, &usart_lite_op[5], &m->usart7,
                             &uart7_rx_dma, "ch32-uart7");
    ch32_usart_lite_one_init(m, owner, &usart_lite_op[6], &m->usart8,
                             &uart8_rx_dma, "ch32-uart8");
}

void ch32_usart_lite_reset(Ch32MachineState *m)
{
    Ch32Usart23State *units[] = {
        &m->usart2, &m->usart3, &m->usart4, &m->usart5,
        &m->usart6, &m->usart7, &m->usart8,
    };
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(units); i++) {
        Ch32Usart23State *u = units[i];

        u->statr = (uint16_t)(STATR_TXE | STATR_TC);
        u->brr = 0;
        u->ctlr1 = 0;
        u->ctlr2 = 0;
        u->ctlr3 = 0;
        u->gpr = 0;
        u->ctlr4 = 0;
        u->datar_last = 0;
        u->rx_pending = false;
        u->rx_fifo_h = 0;
        u->rx_fifo_t = 0;
        memset(u->rx_fifo, 0, sizeof(u->rx_fifo));
    }
}
