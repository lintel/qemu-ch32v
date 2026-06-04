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
 * File:     ch32-eth-10m.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32V20x_D8 ETH10M 内置 10M 以太网 MAC 仿真。
 *
 *     硬件对应：CH32V203RBT6（D8 封装）内置 ENC28J60 兼容接口的 10M MAC+PHY。
 *     地址：0x40028000，大小 0x30 字节，IRQ = 61（CH32V20x_D8 ETH_IRQn）。
 *
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/sysbus.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/irq.h"
#include "net/net.h"
#include "qemu/main-loop.h"
#include "qemu/bswap.h"
#include "system/address-spaces.h"
#include "target/riscv/cpu.h"
#include "target/riscv/cpu_bits.h"
#include "ch32-machine-internal.h"

/* -----------------------------------------------------------------------
 * ETH10M 寄存器偏移（相对 0x40028000）
 * ----------------------------------------------------------------------- */
#define E10M_OFF_EIE       0x03u   /* 中断使能 */
#define E10M_OFF_EIR       0x04u   /* 中断标志（W1C） */
#define E10M_OFF_ESTAT     0x05u   /* 状态（只读） */
#define E10M_OFF_ECON2     0x06u   /* PHY 模拟控制 */
#define E10M_OFF_ECON1     0x07u   /* 收发控制 */
#define E10M_OFF_ETXST     0x08u   /* TX DMA 起始地址（16b） */
#define E10M_OFF_ETXLN     0x0Au   /* TX 数据长度（16b） */
#define E10M_OFF_ERXST     0x0Cu   /* RX DMA 起始地址（16b），写触发接收准备 */
#define E10M_OFF_ERXLN     0x0Eu   /* RX 已接收长度（16b，只读） */
#define E10M_OFF_HTL       0x10u   /* 哈希表低 32b */
#define E10M_OFF_HTH       0x14u   /* 哈希表高 32b */
#define E10M_OFF_ERXFCON   0x18u   /* 接收过滤控制 */
#define E10M_OFF_MACON1    0x19u   /* MAC 接收使能 */
#define E10M_OFF_MACON2    0x1Au   /* MAC 帧控制 */
#define E10M_OFF_MABBIPG   0x1Bu   /* 最小帧间距 */
#define E10M_OFF_EPAUS     0x1Cu   /* 流控暂停时间（16b） */
#define E10M_OFF_MAMXFL    0x1Eu   /* 最大帧长（16b） */
#define E10M_OFF_MIRD      0x20u   /* PHY 读数据（16b） */
#define E10M_OFF_MIREGADR  0x24u   /* PHY 寄存器地址（8b） */
#define E10M_OFF_MISTAT    0x25u   /* PHY 状态（8b） */
#define E10M_OFF_MIWR      0x26u   /* PHY 写操作（16b） */
/* R32_ETH_MIWR = MIREGADR(8b) | (1<<8) | (val<<16) —— 32b 写合并 */
#define E10M_OFF_MIWR32    0x24u   /* R32_ETH_MIWR 使用的 32b 窗口 */
#define E10M_OFF_MAADRL    0x28u   /* MAC 地址字节 1-4（32b） */
#define E10M_OFF_MAADRH    0x2Cu   /* MAC 地址字节 5-6（16b） */

/* EIE 位 */
#define E10M_EIE_INTIE     0x80u
#define E10M_EIE_RXIE      0x40u
#define E10M_EIE_LINKIE    0x10u
#define E10M_EIE_TXIE      0x08u
#define E10M_EIE_TXERIE    0x02u
#define E10M_EIE_RXERIE    0x01u

/* EIR 位（W1C） */
#define E10M_EIR_RXIF      0x40u
#define E10M_EIR_LINKIF    0x10u
#define E10M_EIR_TXIF      0x08u
#define E10M_EIR_TXERIF    0x02u
#define E10M_EIR_RXERIF    0x01u

/* ECON1 位 */
#define E10M_ECON1_TXRTS   0x08u   /* 发送启动 */
#define E10M_ECON1_RXEN    0x04u   /* 接收使能 */
#define E10M_ECON1_RXRST   0x40u   /* 接收模块复位 */
#define E10M_ECON1_TXRST   0x80u   /* 发送模块复位 */

/* PHY 寄存器地址 */
#define E10M_PHY_BMCR      0x00u
#define E10M_PHY_BMSR      0x01u
#define E10M_PHY_ANLPAR    0x05u
#define E10M_PHY_MDIX      0x1eu

/* PHY 状态值 */
#define E10M_PHY_LINKED    0x0004u
#define E10M_PHY_AUTONEG   0x0020u

#define E10M_PKT_MAX                  1536u

/* Link-up 延迟常量（ns/ms 换算 + 三处 timing） */
#define E10M_NS_PER_MS                1000000ULL
#define E10M_LINK_UP_DELAY_MS         200u   /* device reset 后到 link-up */
#define E10M_PHY_RESET_LINK_DELAY_MS  300u   /* PHY BMCR.Reset 后到 link-up */
#define E10M_LINK_TIMER_SLACK_MS      10u    /* timer_mod 比 deadline 多出余量 */

/* -----------------------------------------------------------------------
 * 设备状态结构
 * ----------------------------------------------------------------------- */
typedef struct Ch32Eth10mState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    NICState *nic;
    NICConf conf;

    uint8_t regs[CH32_ETH_10M_MMIO_SIZE]; /* 寄存器镜像 */
    uint16_t phy_regs[32];               /* PHY MII 寄存器 */

    uint8_t tx_buf[E10M_PKT_MAX];    /* TX 常驻缓冲，避免栈上 1536B */
    uint8_t rx_stash[E10M_PKT_MAX];  /* 已收到但尚未投递给固件的帧 */
    uint32_t rx_stash_len;

    /* 反向指针：连接中断到 PFIC（通过 ch32_eth_10m_meip_resync） */
    Ch32MachineState *machine;

    /* 中断激活防重入标志 */
    bool irq_active;

    /*
     * PHY LINKIF 延迟：模拟真机 PHY 协商过程。
     * 复位后等待一段时间再触发 LINKIF，使固件先以 link-down 初始化，
     * 后收到 LINKIF 触发 ETH_PHYLink() → ETH_LinkUpCfg()。
     */
    uint64_t link_up_deadline_ns;
    bool link_announced;     /* 已触发过 LINKIF 一次 */
    QEMUTimer *link_timer;
} Ch32Eth10mState;

OBJECT_DECLARE_SIMPLE_TYPE(Ch32Eth10mState, CH32_ETH_10M)

/* -----------------------------------------------------------------------
 * SRAM 安全检查（防止 RX 写入任意地址）
 * ----------------------------------------------------------------------- */
static bool eth_10m_span_in_sram(hwaddr p, hwaddr len)
{
    MachineState *ms;
    ram_addr_t rsz;
    hwaddr off;

    if (len == 0) {
        return false;
    }
    ms = current_machine;
    if (!ms || !ms->ram) {
        return false;
    }
    rsz = memory_region_size(ms->ram);
    if (p < CH32_SRAM_BASE) {
        return false;
    }
    off = p - CH32_SRAM_BASE;
    if (off >= rsz || len > rsz - off) {
        return false;
    }
    return true;
}

/* -----------------------------------------------------------------------
 * 中断路由：IRQ 61 → PFIC/MEIP
 * IRQ 61 在 PFIC 中 word=1（61/32=1），bit=29（61%32=29）
 * ----------------------------------------------------------------------- */

/*
 * eth_10m_meip_withdraw - 撤销本设备占用的 MEIP override。
 *
 * 仅当 override 仍由本设备持有（== n）时才清理 CPU 端状态，避免影响其他
 * 外设的 override。无论是否持有，都会复位 s->irq_active=false，允许后续
 * 重新注入。
 *
 * resync_others=true 时在清理后通知其他外设重新评估其挂起中断（撤销路径
 * 用），fall through 准备重注入路径不需要（自身马上会注入，避免无谓抢占）。
 */
static void eth_10m_meip_withdraw(Ch32Eth10mState *s, CPURISCVState *env,
                                  unsigned n, bool resync_others)
{
    if (env->wch_evt_mcause_override == n) {
        riscv_cpu_update_mip(env, MIP_MEIP, 0);
        env->wch_evt_mcause_override = 0;
        env->mie &= ~MIP_MEIP;
        ch32_pfic_irq_active_clear(s->machine, n);
        if (resync_others) {
            /* override 被清除，通知其他外设重新评估其中断 */
            ch32_pfic_resync_pending_meip(s->machine);
        }
    }
    s->irq_active = false;
}

/*
 * eth_10m_meip_inject - 注入 MEIP override 并标记 irq_active。
 * 调用前需保证 override 未被其他外设占用（仲裁通过）。
 */
static void eth_10m_meip_inject(Ch32Eth10mState *s, CPURISCVState *env,
                                unsigned n)
{
    env->mie |= MIP_MEIP;
    env->wch_evt_mcause_override = n;
    s->irq_active = true;
    ch32_pfic_irq_active_set(s->machine, n);
    riscv_cpu_update_mip(env, MIP_MEIP, MIP_MEIP);
    riscv_cpu_interrupt(env);
}

void ch32_eth_10m_meip_resync(Ch32MachineState *m)
{
    Ch32Eth10mState *s;
    RISCVCPU *cpu;
    CPURISCVState *env;
    const unsigned n   = CH32_ETH_10M_IRQ_N;   /* 61 */
    const unsigned wd  = n / 32u;              /* 1  */
    const unsigned bit = n % 32u;              /* 29 */
    bool ienr_on;
    bool eir_active;
    uint8_t eie, eir;

    if (!m) {
        return;
    }
    s = m->eth10m_state;
    if (!s) {
        return;
    }
    cpu = &m->cpus.harts[0];
    env = &cpu->env;

    ienr_on = (m->pfic_ienr[wd] >> bit) & 1u;
    eie = s->regs[E10M_OFF_EIE];
    eir = s->regs[E10M_OFF_EIR];
    /* 中断激活条件：INTIE=1 且 EIR 中有对应 EIE 使能的标志 */
    eir_active = ((eie & E10M_EIE_INTIE) != 0u) && ((eie & eir) != 0u);

    BQL_LOCK_GUARD();

    /* 路径 1：不应 active —— 撤销 override，并通知其他外设抢占 */
    if (!ienr_on || !eir_active) {
        eth_10m_meip_withdraw(s, env, n, true);
        riscv_cpu_interrupt(env);
        return;
    }

    /*
     * 路径 2：已 active，仍有挂起 EIR。
     * 若 ISR 已 mret 返回（wch_vtf_in_isr==0 且 override 仍是我们）则撤销
     * 后 fall through 重注入；否则 ISR 还在跑，等待。
     */
    if (s->irq_active) {
        bool isr_done = (env->wch_vtf_in_isr == 0
                      && env->wch_evt_mcause_override == n);
        if (!isr_done) {
            return;
        }
        eth_10m_meip_withdraw(s, env, n, false);
    }

    /*
     * 路径 3：仲裁 + 注入。其他外设已持有 override 时不抢占；等对方
     * 处理完会通过 ch32_pfic_resync_pending_meip 主动唤醒我们。
     */
    if (env->wch_evt_mcause_override != 0u
        && env->wch_evt_mcause_override != n) {
        return;
    }
    eth_10m_meip_inject(s, env, n);
}

static void eth_10m_update_irq(Ch32Eth10mState *s)
{
    ch32_eth_10m_meip_resync(s->machine);
}

static void eth_10m_raise_eir(Ch32Eth10mState *s, uint8_t bits)
{
    s->regs[E10M_OFF_EIR] |= bits;
    eth_10m_update_irq(s);
}

/* -----------------------------------------------------------------------
 * PHY 模拟
 * ----------------------------------------------------------------------- */
static uint16_t eth_10m_phy_read(Ch32Eth10mState *s, unsigned reg)
{
    if (reg >= 32u) {
        return 0u;
    }
    if (reg == E10M_PHY_BMSR) {
        /*
         * 在 link_up_deadline_ns 之前返回 link-down（0），之后返回
         * link-up + autoneg complete，触发 ETH_PHYLink() → ETH_LinkUpCfg()。
         */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        bool up = (now >= (int64_t)s->link_up_deadline_ns);
        return up ? (E10M_PHY_LINKED | E10M_PHY_AUTONEG) : 0x0000u;
    }
    if (reg == E10M_PHY_ANLPAR) {
        /*
         * 返回 0（无远端协商信息），驱动 (phy_anlpar & SELECTOR) 为假，
         * 走 phy_bmsr 路径，autoneg complete 时调用 ETH_LinkUpCfg()。
         */
        return 0x0000u;
    }
    return s->phy_regs[reg];
}

static void eth_10m_phy_write(Ch32Eth10mState *s, unsigned reg, uint16_t val)
{
    if (reg >= 32u) {
        return;
    }
    s->phy_regs[reg] = val;
    if (reg == E10M_PHY_BMCR && (val & 0x8000u)) {
        /* PHY Reset：恢复默认，延迟重置 link-up 时钟，并重排 timer */
        s->phy_regs[reg] = 0x3100u;
        s->link_up_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                                 + E10M_PHY_RESET_LINK_DELAY_MS * E10M_NS_PER_MS;
        s->link_announced = false;
        /*
         * 必须 timer_mod：若旧 timer 已触发过，不 timer_mod 则 LINKIF
         * 不会再次发出，固件会陷在 link-down 等待。
         */
        timer_mod(s->link_timer,
                  s->link_up_deadline_ns + E10M_LINK_TIMER_SLACK_MS * E10M_NS_PER_MS);
    }
}

/*
 * eth_10m_dma_addr - 将 ETH10M 16 位 DMA 指针扩展为完整物理地址。
 *
 * 真实 CH32V20x ETH10M 的 ETXST/ERXST 寄存器只有 16 位，但其 DMA 引擎
 * 直接访问 MCU SRAM（基址 0x20000000），硬件内部自动补全高位地址。
 * QEMU 仿真中需要在软件层面重建完整地址：
 *   若 addr < CH32_SRAM_BASE，则 phys_addr = CH32_SRAM_BASE + addr
 *   否则 phys_addr = addr（固件偶尔写入完整 32 位地址的情况）
 */
static hwaddr eth_10m_dma_addr(hwaddr addr)
{
    if (addr < CH32_SRAM_BASE) {
        return CH32_SRAM_BASE + addr;
    }
    return addr;
}

/* -----------------------------------------------------------------------
 * TX：ECON1.TXRTS=1 触发发送
 * ----------------------------------------------------------------------- */
static void eth_10m_do_tx(Ch32Eth10mState *s)
{
    hwaddr tx_addr;
    uint16_t tx_len;

    tx_addr = eth_10m_dma_addr(lduw_le_p(&s->regs[E10M_OFF_ETXST]));
    tx_len  = lduw_le_p(&s->regs[E10M_OFF_ETXLN]);

    if (tx_len == 0u || tx_len > E10M_PKT_MAX) {
        /* 无效长度，报错标志 */
        s->regs[E10M_OFF_ECON1] &= ~E10M_ECON1_TXRTS;
        eth_10m_raise_eir(s, E10M_EIR_TXERIF);
        return;
    }
    if (!eth_10m_span_in_sram((hwaddr)tx_addr, tx_len)) {
        s->regs[E10M_OFF_ECON1] &= ~E10M_ECON1_TXRTS;
        eth_10m_raise_eir(s, E10M_EIR_TXERIF);
        return;
    }
    cpu_physical_memory_read(tx_addr, s->tx_buf, tx_len);
    qemu_send_packet(qemu_get_queue(s->nic), s->tx_buf, tx_len);

    /* 发送完成：清 TXRTS，设 TXIF */
    s->regs[E10M_OFF_ECON1] &= ~E10M_ECON1_TXRTS;
    eth_10m_raise_eir(s, E10M_EIR_TXIF);
}

/* -----------------------------------------------------------------------
 * RX：网络后端接收回调
 * ----------------------------------------------------------------------- */
static bool eth_10m_can_receive(NetClientState *nc)
{
    Ch32Eth10mState *s = qemu_get_nic_opaque(nc);
    return (s->regs[E10M_OFF_ECON1] & E10M_ECON1_RXEN) &&
           (s->rx_stash_len == 0u);
}

static ssize_t eth_10m_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    Ch32Eth10mState *s = qemu_get_nic_opaque(nc);
    hwaddr rx_addr;
    uint16_t rx_len;

    if (!(s->regs[E10M_OFF_ECON1] & E10M_ECON1_RXEN)) {
        return size;   /* 丢弃 */
    }
    if (size > E10M_PKT_MAX) {
        size = E10M_PKT_MAX;
    }

    rx_addr = eth_10m_dma_addr(lduw_le_p(&s->regs[E10M_OFF_ERXST]));
    rx_len  = (uint16_t)size;

    if (!eth_10m_span_in_sram((hwaddr)rx_addr, rx_len)) {
        /*
         * ERXST 还未被固件写入（= 0 或越界）：
         * 暂存帧，等下次 ERXST 写入后由 mmio_write 触发投递。
         * size 已在上方截断到 E10M_PKT_MAX，可直接 memcpy。
         */
        memcpy(s->rx_stash, buf, size);
        s->rx_stash_len = (uint32_t)size;
        return size;
    }

    /* 写入 SRAM */
    cpu_physical_memory_write(rx_addr, buf, rx_len);
    /* 更新 ERXLN（只读，固件读此值得到帧长） */
    stw_le_p(&s->regs[E10M_OFF_ERXLN], rx_len);
    /* 触发 RXIF 中断 */
    eth_10m_raise_eir(s, E10M_EIR_RXIF);
    return size;
}

/* -----------------------------------------------------------------------
 * 定时器回调：触发 LINKIF（PHY link-up 延迟到期）
 * ----------------------------------------------------------------------- */
static void eth_10m_link_timer_cb(void *opaque)
{
    Ch32Eth10mState *s = opaque;

    if (!s->link_announced) {
        s->link_announced = true;
        eth_10m_raise_eir(s, E10M_EIR_LINKIF);
    }
}

/* -----------------------------------------------------------------------
 * MMIO 读
 * ----------------------------------------------------------------------- */
static uint64_t eth_10m_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32Eth10mState *s = opaque;
    uint64_t val = 0u;
    uint16_t mird;
    unsigned i;

    if (addr + size > CH32_ETH_10M_MMIO_SIZE) {
        return 0u;
    }

    /*
     * MIRD 动态从 PHY 取值，不污染 regs 镜像（读路径应无副作用）。
     * 覆盖逻辑放在逐字节拼装里：遇到 MIRD 低/高字节时从 mird 局部变量取。
     */
    mird = eth_10m_phy_read(s, s->regs[E10M_OFF_MIREGADR] & 0x1fu);

    for (i = 0; i < size; i++) {
        hwaddr off = addr + i;
        uint8_t byte;

        if (off == E10M_OFF_MIRD) {
            byte = (uint8_t)mird;
        } else if (off == E10M_OFF_MIRD + 1u) {
            byte = (uint8_t)(mird >> 8u);
        } else {
            byte = s->regs[off];
        }
        val |= (uint64_t)byte << (i * 8u);
    }
    return val;
}

/* -----------------------------------------------------------------------
 * MMIO 写
 * ----------------------------------------------------------------------- */
static void eth_10m_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Ch32Eth10mState *s = opaque;
    unsigned i;

    if (addr + size > CH32_ETH_10M_MMIO_SIZE) {
        return;
    }

    /* 先写入寄存器镜像（字节展开） */
    for (i = 0; i < size; i++) {
        uint8_t byte = (uint8_t)(val >> (i * 8u));
        hwaddr off = addr + i;

        switch (off) {
        case E10M_OFF_EIR:
            /* W1C: 清除对应标志位 */
            s->regs[off] &= ~byte;
            break;
        case E10M_OFF_EIE:
            s->regs[off] = byte;
            break;
        case E10M_OFF_ECON1: {
            uint8_t prev = s->regs[off];
            s->regs[off] = byte;
            /* TXRTS 上升沿触发发送 */
            if (!(prev & E10M_ECON1_TXRTS) && (byte & E10M_ECON1_TXRTS)) {
                eth_10m_do_tx(s);
            }
            /*
             * RXEN 上升沿：通知 QEMU 网络层有包待接收。
             * can_receive() 曾因 RXEN=0 返回 NO 导致 QEMU 停止投递，
             * 当 RXEN 变为 1 时需要主动触发重投递。
             */
            if (!(prev & E10M_ECON1_RXEN) && (byte & E10M_ECON1_RXEN)) {
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
            break;
        }
        case E10M_OFF_ESTAT:
            /*
             * ESTAT 是只读状态寄存器，忽略固件写入。
             * ETH_Configuration() 会向此地址写功能控制位（0xC0），
             * 若保存这些值会导致 WCHNET_ETHIsr 中 ESTAT & 0x78 != 0
             * 误判为接收错误，跳过 DMA 描述符 OWN 清除处理。
             */
            break;
        default:
            s->regs[off] = byte;
            break;
        }
    }

    /* R32_ETH_MIWR 写（偏移 0x24，32b：addr|0x100|val<<16）*/
    if (addr == E10M_OFF_MIWR32 && size == 4u) {
        uint32_t v32 = (uint32_t)val;
        uint8_t phy_addr = v32 & 0x1fu;
        bool is_write    = (v32 >> 8u) & 1u;
        uint16_t phy_val = (uint16_t)(v32 >> 16u);
        if (is_write) {
            eth_10m_phy_write(s, phy_addr, phy_val);
        } else {
            /* 只写地址时更新 MIREGADR */
            s->regs[E10M_OFF_MIREGADR] = phy_addr;
        }
        return;
    }

    /* MIWR 16b 写（0x26，用于分离的 write-data 路径） */
    if (addr == E10M_OFF_MIWR && size == 2u) {
        uint8_t phy_addr = s->regs[E10M_OFF_MIREGADR] & 0x1fu;
        eth_10m_phy_write(s, phy_addr, (uint16_t)val);
        return;
    }

    /* MIREGADR 8b 写：无副作用（MIRD 在读路径动态取值） */

    /* ERXST 写：如果有暂存帧，立即投递 */
    if (addr <= E10M_OFF_ERXST && addr + size > E10M_OFF_ERXST) {
        uint16_t rx_addr_raw = lduw_le_p(&s->regs[E10M_OFF_ERXST]);
        hwaddr rx_phys = eth_10m_dma_addr((hwaddr)rx_addr_raw);
        if (s->rx_stash_len > 0u && eth_10m_span_in_sram(rx_phys, s->rx_stash_len)) {
            uint16_t rx_len = (uint16_t)s->rx_stash_len;
            cpu_physical_memory_write(rx_phys, s->rx_stash, rx_len);
            stw_le_p(&s->regs[E10M_OFF_ERXLN], rx_len);
            s->rx_stash_len = 0u;
            eth_10m_raise_eir(s, E10M_EIR_RXIF);
        }
    }

    eth_10m_update_irq(s);
}

static const MemoryRegionOps eth_10m_mmio_ops = {
    .read  = eth_10m_mmio_read,
    .write = eth_10m_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* -----------------------------------------------------------------------
 * NetClientInfo
 * ----------------------------------------------------------------------- */
static NetClientInfo eth_10m_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = eth_10m_can_receive,
    .receive = eth_10m_receive,
};

/* -----------------------------------------------------------------------
 * Device 生命周期
 * ----------------------------------------------------------------------- */
static void eth_10m_reset(DeviceState *dev)
{
    Ch32Eth10mState *s = CH32_ETH_10M(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    s->phy_regs[E10M_PHY_BMCR] = 0x3100u; /* AN=1, 10M FD */
    s->rx_stash_len = 0u;
    s->irq_active = false;
    s->link_announced = false;

    /*
     * PHY link-up 延迟 E10M_LINK_UP_DELAY_MS：
     * 固件 ETH_Configuration() 读 PHY_BMSR 时得到 link-down（0x0000），
     * 延迟到期后 link_timer 触发 EIR_LINKIF，驱动调用 ETH_PHYLink()，
     * 此时 PHY_BMSR 返回 link-up + autoneg complete → ETH_LinkUpCfg()。
     */
    s->link_up_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                             + E10M_LINK_UP_DELAY_MS * E10M_NS_PER_MS;
    timer_mod(s->link_timer,
              s->link_up_deadline_ns + E10M_LINK_TIMER_SLACK_MS * E10M_NS_PER_MS);

    /* 复位后撤销中断 */
    ch32_eth_10m_meip_resync(s->machine);
}

static void eth_10m_realize(DeviceState *dev, Error **errp)
{
    Ch32Eth10mState *s = CH32_ETH_10M(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &eth_10m_mmio_ops, s,
                          "ch32-eth-10m-mmio", CH32_ETH_10M_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);

    s->nic = qemu_new_nic(&eth_10m_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    s->link_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, eth_10m_link_timer_cb, s);

    /* 其余状态初始化与 link_timer 排程统一在 reset() 内完成，避免重复。 */
}

static void eth_10m_finalize(Object *obj)
{
    Ch32Eth10mState *s = CH32_ETH_10M(obj);

    if (s->link_timer) {
        timer_del(s->link_timer);
        timer_free(s->link_timer);
        s->link_timer = NULL;
    }
    if (s->nic) {
        qemu_del_nic(s->nic);
        s->nic = NULL;
    }
}

static const Property eth_10m_properties[] = {
    DEFINE_NIC_PROPERTIES(Ch32Eth10mState, conf),
};

static void eth_10m_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = eth_10m_realize;
    device_class_set_legacy_reset(dc, eth_10m_reset);
    device_class_set_props(dc, eth_10m_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo eth_10m_type_info = {
    .name          = TYPE_CH32_ETH_10M,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Ch32Eth10mState),
    .instance_finalize = eth_10m_finalize,
    .class_init    = eth_10m_class_init,
};

static void eth_10m_register_types(void)
{
    type_register_static(&eth_10m_type_info);
}

type_init(eth_10m_register_types)

/*
 * ch32_eth_10m_set_machine - 设置 ETH10M 外设与 machine 的双向关联。
 */
void ch32_eth_10m_set_machine(DeviceState *eth_dev, Ch32MachineState *m)
{
    Ch32Eth10mState *s = CH32_ETH_10M(eth_dev);

    s->machine = m;
    m->eth10m_state = s;
}
