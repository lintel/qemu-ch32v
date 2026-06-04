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
 * File:     ch32-dma.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 通用 DMA 控制器（DMA1 / DMA2 / DMA2_EXTEN）模拟。
 *
 *     寄存器布局（AHBPERIPH_BASE = 0x40020000，即 CH32_DMA1_BLOCK_BASE）：
 *
 *       +0x000        DMA1  INTFR  / INTFCR    全局中断状态 / 清除
 *       +0x008~+0x090 DMA1_Ch1~7  各 0x14 字节（CFGR/CNTR/PADDR/MADDR + 4字节填充）
 *       +0x400        DMA2  INTFR  / INTFCR
 *       +0x408~+0x480 DMA2_Ch1~7  各 0x14 字节
 *       +0x490~+0x4C0 DMA2_Ch8~11 各 0x10 字节（无填充）
 *       +0x4D0        DMA2_EXTEN  INTFR / INTFCR  Ch8~11 全局状态
 *
 *     实现策略：软件同步执行（写 CFGR.EN=1 时立即完成整个传输），
 *     模拟真实 DMA 控制器行为：更新 INTFR 标志位，TCIE=1 时触发 IRQ。
 */

#include "ch32-machine-internal.h"
#include "qemu/log.h"

/* =====================================================================
 * CFGR 位定义
 * ===================================================================== */
#define DMA_CFGR_EN      (1u <<  0)  /* 通道使能，写1触发传输 */
#define DMA_CFGR_TCIE    (1u <<  1)  /* 传输完成中断使能 */
#define DMA_CFGR_HTIE    (1u <<  2)  /* 半传输中断使能 */
#define DMA_CFGR_TEIE    (1u <<  3)  /* 传输错误中断使能 */
#define DMA_CFGR_DIR     (1u <<  4)  /* 方向：0=Periph→Mem, 1=Mem→Periph */
#define DMA_CFGR_CIRC    (1u <<  5)  /* 循环模式（不支持，忽略） */
#define DMA_CFGR_PINC    (1u <<  6)  /* 外设地址自增 */
#define DMA_CFGR_MINC    (1u <<  7)  /* 内存地址自增 */
#define DMA_CFGR_PSIZE   (3u <<  8)  /* 外设数据宽度 */
#define DMA_CFGR_MSIZE   (3u << 10)  /* 内存数据宽度 */
#define DMA_CFGR_PL      (3u << 12)  /* 优先级（忽略） */
#define DMA_CFGR_MEM2MEM (1u << 14)  /* 内存到内存 */

/* 数据宽度：PSIZE/MSIZE 字段值 */
#define DMA_SIZE_BYTE    0u
#define DMA_SIZE_HWORD   1u
#define DMA_SIZE_WORD    2u

/* INTFR 每通道 4 bit：[GIF, TCIF, HTIF, TEIF] */
#define DMA_INTFR_GIF_BIT(ch)   (((ch) - 1) * 4)
#define DMA_INTFR_TCIF_BIT(ch)  (((ch) - 1) * 4 + 1)
#define DMA_INTFR_HTIF_BIT(ch)  (((ch) - 1) * 4 + 2)
#define DMA_INTFR_TEIF_BIT(ch)  (((ch) - 1) * 4 + 3)

/* =====================================================================
 * 内存访问封装：任一通道读/写异常（固件配错 paddr/maddr
 * 指向非法区域）时，真机 DMA 会置 INTFR.TEIF 并在 TEIE 使能时
 * 产生传输错误中断。之前 QEMU 侧全部忽略 MemTxResult，导致固件
 * bug 默默失败、无任何反馈。现按真机语义上报。
 * 调用点位于 ch32_dma_exec 内部 switch/case，失败时通过 goto teif
 * 跳到错误处理分支（在循环外）。
 * ===================================================================== */
#define DMA_MEM_RD(_addr, _buf, _sz)                                          \
    do {                                                                      \
        if (address_space_read(as, (_addr), MEMTXATTRS_UNSPECIFIED,           \
                               (_buf), (_sz)) != MEMTX_OK) {                  \
            goto teif;                                                        \
        }                                                                     \
    } while (0)

#define DMA_MEM_WR(_addr, _buf, _sz)                                          \
    do {                                                                      \
        if (address_space_write(as, (_addr), MEMTXATTRS_UNSPECIFIED,          \
                                (_buf), (_sz)) != MEMTX_OK) {                 \
            goto teif;                                                        \
        }                                                                     \
    } while (0)

/* =====================================================================
 * 地址偏移常量（相对于 CH32_DMA1_BLOCK_BASE）
 * ===================================================================== */
/* DMA1 全局寄存器 */
#define DMA1_INTFR_OFF   0x000u
#define DMA1_INTFCR_OFF  0x004u
/* DMA1 通道基地址：Ch(n) = 0x008 + (n-1)*0x14，n=1..7 */
#define DMA1_CH_BASE(n)  (0x008u + ((n) - 1) * 0x14u)

/* DMA2 全局寄存器 */
#define DMA2_INTFR_OFF   0x400u
#define DMA2_INTFCR_OFF  0x404u
/* DMA2 Ch1~7：0x408 + (n-1)*0x14 */
#define DMA2_CH17_BASE(n) (0x408u + ((n) - 1) * 0x14u)
/* DMA2 Ch8~11：0x490 + (n-8)*0x10 */
#define DMA2_CH811_BASE(n) (0x490u + ((n) - 8) * 0x10u)
/* DMA2_EXTEN 全局寄存器 */
#define DMA2_EXTEN_INTFR_OFF  0x4D0u
#define DMA2_EXTEN_INTFCR_OFF 0x4D4u

/* =====================================================================
 * 每通道 CFGR 偏移（相对于通道基地址）
 * ===================================================================== */
#define CH_CFGR_OFF  0u
#define CH_CNTR_OFF  4u
#define CH_PADDR_OFF 8u
#define CH_MADDR_OFF 12u

/* =====================================================================
 * IRQ 号（CH32V307/317，D8C 变体）
 * DMA1 Ch1~7  : IRQ 27~33  -> pfic_ienr[0] bit27~31, pfic_ienr[1] bit0~1
 * DMA2 Ch1~5  : IRQ 72~76  -> pfic_ienr[2] bit8~12
 * DMA2 Ch6~7  : IRQ 98~99  -> pfic_ienr[3] bit2~3
 * DMA2 Ch8~11 : IRQ 100~103-> pfic_ienr[3] bit4~7
 * ===================================================================== */
static const uint8_t dma1_ch_irq[7] = { 27, 28, 29, 30, 31, 32, 33 };
static const uint8_t dma2_ch_irq[11] = {
    72, 73, 74, 75, 76,   /* Ch1~5 */
    98, 99,               /* Ch6~7 */
    100, 101, 102, 103    /* Ch8~11 */
};

/* =====================================================================
 * IRQ 触发：模仿 ch32_usart1_meip_resync 模式
 *
 * 注意：DMA 中断为一次性（写 EN=1 同步完成后 EN 自动清 0），不存在持续
 * pending 状态，因此不需要 irq_active 防重入标志。但 wch_evt_mcause_override
 * 是全局唯一的路由槽，必须仅在当前无其他外设占用时才覆写，否则会破坏
 * ETH/USB/USART 等持有 MEIP 的外设的中断路由。
 * ===================================================================== */
static void ch32_dma_fire_irq(Ch32MachineState *m, unsigned irq_n)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;
    unsigned wd  = irq_n / 32u;
    unsigned bit = irq_n % 32u;
    bool ienr_on = (m->pfic_ienr[wd] >> bit) & 1u;

    BQL_LOCK_GUARD();
    if (!ienr_on) {
        return;
    }
    /*
     * 注入槽被其他外设（ETH/USB/USART）占用时：
     * 直接挂软件 pending 到 pfic_ipr，由 ch32_pfic_resync_software_pending
     * （mret 后执行）或 ch32_dma_meip_resync（IENR 写入 / MEIP owner 释放时
     * 由 pfic_resync_table 调度）重新尝试注入。
     *
     * 注意：不能直接覆写 override 槽，否则会破坏现任拥有者的路由状态
     * （曾出现过 DMA 抢占导致 ETH 中断失踪的回归）。
     */
    if (env->wch_evt_mcause_override != 0) {
        m->pfic_ipr[wd] |= (1u << bit);
        return;
    }
    env->mie |= MIP_MEIP;
    env->wch_evt_mcause_override = irq_n;
    riscv_cpu_update_mip(env, MIP_MEIP, MIP_MEIP);
    riscv_cpu_interrupt(env);
}

/* =====================================================================
 * ch32_dma_meip_resync - 重新评估所有 DMA 通道的中断状态
 *
 * 由 pfic_resync_table 调度，触发时机：
 *   1. 固件写 PFIC IENR/IRER 使能 DMA IRQ 后，而此时 DMA 可能已有
 *      INTFR.TCIF 置位但 fire_irq 早已在 IENR=0 时被跟——与 upstream meip
 *      释放的 ch32_pfic_resync_pending_meip 一同调用
 *   2. ch32_pfic_resync_pending_meip 被调用时 override 槽释放，之前被
 *      挂在 pfic_ipr 的 DMA 软件 pending 需要重新注入
 *
 * 策略：遍历 DMA1 ch1[] / DMA2 ch2[]，对 (CFGR.TCIE && INTFR.TCIF) 的通道
 * 重新调用 ch32_dma_fire_irq，fire 函数自身会检查 IENR 与 override 槽。
 * ===================================================================== */
void ch32_dma_meip_resync(Ch32MachineState *m)
{
    unsigned i;

    /* DMA1 Ch1~7 -> intfr[0], IRQ dma1_ch_irq[0..6] */
    for (i = 0; i < CH32_DMA1_NUM_CH; i++) {
        const Ch32DmaChannel *ch = &m->dma.ch1[i];
        unsigned tcif_bit = DMA_INTFR_TCIF_BIT(i + 1);

        if ((ch->cfgr & DMA_CFGR_TCIE) &&
            (m->dma.intfr[0] & (1u << tcif_bit))) {
            ch32_dma_fire_irq(m, dma1_ch_irq[i]);
        }
    }
    /* DMA2 Ch1~7 -> intfr[1], IRQ dma2_ch_irq[0..4] + dma2_ch_irq[5..6] */
    for (i = 0; i < 7u; i++) {
        const Ch32DmaChannel *ch = &m->dma.ch2[i];
        unsigned tcif_bit = DMA_INTFR_TCIF_BIT(i + 1);

        if ((ch->cfgr & DMA_CFGR_TCIE) &&
            (m->dma.intfr[1] & (1u << tcif_bit))) {
            ch32_dma_fire_irq(m, dma2_ch_irq[i]);
        }
    }
    /* DMA2 Ch8~11 -> intfr[2], IRQ dma2_ch_irq[7..10], ch_local 1~4 */
    for (i = 7u; i < CH32_DMA2_NUM_CH; i++) {
        const Ch32DmaChannel *ch = &m->dma.ch2[i];
        unsigned ch_local = i - 6u; /* 7->1, 8->2, 9->3, 10->4 */
        unsigned tcif_bit = DMA_INTFR_TCIF_BIT(ch_local);

        if ((ch->cfgr & DMA_CFGR_TCIE) &&
            (m->dma.intfr[2] & (1u << tcif_bit))) {
            ch32_dma_fire_irq(m, dma2_ch_irq[i]);
        }
    }
}

/* =====================================================================
 * 核心传输执行函数
 *
 * 当固件向通道 CFGR 写入 EN=1 时立即调用，同步完成整个传输：
 *   1. 读取 CFGR/CNTR/PADDR/MADDR
 *   2. 按数据宽度循环搬运 guest 物理内存
 *   3. Normal mode：传输完成后 CNTR=0，清 EN
 *   4. 更新 INTFR TC/GIF 标志位
 *   5. 若 TCIE=1 触发对应 IRQ
 * ===================================================================== */
static unsigned dma_unit_size(unsigned size_field)
{
    switch (size_field & 3u) {
    case DMA_SIZE_HWORD: return 2u;
    case DMA_SIZE_WORD:  return 4u;
    default:             return 1u;
    }
}

static void ch32_dma_exec(Ch32MachineState *m, Ch32DmaChannel *ch,
                           uint32_t *intfr_reg, unsigned ch_local,
                           unsigned irq_n)
{
    /*
     * ch_local：通道在其 DMA 控制器内的局部编号（1~7 for DMA1/DMA2，1~4 for DMA2_EXTEN）
     * intfr_reg：指向对应 DMA 的 INTFR 寄存器（m->dma.intfr[0/1/2]）
     */
    uint32_t cfgr  = ch->cfgr;
    uint32_t cnt   = ch->cntr & 0xFFFFu;
    uint32_t paddr = ch->paddr;
    uint32_t maddr = ch->maddr;
    bool dir       = (cfgr & DMA_CFGR_DIR) != 0;   /* true = Mem→Periph */
    bool pinc      = (cfgr & DMA_CFGR_PINC) != 0;
    bool minc      = (cfgr & DMA_CFGR_MINC) != 0;
    unsigned psize = dma_unit_size((cfgr >> 8) & 3u);
    unsigned msize = dma_unit_size((cfgr >> 10) & 3u);
    AddressSpace *as = &address_space_memory;

    if (cnt == 0) {
        goto done;
    }

    for (uint32_t i = 0; i < cnt; i++) {
        uint8_t  buf8 = 0;
        uint16_t buf16 = 0;
        uint32_t buf32 = 0;

        if (!dir) {
            /* Periph → Mem（或 M2M src=paddr, dst=maddr） */
            switch (psize) {
            case 1:
                DMA_MEM_RD(paddr, &buf8, 1);
                switch (msize) {
                case 1: DMA_MEM_WR(maddr, &buf8, 1); break;
                case 2: buf16 = buf8; DMA_MEM_WR(maddr, &buf16, 2); break;
                case 4: buf32 = buf8; DMA_MEM_WR(maddr, &buf32, 4); break;
                }
                break;
            case 2:
                DMA_MEM_RD(paddr, &buf16, 2);
                switch (msize) {
                case 1: buf8 = (uint8_t)buf16; DMA_MEM_WR(maddr, &buf8, 1); break;
                case 2: DMA_MEM_WR(maddr, &buf16, 2); break;
                case 4: buf32 = buf16; DMA_MEM_WR(maddr, &buf32, 4); break;
                }
                break;
            case 4:
                DMA_MEM_RD(paddr, &buf32, 4);
                switch (msize) {
                case 1: buf8 = (uint8_t)buf32; DMA_MEM_WR(maddr, &buf8, 1); break;
                case 2: buf16 = (uint16_t)buf32; DMA_MEM_WR(maddr, &buf16, 2); break;
                case 4: DMA_MEM_WR(maddr, &buf32, 4); break;
                }
                break;
            }
        } else {
            /* Mem → Periph */
            switch (msize) {
            case 1:
                DMA_MEM_RD(maddr, &buf8, 1);
                switch (psize) {
                case 1: DMA_MEM_WR(paddr, &buf8, 1); break;
                case 2: buf16 = buf8; DMA_MEM_WR(paddr, &buf16, 2); break;
                case 4: buf32 = buf8; DMA_MEM_WR(paddr, &buf32, 4); break;
                }
                break;
            case 2:
                DMA_MEM_RD(maddr, &buf16, 2);
                switch (psize) {
                case 1: buf8 = (uint8_t)buf16; DMA_MEM_WR(paddr, &buf8, 1); break;
                case 2: DMA_MEM_WR(paddr, &buf16, 2); break;
                case 4: buf32 = buf16; DMA_MEM_WR(paddr, &buf32, 4); break;
                }
                break;
            case 4:
                DMA_MEM_RD(maddr, &buf32, 4);
                switch (psize) {
                case 1: buf8 = (uint8_t)buf32; DMA_MEM_WR(paddr, &buf8, 1); break;
                case 2: buf16 = (uint16_t)buf32; DMA_MEM_WR(paddr, &buf16, 2); break;
                case 4: DMA_MEM_WR(paddr, &buf32, 4); break;
                }
                break;
            }
        }

        if (pinc) { paddr += psize; }
        if (minc) { maddr += msize; }
    }

done:
    {
        unsigned gif_bit;
        unsigned tcif_bit;

        /* Normal mode：传输完成，CNTR→0，清 EN */
        ch->cntr = 0;
        ch->cfgr &= ~DMA_CFGR_EN;

        /* 更新 INTFR：置 GIF + TCIF */
        gif_bit  = DMA_INTFR_GIF_BIT(ch_local);
        tcif_bit = DMA_INTFR_TCIF_BIT(ch_local);
        *intfr_reg |= (1u << gif_bit) | (1u << tcif_bit);
    }

    /* 若 TCIE=1，触发对应 IRQ */
    if ((cfgr & DMA_CFGR_TCIE) && irq_n != 0) {
        ch32_dma_fire_irq(m, irq_n);
    }
    return;

teif:
    /*
     * 传输错误：真机 DMA 在总线返回错误时置 INTFR.TEIF 并
     * 在 TEIE 使能时产生传输错误中断。同时硬件会自动清
     * 通道使能位（EN=0）。
     */
    ch->cntr = 0;
    ch->cfgr &= ~DMA_CFGR_EN;
    *intfr_reg |= (1u << DMA_INTFR_GIF_BIT(ch_local))
                | (1u << DMA_INTFR_TEIF_BIT(ch_local));
    error_report("ch32-dma: ch_local=%u memory access error "
                 "(paddr=0x%08" PRIx32 " maddr=0x%08" PRIx32 " dir=%s)",
                 ch_local, paddr, maddr, dir ? "M2P" : "P2M");
    if ((cfgr & DMA_CFGR_TEIE) && irq_n != 0) {
        ch32_dma_fire_irq(m, irq_n);
    }
}

/* =====================================================================
 * MMIO read handler
 * ===================================================================== */
static uint64_t ch32_dma_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;

    /* ---------- DMA1 全局寄存器 ---------- */
    if (addr == DMA1_INTFR_OFF)  { return m->dma.intfr[0]; }
    if (addr == DMA1_INTFCR_OFF) { return 0u; }  /* 只写 */

    /* DMA1 通道寄存器：Ch1~7，每通道 0x14 字节 */
    if (addr >= 0x008u && addr < 0x008u + 7 * 0x14u) {
        unsigned idx  = (addr - 0x008u) / 0x14u;   /* 0~6 */
        unsigned roff = (addr - 0x008u) % 0x14u;
        Ch32DmaChannel *ch = &m->dma.ch1[idx];
        switch (roff) {
        case CH_CFGR_OFF:  return ch->cfgr;
        case CH_CNTR_OFF:  return ch->cntr;
        case CH_PADDR_OFF: return ch->paddr;
        case CH_MADDR_OFF: return ch->maddr;
        default: return 0;
        }
    }

    /* ---------- DMA2 全局寄存器 ---------- */
    if (addr == DMA2_INTFR_OFF)  { return m->dma.intfr[1]; }
    if (addr == DMA2_INTFCR_OFF) { return 0u; }

    /* DMA2 Ch1~7 */
    if (addr >= 0x408u && addr < 0x408u + 7 * 0x14u) {
        unsigned idx  = (addr - 0x408u) / 0x14u;
        unsigned roff = (addr - 0x408u) % 0x14u;
        Ch32DmaChannel *ch = &m->dma.ch2[idx];
        switch (roff) {
        case CH_CFGR_OFF:  return ch->cfgr;
        case CH_CNTR_OFF:  return ch->cntr;
        case CH_PADDR_OFF: return ch->paddr;
        case CH_MADDR_OFF: return ch->maddr;
        default: return 0;
        }
    }

    /* DMA2 Ch8~11 */
    if (addr >= 0x490u && addr < 0x490u + 4 * 0x10u) {
        unsigned idx  = (addr - 0x490u) / 0x10u + 7;  /* index 7~10 in ch2[] */
        unsigned roff = (addr - 0x490u) % 0x10u;
        Ch32DmaChannel *ch = &m->dma.ch2[idx];
        switch (roff) {
        case CH_CFGR_OFF:  return ch->cfgr;
        case CH_CNTR_OFF:  return ch->cntr;
        case CH_PADDR_OFF: return ch->paddr;
        case CH_MADDR_OFF: return ch->maddr;
        default: return 0;
        }
    }

    /* DMA2_EXTEN 全局寄存器 */
    if (addr == DMA2_EXTEN_INTFR_OFF)  { return m->dma.intfr[2]; }
    if (addr == DMA2_EXTEN_INTFCR_OFF) { return 0u; }

    return 0;
}

/* =====================================================================
 * MMIO write handler
 * ===================================================================== */
static void ch32_dma_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v = (uint32_t)val;

    /* ---------- DMA1 全局寄存器 ---------- */
    if (addr == DMA1_INTFR_OFF) {
        return;  /* 只读 */
    }
    if (addr == DMA1_INTFCR_OFF) {
        /* 写1清对应位 */
        m->dma.intfr[0] &= ~v;
        /*
         * 若 override 槽指向 DMA1 某通道且该通道的 TCIF 已被软件清除，
         * 则撤销 MEIP，让 override 槽归还给其他外设。
         * 检查方法：被清除的位涵盖 TCIF（每通道 bit1），且 override 的通道对应
         * TCIF_BIT 已被清。
         */
        {
            RISCVCPU *cpu0 = &m->cpus.harts[0];
            CPURISCVState *env0 = &cpu0->env;
            unsigned ov = (unsigned)env0->wch_evt_mcause_override;

            if (ov >= 27u && ov <= 33u) {
                unsigned ch_local = ov - 26u; /* ch_local 1~7 */
                unsigned tcif_bit = DMA_INTFR_TCIF_BIT(ch_local);

                BQL_LOCK_GUARD();
                if (!(m->dma.intfr[0] & (1u << tcif_bit))) {
                    riscv_cpu_update_mip(env0, MIP_MEIP, 0);
                    env0->wch_evt_mcause_override = 0;
                    riscv_cpu_interrupt(env0);
                }
            }
        }
        return;
    }

    /* DMA1 通道寄存器 */
    if (addr >= 0x008u && addr < 0x008u + 7 * 0x14u) {
        unsigned idx  = (addr - 0x008u) / 0x14u;   /* 0~6 */
        unsigned roff = (addr - 0x008u) % 0x14u;
        Ch32DmaChannel *ch = &m->dma.ch1[idx];
        switch (roff) {
        case CH_CFGR_OFF:
            ch->cfgr = v;
            if (v & DMA_CFGR_EN) {
                ch32_dma_exec(m, ch, &m->dma.intfr[0],
                              idx + 1,          /* ch_local 1~7 */
                              dma1_ch_irq[idx]);
            }
            return;
        case CH_CNTR_OFF:  ch->cntr  = v & 0xFFFFu; return;
        case CH_PADDR_OFF: ch->paddr = v; return;
        case CH_MADDR_OFF: ch->maddr = v; return;
        default: return;
        }
    }

    /* ---------- DMA2 全局寄存器 ---------- */
    if (addr == DMA2_INTFR_OFF) {
        return;
    }
    if (addr == DMA2_INTFCR_OFF) {
        m->dma.intfr[1] &= ~v;
        {
            RISCVCPU *cpu0 = &m->cpus.harts[0];
            CPURISCVState *env0 = &cpu0->env;
            unsigned ov = (unsigned)env0->wch_evt_mcause_override;

            /* DMA2 Ch1~7: IRQ 72~76; Ch6~7: IRQ 98~99 */
            if ((ov >= 72u && ov <= 76u) || ov == 98u || ov == 99u) {
                unsigned ch_local;
                unsigned tcif_bit;

                ch_local = (ov >= 72u && ov <= 76u) ? (ov - 71u) : (ov - 92u);
                tcif_bit = DMA_INTFR_TCIF_BIT(ch_local);
                BQL_LOCK_GUARD();
                if (!(m->dma.intfr[1] & (1u << tcif_bit))) {
                    riscv_cpu_update_mip(env0, MIP_MEIP, 0);
                    env0->wch_evt_mcause_override = 0;
                    riscv_cpu_interrupt(env0);
                }
            }
        }
        return;
    }

    /* DMA2 Ch1~7 */
    if (addr >= 0x408u && addr < 0x408u + 7 * 0x14u) {
        unsigned idx  = (addr - 0x408u) / 0x14u;   /* 0~6 */
        unsigned roff = (addr - 0x408u) % 0x14u;
        Ch32DmaChannel *ch = &m->dma.ch2[idx];
        switch (roff) {
        case CH_CFGR_OFF:
            ch->cfgr = v;
            if (v & DMA_CFGR_EN) {
                ch32_dma_exec(m, ch, &m->dma.intfr[1],
                              idx + 1,
                              dma2_ch_irq[idx]);
            }
            return;
        case CH_CNTR_OFF:  ch->cntr  = v & 0xFFFFu; return;
        case CH_PADDR_OFF: ch->paddr = v; return;
        case CH_MADDR_OFF: ch->maddr = v; return;
        default: return;
        }
    }

    /* DMA2 Ch8~11 */
    if (addr >= 0x490u && addr < 0x490u + 4 * 0x10u) {
        unsigned ch_n = (addr - 0x490u) / 0x10u;  /* 0~3 -> Ch8~11 */
        unsigned roff = (addr - 0x490u) % 0x10u;
        unsigned idx  = ch_n + 7;                 /* index in ch2[]: 7~10 */
        Ch32DmaChannel *ch = &m->dma.ch2[idx];
        switch (roff) {
        case CH_CFGR_OFF:
            ch->cfgr = v;
            if (v & DMA_CFGR_EN) {
                /* DMA2_EXTEN Ch8~11：ch_local 1~4，INTFR 在 intfr[2] */
                ch32_dma_exec(m, ch, &m->dma.intfr[2],
                              ch_n + 1,
                              dma2_ch_irq[idx]);
            }
            return;
        case CH_CNTR_OFF:  ch->cntr  = v & 0xFFFFu; return;
        case CH_PADDR_OFF: ch->paddr = v; return;
        case CH_MADDR_OFF: ch->maddr = v; return;
        default: return;
        }
    }

    /* DMA2_EXTEN 全局寄存器 */
    if (addr == DMA2_EXTEN_INTFR_OFF) {
        return;
    }
    if (addr == DMA2_EXTEN_INTFCR_OFF) {
        m->dma.intfr[2] &= ~v;
        {
            RISCVCPU *cpu0 = &m->cpus.harts[0];
            CPURISCVState *env0 = &cpu0->env;
            unsigned ov = (unsigned)env0->wch_evt_mcause_override;

            /* DMA2 Ch8~11: IRQ 100~103 */
            if (ov >= 100u && ov <= 103u) {
                unsigned ch_local = ov - 99u; /* ch_local 1~4 */
                unsigned tcif_bit = DMA_INTFR_TCIF_BIT(ch_local);

                BQL_LOCK_GUARD();
                if (!(m->dma.intfr[2] & (1u << tcif_bit))) {
                    riscv_cpu_update_mip(env0, MIP_MEIP, 0);
                    env0->wch_evt_mcause_override = 0;
                    riscv_cpu_interrupt(env0);
                }
            }
        }
        return;
    }
}

/* =====================================================================
 * MemoryRegionOps
 * ===================================================================== */
const MemoryRegionOps ch32_dma_ops = {
    .read       = ch32_dma_read,
    .write      = ch32_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* =====================================================================
 * 初始化与 reset
 * ===================================================================== */
void ch32_dma_reset(Ch32MachineState *m)
{
    memset(&m->dma.intfr,  0, sizeof(m->dma.intfr));
    memset(m->dma.ch1, 0, sizeof(m->dma.ch1));
    memset(m->dma.ch2, 0, sizeof(m->dma.ch2));
}

void ch32_dma_init_mr(Ch32MachineState *m, Object *owner)
{
    memory_region_init_io(&m->dma.iomem, owner, &ch32_dma_ops, m,
                          "ch32-dma", CH32_DMA1_BLOCK_SIZE);
}
