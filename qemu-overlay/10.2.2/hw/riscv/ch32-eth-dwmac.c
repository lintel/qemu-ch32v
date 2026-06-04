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
 * File:     ch32-eth-dwmac.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 以太网（STMAC+DMA）+ tap/user 后端。
 *
 *     CH32V307/CH32V317 ST-MAC以太网(DWMAC)兼容后端。
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/sysbus.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/irq.h"
#include "net/net.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "hw/boards.h"
#include "system/address-spaces.h"
#include "target/riscv/cpu.h"
#include "target/riscv/cpu_bits.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "ch32-machine-internal.h"

/* 防止宿主机网包在未配置/未就绪 DMA 时写入任意物理地址，破坏 FreeRTOS 队列等元数据 */
static bool ch32_eth_span_in_main_sram(hwaddr p, hwaddr len)
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
    if (off > rsz || len > rsz - off) {
        return false;
    }
    return true;
}

static bool ch32_eth_ptr_in_main_sram(hwaddr p)
{
    return ch32_eth_span_in_main_sram(p, 1);
}

#define CH32_ETH_OFF_MACMIIAR  0x010u
#define CH32_ETH_OFF_MACMIIDR  0x014u
/*
 * CH32V30x 在 ch32v30x.h 中为单一扁平 ETH_TypeDef（MAC + 大段 RESERVED + PTP + DMA）。
 * DMA 寄存器从 DMABMR 起算 offsetof = 0x1000（非 STM32F4 的 ETH_MAC 与 ETH_DMA 分块 +0x1000
 * 另一套布局）。偏移必须与固件一致，否则 DMATDLAR/DMARDLAR 写在 0x1010/0x100c 而模型仍
 * 用旧 0x1008/0x1004 槽位，读回错误指针会在 fast_copy_to_dma 等处触发 Load fault。
 */
#define CH32_ETH_OFF_DMABMR    0x1000u
#define CH32_ETH_OFF_DMATPDR   0x1004u
#define CH32_ETH_OFF_DMARPDR   0x1008u
#define CH32_ETH_OFF_DMARDLAR  0x100cu
#define CH32_ETH_OFF_DMATDLAR  0x1010u
#define CH32_ETH_OFF_DMASR     0x1014u
#define CH32_ETH_OFF_DMAOMR    0x1018u
#define CH32_ETH_OFF_DMAIER    0x101cu

#define CH32_ETH_MACMIIAR_MB   0x00000001u
#define CH32_ETH_MACMIIAR_MW   0x00000002u
#define CH32_ETH_DMABMR_SR     0x00000001u
#define CH32_ETH_DMAOMR_ST     0x00002000u
#define CH32_ETH_DMAOMR_SR     0x00000002u
#define CH32_ETH_DMAOMR_FTF    0x00100000u

#define CH32_ETH_DMASR_TPS_M   0x00700000u
#define CH32_ETH_DMASR_RPS_M   0x000E0000u
#define CH32_ETH_DMASR_TPS_FETCH 0x00100000u
#define CH32_ETH_DMASR_RPS_WAIT  0x00060000u

/*
 * DMASR 异常位（STM32 以太网 DMA 兼容）：
 *   TBUS (bit 5): Transmit Buffer Unavailable
 *   RBUS (bit 7): Receive Buffer Unavailable
 */
#define CH32_ETH_DMASR_TBUS  0x00000020u
#define CH32_ETH_DMASR_RBUS  0x00000080u

/*
 * DMASR 写 1 清除：覆盖 ch32v30x_eth.h 中常用 IT/异常位及 CH32 内部 PHY 链路位
 *（ETH_DMA_IT_PHYLINK 等），避免固件清标志后读回仍为 1。
 */
#define CH32_ETH_DMASR_W1C_MASK 0x807fffffu

#define CH32_ETH_DMARX_OWN     0x80000000u
#define CH32_ETH_DMARX_FS      0x00000200u
#define CH32_ETH_DMARX_LS      0x00000100u
#define CH32_ETH_DMARX_FT      0x00000020u
#define CH32_ETH_DMARX_FL      0x3fff0000u

#define CH32_ETH_DMATX_OWN     0x80000000u
#define CH32_ETH_DMATX_FS      0x10000000u
#define CH32_ETH_DMATX_LS      0x20000000u

#define CH32_ETH_DMA_IT_R      0x00000040u
#define CH32_ETH_DMA_IT_T      0x00000001u
#define CH32_ETH_DMA_IT_NIS    0x00010000u
#define CH32_ETH_DMA_IT_AIS    0x00008000u

/*
 * DMASR Normal 中断源位（R, T, PHYLINK 等）：NIS = 这些位的 OR。
 * DMASR Abnormal 中断源位（RBUS, TBUS, ER 等）：AIS = 这些位的 OR。
 * NIS/AIS 是只读汇总位，由硬件自动计算。
 *
 * PHYLINK（bit31）也属于 Normal 中断源：
 * EVT eth_driver_10M.c 在 WCHNET_ETHIsr 中先判断 NIS，再判断 PHYLINK。
 * 如果 NIS 不被自动设置，固件将跳过 PHYLINK 分支，导致 PHY link-up 失败。
 */
/* ETH_DMA_IT_PHYLINK = DMASR bit31，EVT 10M 驱动通过此中断触发 ETH_PHYLink() */
#define CH32_ETH_DMA_IT_PHYLINK  0x80000000u
#define CH32_ETH_DMASR_NIS_SRC (CH32_ETH_DMA_IT_R | CH32_ETH_DMA_IT_T | CH32_ETH_DMA_IT_PHYLINK)
#define CH32_ETH_DMASR_AIS_SRC (CH32_ETH_DMASR_RBUS | CH32_ETH_DMASR_TBUS)

/*
 * MACCR 寄存器（偏移 0x0000）
 *   IPC (bit 10): 实现校验和存入控制，开启接收 IP/负载校验和验证
 */
#define CH32_ETH_OFF_MACCR     0x0000u
#define CH32_ETH_MACCR_IPC     0x00000400u

/*
 * MACCR 扩展位（CH32FV2x_V3xRM §27 + STM32 DWMAC 同源布局）：
 *   PS  (bit 15): Port Select，0 = GMII/RGMII（1000M）；1 = MII/RMII（10/100M）
 *   FES (bit 14): Fast Ethernet Speed，0 = 10M；1 = 100M（仅 PS=1 有效）
 *   DM  (bit 11): Duplex Mode，0 = 半双工；1 = 全双工
 *
 * CH32V307/V317 真机支持 MII/RMII/RGMII（10/100/1000 Mbps）。
 * QEMU hw/net 层按字节流透传，这些位仅用于寄存器回读 / PHY PHYSR 回映 /
 * 日志；真机需驱动按 PHY 协商结果主动写 MACCR.PS/FES/DM 以匹配链路。
 */
#define CH32_ETH_MACCR_PS      (1u << 15)
#define CH32_ETH_MACCR_FES     (1u << 14)
#define CH32_ETH_MACCR_DM      (1u << 11)

/*
 * TDes0 的 CIC 字段 [23:22]：校验和插入控制
 *   00: 禁用 COE
 *   01: 仅 IPv4 头部校验和
 *   10: IPv4 头部 + TCP/UDP 负载（仅 segment）
 *   11: 全部（IPv4 头部 + TCP/UDP/ICMP + pseudo-header）
 */
#define CH32_ETH_DMATX_CIC_M   0x00C00000u
#define CH32_ETH_DMATX_CIC_OFF 0x00000000u
#define CH32_ETH_DMATX_CIC_IP  0x00400000u
#define CH32_ETH_DMATX_CIC_SEG 0x00800000u
#define CH32_ETH_DMATX_CIC_ALL 0x00C00000u

/*
 * RDes0 校验和错误位（MACCR.IPC=1 时硬件验证结果）
 *   IPHCE (bit 7): IPv4 头部校验和错误
 *   PCE   (bit 0): 负载校验和错误（TCP/UDP/ICMP）
 */
#define CH32_ETH_DMARX_IPHCE   0x00000080u
#define CH32_ETH_DMARX_PCE     0x00000001u

#define CH32_ETH_MAX_RING      16
#define CH32_ETH_PKT_MAX       2048

/*
 * PHY 变体枚举：
 *   CH182    - CH32V317 外置 100M CH182 PHY
 *   PHY10M   - CH32V307 内置 10M PHY
 *   RTL8211F - Realtek 1000M RGMII PHY（用于 V307/V317 RGMII 路径）
 *
 * 通过属性 phy-model="ch182|phy10m|rtl8211f" 选择，未填时默认 CH182。
 */
typedef enum {
    CH32_ETH_PHY_CH182 = 0,
    CH32_ETH_PHY_PHY10M,
    CH32_ETH_PHY_RTL8211F,
} Ch32EthPhyVariant;

typedef struct Ch32EthState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    NICState *nic;
    NICConf conf;
    QEMUBH *bh;
    QEMUTimer poll;
    bool poll_active;
    /*
     * 退避计数器：BH 发现无实际工作（TX/RX 均无活动）时递增，
     * 达到阈值后将 poll 间隔从 10ms 延长至 100ms，降低空闲 CPU 占用。
     * 有实际工作时清零，恢复 10ms 快速轮询。
     */
    unsigned poll_idle_count;

    uint32_t mmio_w[CH32_ETH_MMIO_SIZE / 4];
    uint16_t phy_reg[32];
    uint16_t phy_bcr;
    uint16_t phy_bsr;
    uint8_t rx_stash[CH32_ETH_PKT_MAX];
    uint32_t rx_stash_len;

    /*
     * DMA chain 游标：跟踪 chain 链表中当前描述符地址，避免每次都从
     * DMATDLAR/DMARDLAR 头部线性扫描。复位时清零，触发重新初始化。
     */
    hwaddr tx_cur_desc;     /* 当前 TX 描述符 hwaddr */
    hwaddr rx_cur_desc;     /* 当前 RX 描述符 hwaddr */

    /*
     * 反向指针：连接 ETH 外设中断到 PFIC（通过 ch32_eth_meip_resync）。
     * 由 ch32-v.c 在 machine init 中设置；若为 NULL 则不触发中断。
     */
    Ch32MachineState *machine;

    /*
     * 中断激活状态标志：模拟真实 PFIC 的"中断处理中不重入"行为。
     * 当 ETH 中断被注入（MEIP=1）时置 true；在 DMASR W1C 清标志使
     * DMASR 不再活跃时置 false（表示 Guest 已应答中断）。
     * 若为 true，resync 不重复注入 MEIP，防止中断风暴。
     */
    bool irq_active;

    /*
     * PHY link-up 延迟：为了使固件的 ETH_Configuration() 在初始化时读到
     * link-down 状态（LastPhyStat != 0x782d），而后续循环读到 link-up，
     * 从而触发 ETH_PHYLink() 建立连接。
     * phy_link_up_deadline_ns：QEMU 虚拟时钟 ns，达到后 BSR 返回 link-up。
     */
    uint64_t phy_link_up_deadline_ns;

    /*
     * phy_link_irq_injected：防止重复注入 PHYLINK 中断。
     * 10M 驱动（eth_driver_10M.c）依赖 ETH_DMA_IT_PHYLINK（DMASR bit31）中断
     * 触发 ETH_PHYLink()，而不是轮询 BSR。
     * 当 phy_link_up_deadline_ns 到期后，QEMU 主动向 DMASR 设置 PHYLINK 位
     * 并注入中断，使固件走 ISR 路径完成 PHY link-up 配置。
     * 此标志确保每次 reset 后只注入一次，避免重复触发。
     */
    bool phy_link_irq_injected;

    /*
     * phy_model：字符串属性（DEFINE_PROP_STRING），取值 "ch182|phy10m|rtl8211f"；
     * NULL 时默认 CH182。未知字符串使 realize 失败。
     * phy_variant 为解析后的枚举值，运行期唯一依据。
     */
    char *phy_model;
    Ch32EthPhyVariant phy_variant;

    /*
     * RTL8211F 扩展寄存器状态：
     *   rtl8211f_page      - Page Select (reg 0x1F)，复位 0x0000（基础页），
     *                         写入 0xd04 进入 LED/EEE 扩展页，其他值透传。
     *   rtl8211f_mmd_ctrl  - MACR (reg 0x0D) MMD 控制寄存器，记录 DEVAD+Function。
     *   rtl8211f_mmd_addr  - MAADR (reg 0x0E) MMD 地址/数据寄存器写入值（简化：
     *                         对未定义 MMD 寄存器读返回 0，不实际维护 MMD 空间）。
     */
    uint16_t rtl8211f_page;
    uint16_t rtl8211f_mmd_ctrl;
    uint16_t rtl8211f_mmd_addr;
} Ch32EthState;

OBJECT_DECLARE_SIMPLE_TYPE(Ch32EthState, CH32_ETH_DWMAC)

/*
 * ch32_eth_is_phy10m_path - 当前 PHY 变体是否走 CH32V307 内置 10M PHY 路径。
 * 所有原 10M 分支判定统一基于 phy_variant。
 */
static inline bool ch32_eth_is_phy10m_path(const Ch32EthState *s)
{
    return s->phy_variant == CH32_ETH_PHY_PHY10M;
}

/*
 * ch32_eth_is_rtl8211f_path - 当前 PHY 变体是否为 Realtek RTL8211F 千兆 RGMII PHY。
 * CH32V307/CH32V317 通过 RGMII 外接 RTL8211F 时使用该路径；PHY Clause22 寄存器
 * 与 CH182 完全不同（PHY ID、PHYSR、Page Select 0x1F、MMD 间接访问等）。
 */
static inline bool ch32_eth_is_rtl8211f_path(const Ch32EthState *s)
{
    return s->phy_variant == CH32_ETH_PHY_RTL8211F;
}

/*
 * ch32_eth_phy_bcr_reset - PHY BCR 上电/软复位默认值：
 *   PHY10M   : 0x1000 (AN=1, 10M HD)
 *   RTL8211F : 0x1140 (AN=1, 1000M FD)
 *   CH182    : 0x3100 (AN=1, 100M FD)
 */
static uint16_t ch32_eth_phy_bcr_reset(const Ch32EthState *s)
{
    switch (s->phy_variant) {
    case CH32_ETH_PHY_PHY10M:
        return 0x1000u;
    case CH32_ETH_PHY_RTL8211F:
        return 0x1140u;
    case CH32_ETH_PHY_CH182:
    default:
        return 0x3100u;
    }
}

/*
 * ch32_eth_meip_resync - 同步 ETH DMA 中断到 PFIC/MEIP。
 *
 * ETH_IRQn = 77（word=2, bit=13）。固件 ETH_IRQHandler 通过
 * wch_evt_mcause_override=77 由 VTF/MEIP 机制路由。
 * 调用点：
 *   - DMA 状态寄存器（DMASR）发生变化后（收/发完成、DMASR W1C 清标志）
 *   - PFIC IENR/IRER 写后（通过 ch32_pfic_resync_all → ch32_eth_meip_resync）
 *
 * 中断风暴防护（模拟 PFIC "中断激活时不重入" 语义）：
 * 真实 CH32 PFIC 在中断被应答（handler 执行中）时不会再次投递同一中断，
 * 直到 handler 返回（mret）。QEMU 的 MEIP 模拟缺少此机制：
 * 若 DMASR 标志在 Guest mret 前又被置位，resync 会重新注入 MEIP，
 * 导致 Guest mret 后立即再次进入 IRQ handler 形成死循环。
 *
 * 解决：用 s->irq_active 标志跟踪中断激活状态：
 *   - 注入 MEIP 时 irq_active=true
 *   - Guest 在 IRQ handler 中 W1C 清 DMASR 使 dmasr_active=false 时 irq_active=false
 *   - irq_active=true 时 resync 不注入 MEIP
 *   - Guest mret 后若 DMASR 仍有事件，poll timer/BH 会在下次调用时注入
 */
void ch32_eth_meip_resync(Ch32MachineState *m)
{
    Ch32EthState *s;
    RISCVCPU *cpu;
    CPURISCVState *env;
    const unsigned n   = CH32_ETH_IRQ_N;    /* 77 */
    const unsigned wd  = n / 32u;            /* 2  */
    const unsigned bit = n % 32u;            /* 13 */
    bool ienr_on;
    bool dmasr_active;
    uint32_t ier, sr;

    if (!m) {
        return;
    }
    /* 找到 ETH 外设状态（通过 machine 结构中记录的反向指针） */
    s = m->eth_state;
    if (!s) {
        return;
    }
    cpu = &m->cpus.harts[0];
    env = &cpu->env;
    ienr_on = (m->pfic_ienr[wd] >> bit) & 1u;
    ier = s->mmio_w[CH32_ETH_OFF_DMAIER / 4];
    sr  = s->mmio_w[CH32_ETH_OFF_DMASR  / 4];
    dmasr_active = (ier & sr) != 0u;

    BQL_LOCK_GUARD();
    if (!ienr_on || !dmasr_active) {
        if (env->wch_evt_mcause_override == n) {
            riscv_cpu_update_mip(env, MIP_MEIP, 0);
            env->wch_evt_mcause_override = 0;
            env->mie &= ~MIP_MEIP;
            /* 清除 PFIC IACTR 中 ETH IRQ(77) 的激活状态 */
            ch32_pfic_irq_active_clear(m, n);
            /*
             * ETH MEIP 撤销：通知其他外设重新评估其中断请求
             * （与 ch32-eth-10m.c 的 MEIP 撤销路径一致）。
             */
            ch32_pfic_resync_pending_meip(m);
        }
        /* DMASR 不再活跃 → 清除激活标志 */
        s->irq_active = false;
        riscv_cpu_interrupt(env);
        return;
    }
    /*
     * 中断风暴防护：如果 irq_active=true，说明 ETH 中断正在处理中，
     * 不重复注入 MEIP。Guest 在 IRQ handler 中 W1C 清 DMASR 后
     * irq_active 会被清除；之后若有新事件（poll timer/BH 设 DMASR），
     * resync 会重新注入 MEIP。
     */
    if (s->irq_active) {
        return;
    }
    /*
     * MEIP 仲裁：如果其他外设已持有 MEIP，则不覆盖。
     * 等对方 IRQ 处理完后，对方会调用 ch32_pfic_resync_pending_meip
     * 重新触发我们的注入（与 ch32-eth-10m.c 的仲裁路径一致）。
     */
    if (env->wch_evt_mcause_override != 0u && env->wch_evt_mcause_override != n) {
        return;
    }
    env->mie |= MIP_MEIP;
    env->wch_evt_mcause_override = n;
    s->irq_active = true;
    /* 在 PFIC IACTR 中标记 ETH IRQ(77) 为激活状态 */
    ch32_pfic_irq_active_set(m, n);
    riscv_cpu_update_mip(env, MIP_MEIP, MIP_MEIP);
    riscv_cpu_interrupt(env);
}

static void ch32_eth_update_irq(Ch32EthState *s)
{
    ch32_eth_meip_resync(s->machine);
}

/*
 * ch32_eth_dmasr_raise - 设置 DMASR 位并同步中断。
 *
 * 注意：不手动设置 NIS/AIS 汇总位——它们在 ch32_eth_dmasr_read 中
 * 从底层事件位动态计算。这确保固件 W1C 清 NIS 后，如果底层事件
 * 位（R/T 等）仍在，NIS 会在下次读取时自动恢复，不会导致
 * dmasr_active 误判为 false。
 *
 * 中断风暴防护：
 * 调用者（BH/poll timer/MMIO write）每次都可能设置 DMASR 标志。
 * 如果 Guest 正在处理 ETH 中断（MEIP 已置位，irq_active=true），
 * ch32_eth_meip_resync 会跳过重复注入 MEIP，防止 mret 后立即重入
 * IRQ handler 形成死循环。
 *
 * 每次 dmasr_raise 都调用 update_irq（进而 meip_resync），
 * 由 meip_resync 内部的 irq_active 检查来防护中断风暴——
 * 而不是在 dmasr_raise 中跳过 update_irq 调用。
 * 这样确保：当 irq_active 从 false 变为 true 时（Guest 首次进入
 * IRQ handler），后续的 dmasr_raise 不会重复注入；而当
 * irq_active 恢复为 false（Guest 清完 DMASR 标志）后，如果有
 * 新事件，update_irq 会正确注入 MEIP。
 */
static void ch32_eth_dmasr_raise(Ch32EthState *s, uint32_t bits)
{
    /* 只设底层事件位，不设 NIS/AIS 汇总位（它们在 dmasr_read 中动态计算） */
    s->mmio_w[CH32_ETH_OFF_DMASR / 4] |= (bits & ~(CH32_ETH_DMA_IT_NIS | CH32_ETH_DMA_IT_AIS));
    ch32_eth_update_irq(s);
}

/*
 * RTL8211F Clause22 + Page 0xd04 + MMD 间接访问模型。
 *
 * 基础页（rtl8211f_page==0x0000）寄存器：
 *   0x00 BMCR、 0x01 BMSR、 0x02/0x03 PHYID1/2（0x001CC916）、
 *   0x04 ANAR、  0x05 ANLPAR、 0x06 ANER、
 *   0x09 GBCR、  0x0A GBSR、   0x0F GBESR、
 *   0x0D MACR、  0x0E MAADR（MMD 间接）、
 *   0x11 PHYSR (RTL8211F 专属 speed/duplex/link)、
 *   0x1A INSR、  0x1F PAGSR。
 *
 * 扩展页 0xd04：0x10 LCR、 0x11 EEELCR，返回 guest 写入值。
 *
 * 链路模型：phy_link_up_deadline_ns 到期前返回 link-down，到期后返回
 * 1000M FD link-up（PHYSR=0x002C，GBSR 宣告链路伙伴 1000FD）。
 */
static uint16_t ch32_eth_phy_read_rtl8211f(Ch32EthState *s, unsigned reg)
{
    bool link_up = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->phy_link_up_deadline_ns;

    if (s->rtl8211f_page == 0x0d04u && (reg == 0x10u || reg == 0x11u)) {
        /* Page 0xd04 LCR/EEELCR：返回 guest 写入值（phy_reg[] 当缓存） */
        return s->phy_reg[reg];
    }

    switch (reg) {
    case 0x00:
        return s->phy_bcr;
    case 0x01:
        /* BMSR：link-up 后返回 bit2(Link)+bit5(AN Complete) */
        return link_up ? (uint16_t)(s->phy_bsr | 0x0024u) : s->phy_bsr;
    case 0x02:
        /* PHYID1 = OUI[21:6] = 0x001C (Realtek) */
        return 0x001cu;
    case 0x03:
        /* PHYID2 = OUI[5:0]<<10 | Model<<4 | Rev = 0xC916 (RTL8211F) */
        return 0xc916u;
    case 0x04:
        /* ANAR：宣告 100FD/100HD/10FD/10HD + 802.3 selector */
        return s->phy_reg[4] ? s->phy_reg[4] : 0x01e1u;
    case 0x05:
        /* ANLPAR：link-up 后返回链路伙伴能力，否则 0 */
        return link_up ? 0x45e1u : 0x0000u;
    case 0x06:
        /* ANER：LP AN Able + Page Received + Next Page Able */
        return 0x0007u;
    case 0x09:
        /* GBCR：宣告 1000FD（bit9） */
        return s->phy_reg[9] ? s->phy_reg[9] : 0x0200u;
    case 0x0a:
        /*
         * GBSR：link-up 后宣告链路伙伴 1000FD Capable（bit11）+
         * Local/Remote Receiver Status OK（bit13/bit12）+ Master Resolution（bit14）。
         */
        return link_up ? 0x7c00u : 0x0000u;
    case 0x0d:
        return s->rtl8211f_mmd_ctrl;
    case 0x0e:
        /* 简化：不实际维护 MMD 空间，所有未定义 MMD 寄存器读为 0 */
        return 0x0000u;
    case 0x0f:
        /* GBESR：1000T FD Capable (bit13) + 1000T HD Capable (bit12) */
        return 0x3000u;
    case 0x11:
        /*
         * PHYSR (RTL8211F 专属)：
         *   bits[5:4] Speed: 00=10M, 01=100M, 10=1000M, 11=reserved
         *   bit3      Duplex: 1=FD
         *   bit2      Link:   1=up
         * link-up 后返回 1000M FD link-up = 0b0000_0000_0010_1100 = 0x002C。
         */
        return link_up ? 0x002cu : 0x0000u;
    case 0x1a:
        return s->phy_reg[0x1a];
    case 0x1f:
        return s->rtl8211f_page;
    default:
        return (reg < 32) ? s->phy_reg[reg] : 0u;
    }
}

static void ch32_eth_phy_write_rtl8211f(Ch32EthState *s, unsigned reg, uint16_t v)
{
    if (reg < 32) {
        s->phy_reg[reg] = v;
    }
    /* Page 0xd04 LCR/EEELCR：仅存储 guest 写入值（上面 phy_reg[] 已存） */
    if (s->rtl8211f_page == 0x0d04u && (reg == 0x10u || reg == 0x11u)) {
        return;
    }
    switch (reg) {
    case 0x00:
        s->phy_bcr = v;
        if (v & 0x8000u) {
            /* 软复位：BMCR 恢复 0x1140，link-up 延迟 300ms，页选择清零 */
            s->phy_bcr = 0x1140u;
            s->phy_link_up_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                                         + 300 * 1000000ULL;
            s->phy_link_irq_injected = false;
            s->rtl8211f_page = 0x0000u;
        }
        return;
    case 0x0d:
        s->rtl8211f_mmd_ctrl = v;
        return;
    case 0x0e:
        s->rtl8211f_mmd_addr = v;
        return;
    case 0x1f:
        s->rtl8211f_page = v;
        return;
    default:
        return;
    }
}

static void ch32_eth_phy_write(Ch32EthState *s, unsigned reg, uint16_t v)
{
    if (ch32_eth_is_rtl8211f_path(s)) {
        ch32_eth_phy_write_rtl8211f(s, reg, v);
        return;
    }
    if (reg < 32) {
        s->phy_reg[reg] = v;
    }
    if (reg == 0) {
        s->phy_bcr = v;
        if (v & 0x8000u) {
            /*
             * 软件复位：恢复 PHY 上电默认值（速率能力由 phy_variant 决定）。
             *   10M（CH32V307 内置）：BCR=0x1000（AN=1, 10M half-duplex）
             *   100M（CH32V317 CH182）：BCR=0x3100（AN=1, 100M=1, FD=1）
             * 同时重置 link-up 延迟，模拟 PHY 重新连接过程。
             * 对于 10M 模式，重置 phy_link_irq_injected，使下次 deadline
             * 到期时再次注入 PHYLINK 中断。
             */
            s->phy_bcr = ch32_eth_phy_bcr_reset(s);
            s->phy_link_up_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 300 * 1000000ULL;
            s->phy_link_irq_injected = false;
        }
    }
    /* PHY write debug removed */
}

static uint16_t ch32_eth_phy_read(Ch32EthState *s, unsigned reg)
{
    uint16_t val;

    if (ch32_eth_is_rtl8211f_path(s)) {
        return ch32_eth_phy_read_rtl8211f(s, reg);
    }
    switch (reg) {
    case 0:
        val = s->phy_bcr;
        break;
    case 1:
        /*
         * IEEE BSR：Link(2)、Autoneg Complete(5) 等（bit2+bit5 = 0x0024）。
         * PHY link-up 延迟：在 phy_link_up_deadline_ns 之前返回 link-down
         * （0x7809），之后返回 link-up（0x782d）。
         * 这样 ETH_Configuration() 初始化时读到 link-down，
         * 当 WCHNET_QueryPhySta() 循环读到 link-up 时触发 ETH_PHYLink()。
         * 10M 和 100M 的 BSR 位定义相同，无需区分。
         */
        if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->phy_link_up_deadline_ns) {
            val = s->phy_bsr;  /* link-down: 0x7809 */
        } else {
            val = (uint16_t)(s->phy_bsr | 0x0024u);  /* link-up + AutoNego Complete: 0x782d */
        }
        break;
    case 2:
        /*
         * PHY ID1（OUI 高位）：
         *   100M CH182（CH32V317）：0x7371（与真机 OrayOS 日志一致）
         *   10M 内置（CH32V307）：0x0000（CH32V307 片内 10M PHY 无独立 OUI）
         */
        val = ch32_eth_is_phy10m_path(s) ? 0x0000u : 0x7371u;
        break;
    case 3:
        /* PHY ID2：同上，10M 返回 0x0000 */
        val = ch32_eth_is_phy10m_path(s) ? 0x0000u : 0x9011u;
        break;
    case 5:
        /*
         * ANLPAR（远端自动协商能力寄存器）：
         *   100M CH182：宣告 100TX/100TX-FD（0x05e1），供 phy_ch182_get_status 解析。
         *   10M 内置：返回 0x0000（无远端能力宣告）。
         *   eth_driver_10M.c 检查 (phy_anlpar & PHY_ANLPAR_SELECTOR_FIELD)：
         *   结果为假时走 phy_bmsr 路径，AutoNego Complete 后调用 ETH_LinkUpCfg()，
         *   并强制 MACCR &= ~ETH_Mode_FullDuplex（半双工，10M 标准行为）。
         */
        val = ch32_eth_is_phy10m_path(s) ? 0x0000u : (s->phy_reg[5] ? s->phy_reg[5] : 0x05e1u);
        break;
    case 0x19:
        /*
         * WCHNET_AccelerateLink step2（仅 100M CH182 驱动调用）：
         * 写 PHY_PAG_SEL(0x1F)=99，读 reg 0x19，要求 (val & 0xf) == 2。
         * 10M 驱动不调用此路径，返回存储值即可。
         */
        val = (!ch32_eth_is_phy10m_path(s) && s->phy_reg[0x1f] == 99u) ? 0x0002u : s->phy_reg[reg];
        break;
    case 0x10:
        /*
         * WCHNET_CheckLinkVaild（仅 100M CH182 驱动调用）：
         * 要求 (phy_stat & (1<<9)) != 0 以确认 link 有效。
         * 10M 驱动不调用此路径，返回存储值即可。
         */
        val = ch32_eth_is_phy10m_path(s) ? s->phy_reg[reg]
                         : (uint16_t)(s->phy_reg[reg] | (1u << 9));
        break;
    default:
        val = (reg < 32) ? s->phy_reg[reg] : 0u;
        break;
    }
    /* PHY read debug removed */
    return val;
}

static void ch32_eth_mdio_run(Ch32EthState *s, uint32_t *pmiiar, uint32_t *pmiidr)
{
    unsigned pa = (*pmiiar >> 11) & 0x1fu;
    unsigned ra = (*pmiiar >> 6) & 0x1fu;
    bool is_write = (*pmiiar & CH32_ETH_MACMIIAR_MW) != 0;

    if (pa != 1u) {
        if (is_write) {
            /* 忽略非 CH182 地址的写 */
        } else {
            *pmiidr = 0xffffu;
        }
        *pmiiar &= ~CH32_ETH_MACMIIAR_MB;
        return;
    }
    if (is_write) {
        ch32_eth_phy_write(s, ra, (uint16_t)(*pmiidr & 0xffffu));
    } else {
        *pmiidr = ch32_eth_phy_read(s, ra);
    }
    *pmiiar &= ~CH32_ETH_MACMIIAR_MB;
}

/*
 * ch32_eth_tx_coe - 模拟 TX 硬件校验和卸载引擎（COE）。
 *
 * CH32V30x ETH 的 TDes0[23:22] CIC 字段指示 MAC DMA 应自动补充哪些校验和：
 *
 *   CH32_ETH_DMATX_CIC_OFF (00): 不补充（固件自行填充或不需要）
 *   CH32_ETH_DMATX_CIC_IP  (01): 仅补 IPv4 头部校验和
 *   CH32_ETH_DMATX_CIC_SEG (10): IPv4 头部 + TCP/UDP 负载（segment only，不含伪头）
 *   CH32_ETH_DMATX_CIC_ALL (11): 全部（IPv4头部 + TCP/UDP/ICMP + 伪头）
 *
 * 对于 CIC_OFF 以外的所有模式，我们只需对实际为 0 的校验和字段进行软件补全。
 * 使用 net_checksum_calculate 处理 IPv4/TCP/UDP，并针对 ICMP 字段单独补全。
 */
static void ch32_eth_tx_coe(uint8_t *pkt, size_t len, uint32_t td0)
{
    uint32_t cic = td0 & CH32_ETH_DMATX_CIC_M;
    struct ip_header *iph;
    int iph_len;
    uint8_t *l4hdr;
    int l4_len;

    if (cic == CH32_ETH_DMATX_CIC_OFF) {
        /* COE 未启用，固件自行填充校验和 */
        return;
    }

    /* 包长至少需要能容纳一个局部头 + IP 头 + L4 头最小 8 字节 */
    if (len < sizeof(struct eth_header) + sizeof(struct ip_header) + 8u) {
        return;
    }

    iph = (struct ip_header *)(pkt + sizeof(struct eth_header));
    if (IP_HEADER_VERSION(iph) != IP_HEADER_VERSION_4) {
        /* 非 IPv4（如 IPv6 或非-IP），跳过 */
        return;
    }
    iph_len = IP_HDR_GET_LEN(iph);
    if (iph_len < (int)sizeof(struct ip_header) ||
        (size_t)((int)sizeof(struct eth_header) + iph_len) > len) {
        return;
    }

    /*
     * CIC_IP 及以上：补 IPv4 头部校验和。
     * net_checksum_calculate(CSUM_IP) 会处理 ip_sum 字段。
     */
    if (cic >= CH32_ETH_DMATX_CIC_IP) {
        net_checksum_calculate(pkt, (int)len, CSUM_IP);
    }

    /* CIC_SEG 和 CIC_ALL：补 TCP/UDP/ICMP 校验和 */
    if (cic >= CH32_ETH_DMATX_CIC_SEG) {
        l4hdr = (uint8_t *)iph + iph_len;
        l4_len = (int)lduw_be_p(&iph->ip_len) - iph_len;

        if (l4_len < 8) {
            return;
        }
        if ((size_t)((int)sizeof(struct eth_header) + iph_len + l4_len) > len) {
            return;
        }

        switch (iph->ip_p) {
        case IPPROTO_TCP:
        case IPPROTO_UDP:
            /*
             * CIC_SEG 和 CIC_ALL 对 TCP/UDP 效果一致：
             * net_checksum_calculate(CSUM_TCP/CSUM_UDP) 含 pseudo-header 计算。
             */
            net_checksum_calculate(pkt, (int)len,
                                   iph->ip_p == IPPROTO_TCP ? CSUM_TCP : CSUM_UDP);
            break;
        case IPPROTO_ICMP:
            /*
             * ICMP 校验和：只有 CIC_ALL 才要求硬件补全（CIC_SEG 不要求 ICMP）。
             * net_checksum_calculate 不处理 ICMP，这里手动补全。
             */
            if (cic == CH32_ETH_DMATX_CIC_ALL) {
                uint32_t csum;

                stw_he_p(l4hdr + 2, 0);     /* clear ICMP checksum field */
                csum = net_checksum_add(l4_len, l4hdr);
                stw_be_p(l4hdr + 2, net_checksum_finish(csum));
            }
            break;
        default:
            break;
        }
    }
}

static void ch32_eth_desc_read(hwaddr da, uint32_t *st, uint32_t *cb,
                              uint32_t *b1, uint32_t *b2)
{
    uint8_t buf[16];

    if (address_space_read(&address_space_memory, da,
                           MEMTXATTRS_UNSPECIFIED, buf, 16) != MEMTX_OK) {
        *st = *cb = *b1 = *b2 = 0;
        return;
    }
    *st = ldl_le_p(buf);
    *cb = ldl_le_p(buf + 4);
    *b1 = ldl_le_p(buf + 8);
    *b2 = ldl_le_p(buf + 12);
}

static void ch32_eth_desc_write_status(hwaddr da, uint32_t st)
{
    uint8_t b[4];

    stl_le_p(b, st);
    address_space_write(&address_space_memory, da, MEMTXATTRS_UNSPECIFIED,
                        b, 4);
}

static void ch32_eth_process_tx(Ch32EthState *s)
{
    NetClientState *nc;
    hwaddr base;
    int i;

    if ((s->mmio_w[CH32_ETH_OFF_DMAOMR / 4] & CH32_ETH_DMAOMR_ST) == 0) {
        return;
    }
    if (!s->nic) {
        return;
    }
    nc = qemu_get_queue(s->nic);
    if (!nc || !nc->peer) {
        return;
    }
    base = (hwaddr)s->mmio_w[CH32_ETH_OFF_DMATDLAR / 4];
    if (!ch32_eth_ptr_in_main_sram(base)) {
        return;
    }
    /* 初始化 chain 游标 */
    if (s->tx_cur_desc == 0) {
        s->tx_cur_desc = base;
    }

    for (i = 0; i < CH32_ETH_MAX_RING; i++) {
        hwaddr da = s->tx_cur_desc;
        uint32_t td0, td1, td2, td3;
        uint32_t len;
        uint8_t pkt[CH32_ETH_PKT_MAX];

        if (!ch32_eth_ptr_in_main_sram(da)) {
            s->tx_cur_desc = base;
            break;
        }
        ch32_eth_desc_read(da, &td0, &td1, &td2, &td3);
        if ((td0 & CH32_ETH_DMATX_OWN) == 0) {
            /* 无待发描述符 */
            break;
        }
        len = td1 & 0x1fffu;
        if (len == 0 || len > CH32_ETH_PKT_MAX) {
            /* 描述符长度异常，跳过并前进 */
            td0 &= ~CH32_ETH_DMATX_OWN;
            ch32_eth_desc_write_status(da, td0);
            s->tx_cur_desc = ch32_eth_ptr_in_main_sram((hwaddr)td3) ?
                             (hwaddr)td3 : base;
            continue;
        }
        if (!ch32_eth_span_in_main_sram((hwaddr)td2, len)) {
            td0 &= ~CH32_ETH_DMATX_OWN;
            ch32_eth_desc_write_status(da, td0);
            ch32_eth_dmasr_raise(s, CH32_ETH_DMASR_TBUS);
            s->tx_cur_desc = ch32_eth_ptr_in_main_sram((hwaddr)td3) ?
                             (hwaddr)td3 : base;
            break;
        }
        if (address_space_read(&address_space_memory, (hwaddr)td2,
                               MEMTXATTRS_UNSPECIFIED, pkt, len) != MEMTX_OK) {
            td0 &= ~CH32_ETH_DMATX_OWN;
            ch32_eth_desc_write_status(da, td0);
            s->tx_cur_desc = ch32_eth_ptr_in_main_sram((hwaddr)td3) ?
                             (hwaddr)td3 : base;
            break;
        }
        /*
         * TX 硬件校验和卸载（Checksum Offload Engine）仿真：
         * TDes0 的 CIC [23:22] 字段指示硬件需要补充哪些校验和；
         * QEMU 这里进行软件模拟，确保发射的包校验和正确。
         */
        ch32_eth_tx_coe(pkt, len, td0);
        qemu_send_packet(nc, pkt, (int)len);
        /* 只清 OWN 位，保留固件写入的 FS/LS 等标志 */
        td0 &= ~CH32_ETH_DMATX_OWN;
        ch32_eth_desc_write_status(da, td0);
        /* 只设底层事件位，不设 NIS/AIS 汇总位（它们在 dmasr_read 中动态计算） */
        ch32_eth_dmasr_raise(s, CH32_ETH_DMA_IT_T);
        /* 通过 Buffer2NextDescAddr（td3）前进 chain 游标 */
        s->tx_cur_desc = ch32_eth_ptr_in_main_sram((hwaddr)td3) ?
                         (hwaddr)td3 : base;
        /* 不 break：继续处理下一个待发描述符，直到环中无 OWN=1 */
    }
}

static void ch32_eth_rx_resume(Ch32EthState *s);

static void ch32_eth_deliver_rx_stash(Ch32EthState *s)
{
    hwaddr base;
    int i;

    if (s->rx_stash_len == 0) {
        return;
    }
    if ((s->mmio_w[CH32_ETH_OFF_DMAOMR / 4] & CH32_ETH_DMAOMR_SR) == 0) {
        /* SR=0 时不应有 stash；强制清零并恢复 tap，防止永久卡死 */
        s->rx_stash_len = 0;
        ch32_eth_rx_resume(s);
        return;
    }
    base = (hwaddr)s->mmio_w[CH32_ETH_OFF_DMARDLAR / 4];
    if (!ch32_eth_ptr_in_main_sram(base)) {
        /* DMARDLAR 无效；强制清零并恢复 tap，防止永久卡死 */
        s->rx_stash_len = 0;
        ch32_eth_rx_resume(s);
        return;
    }
    /* 初始化 chain 游标 */
    if (s->rx_cur_desc == 0) {
        s->rx_cur_desc = base;
    }

    for (i = 0; i < CH32_ETH_MAX_RING; i++) {
        hwaddr da = s->rx_cur_desc;
        uint32_t rd0, rd1, rd2, rd3;
        uint32_t fl;
        uint32_t new0;

        if (!ch32_eth_ptr_in_main_sram(da)) {
            s->rx_cur_desc = base;
            break;
        }
        ch32_eth_desc_read(da, &rd0, &rd1, &rd2, &rd3);
        if ((rd0 & CH32_ETH_DMARX_OWN) == 0) {
            /*
             * 当前描述符已被固件消费（OWN=0），前进到下一个描述符继续查找。
             * 若 Buffer2NextDescAddr 无效则回绕到链表头部。
             */
            s->rx_cur_desc = ch32_eth_ptr_in_main_sram((hwaddr)rd3) ?
                             (hwaddr)rd3 : base;
            continue;
        }
        if (!ch32_eth_span_in_main_sram((hwaddr)rd2, s->rx_stash_len)) {
            /*
             * 描述符 buf 指针无效（rd2 超出 SRAM），丢弃此帧并前进游标。
             * 不清 rx_stash_len，让 poll 重试下一个描述符；若全部无效才丢包。
             */
            s->rx_cur_desc = ch32_eth_ptr_in_main_sram((hwaddr)rd3) ?
                             (hwaddr)rd3 : base;
            ch32_eth_dmasr_raise(s, CH32_ETH_DMASR_RBUS);
            s->rx_stash_len = 0;   /* buf 无效，此帧无法投递，丢弃 */
            return;
        }
        if (address_space_write(&address_space_memory, (hwaddr)rd2,
                                MEMTXATTRS_UNSPECIFIED,
                                s->rx_stash, s->rx_stash_len) != MEMTX_OK) {
            ch32_eth_dmasr_raise(s, CH32_ETH_DMASR_RBUS);
            s->rx_stash_len = 0;
            ch32_eth_rx_resume(s);
            return;
        }
        fl = ((uint32_t)s->rx_stash_len + 4u) << 16;
        new0 = fl | CH32_ETH_DMARX_FS | CH32_ETH_DMARX_LS | CH32_ETH_DMARX_FT;
        new0 &= ~CH32_ETH_DMARX_OWN;
        /*
         * RX 硬件校验和卸载（IPC = MACCR bit10）仿真：
         * 当 MACCR.IPC=1 时，MAC 硬件会验证接收到的包的 IP/负载校验和。
         * QEMU 收到的包来自宿主机内核 TCP/IP 栈，校验和必然正确；
         * 因此在 RDes0 中不设 IPHCE（bit7）和 PCE（bit0）错误位。
         * 此外也不设 ES（错误汇总），确保固件不会因校验和错误而丢弃正常包。
         */
        ch32_eth_desc_write_status(da, new0);
        ch32_eth_dmasr_raise(s, CH32_ETH_DMA_IT_R);
        /* 通过 Buffer2NextDescAddr（rd3）前进 chain 游标 */
        s->rx_cur_desc = ch32_eth_ptr_in_main_sram((hwaddr)rd3) ?
                         (hwaddr)rd3 : base;
        s->rx_stash_len = 0;
        return;
    }
    /* 遍历完整个环形链表均无 OWN=1 描述符 */
    ch32_eth_dmasr_raise(s, CH32_ETH_DMASR_RBUS);
    /*
     * 描述符耗尽时丢弃 stash 中的帧，防止 tap 永久暂停。
     *
     * 当所有 RX 描述符都被 Guest 消费（OWN=0）且未归还时，stash 中的帧
     * 无法投递。如果不丢弃：rx_stash_len!=0 → can_receive=false →
     * tap_read_poll(false) → QEMU 不再从 tap 读取新包 → 网络永久卡死。
     *
     * 丢弃帧后 rx_stash_len=0，can_receive 恢复 true，tap 继续读取新包。
     * 如果 Guest 在 RBUS 中断处理中归还了描述符（设 OWN=1 写 DMARPDR），
     * 后续的 BH 或 poll timer 会正确投递新帧。
     *
     * 代价：高速流量下偶尔丢帧，但远好过网络永久卡死。
     */
    s->rx_stash_len = 0;
    ch32_eth_rx_resume(s);
}

/*
 * ch32_eth_rx_resume - flush 排队包并恢复 tap 读取轮询。
 *
 * qemu_flush_or_purge_queued_packets 只清 receive_disabled，不恢复
 * tap_read_poll 状态（tap fd 被 set_fd_handler 到 NULL）。
 * 当队列恰好为空时 flush 返回 true 但没有触发 tap_send_completed 回调，
 * tap 的 read_poll 仍为 false，导致 WDT 复位后 tap fd 永不触发新包。
 * 修复：在 flush 之后，若 peer 提供了 poll 接口，显式调用 poll(true)
 * 重新向事件循环注册 tap fd 读取处理函数。
 */
static void ch32_eth_rx_resume(Ch32EthState *s)
{
    NetClientState *nc = qemu_get_queue(s->nic);

    qemu_flush_or_purge_queued_packets(nc, false);
    /*
     * 直接恢复 peer（tap）的读取轮询：
     * tap_poll(enable=true) → tap_read_poll(true) → qemu_set_fd_handler(tap_send)
     */
    if (nc->peer && nc->peer->info->poll) {
        nc->peer->info->poll(nc->peer, true);
    }
}

/*
 * ETH poll timer 间隔（虚拟时钟 ns）：
 *   CH32_ETH_POLL_FAST：有活跃 TX/RX 工作时使用，10 ms。
 *   CH32_ETH_POLL_SLOW：连续若干次 BH 无实际工作后切换，100 ms。
 *   CH32_ETH_POLL_IDLE_THR：连续无工作 BH 次数阈值，超过后切慢速。
 *
 * 典型场景：固件建立 TCP 连接后 HTTP 静默期无流量，poll 退到慢速，
 * 宿主机 curl 请求到来时由 net_receive→BH 立即处理（不依赖 poll）。
 */
#define CH32_ETH_POLL_FAST_NS   (10ULL * 1000 * 1000)   /* 10 ms */
#define CH32_ETH_POLL_SLOW_NS   (100ULL * 1000 * 1000)  /* 100 ms */
#define CH32_ETH_POLL_IDLE_THR  4u

static void ch32_eth_bh(void *opaque)
{
    Ch32EthState *s = opaque;
    uint32_t stash_before;
    bool rx_had_work, tx_had_work;
    uint32_t dmasr_before;

    /*
     * TX: 扫描整个描述符环处理所有待发帧。
     * RX: 投递 stash 中的帧。
     */
    dmasr_before = s->mmio_w[CH32_ETH_OFF_DMASR / 4];
    ch32_eth_process_tx(s);
    tx_had_work = (s->mmio_w[CH32_ETH_OFF_DMASR / 4] & CH32_ETH_DMA_IT_T) &&
                  !(dmasr_before & CH32_ETH_DMA_IT_T);

    stash_before = s->rx_stash_len;
    ch32_eth_deliver_rx_stash(s);
    rx_had_work = (stash_before != 0 && s->rx_stash_len == 0);
    /*
     * 仅当 deliver 成功清空了 stash（stash_before!=0 && 现在=0）时才 resume；
     * 否则（deliver 失败，stash 仍非空）不调用，避免描述符未归还
     * 时就让后续包进来造成级联失败。等固件写 DMARPDR 后再次来的 BH 里再 resume。
     */
    if (rx_had_work) {
        ch32_eth_rx_resume(s);
    }
    /*
     * 退避计数：有实际 TX/RX 工作则清零（恢复快速轮询），否则累加。
     */
    if (tx_had_work || rx_had_work) {
        s->poll_idle_count = 0;
    } else {
        if (s->poll_idle_count < CH32_ETH_POLL_IDLE_THR + 1u) {
            s->poll_idle_count++;
        }
    }
    /*
     * BH 完成后重新同步中断：
     * 若 BH 期间设置了新的 DMASR 事件位（如 TX 完成或 RX 完成），
     * 但 Guest 正在处理上一次中断（irq_active=true），新事件不会
     * 在 dmasr_raise 中注入 MEIP。BH 结束后若 irq_active=false（Guest
     * 已 mret），需要重新检查是否应该注入 MEIP。
     *
     * 场景：Guest mret → MEIP 撤销 → 但 DMASR 仍有未处理的 R/T 位
     * 此时需要重新注入 MEIP，否则中断会丢失。
     */
    ch32_eth_update_irq(s);
}

static void ch32_eth_poll_tick(void *opaque)
{
    Ch32EthState *s = opaque;
    uint32_t omr = s->mmio_w[CH32_ETH_OFF_DMAOMR / 4];
    uint64_t interval_ns;

    /*
     * PHY PHYLINK 中断注入（仅 10M 内置 PHY）：
     * EVT eth_driver_10M.c 在 WCHNET_ETHIsr() 中检测 ETH_DMA_IT_PHYLINK
     * 位来调用 ETH_PHYLink()，而不是轮询 BSR。
     * 当 phy_link_up_deadline_ns 到期（BSR 开始返回 link-up）时，
     * 主动设置 DMASR PHYLINK 位并触发 ETH 中断，使固件完成 PHY link-up 配置。
     */
    if (ch32_eth_is_phy10m_path(s) && !s->phy_link_irq_injected &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->phy_link_up_deadline_ns) {
        s->phy_link_irq_injected = true;
        ch32_eth_dmasr_raise(s, CH32_ETH_DMA_IT_PHYLINK);
    }

    if ((omr & (CH32_ETH_DMAOMR_ST | CH32_ETH_DMAOMR_SR)) == 0) {
        /*
         * DMA 未启动：
         * 10M 模式下保持 poll timer 持续运行，处理以下场景：
         * 1. deadline 未到：等待到期后注入 PHYLINK 中断
         * 2. deadline 到期且已注入：等待 BCR 软复位（固件 ETH_PHYLink
         *    失败后会重置 deadline + phy_link_irq_injected=false）
         */
        if (ch32_eth_is_phy10m_path(s)) {
            timer_mod(&s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CH32_ETH_POLL_FAST_NS);
            return;
        }
        s->poll_active = false;
        s->poll_idle_count = 0;
        return;
    }
    /*
     * 退避轮询：连续 CH32_ETH_POLL_IDLE_THR 次 BH 无实际工作后切换到慢速，
     * 降低 CPU 占用。有流量时（net_receive 触发 bh_schedule）不依赖此 timer。
     */
    interval_ns = (s->poll_idle_count >= CH32_ETH_POLL_IDLE_THR)
                  ? CH32_ETH_POLL_SLOW_NS : CH32_ETH_POLL_FAST_NS;
    qemu_bh_schedule(s->bh);
    timer_mod(&s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + interval_ns);
}

static void ch32_eth_poll_arm(Ch32EthState *s)
{
    if (s->poll_active) {
        return;
    }
    s->poll_active = true;
    s->poll_idle_count = 0;
    timer_mod(&s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CH32_ETH_POLL_FAST_NS);
}

static ssize_t ch32_eth_net_receive(NetClientState *nc, const uint8_t *buf,
                                    size_t size)
{
    Ch32EthState *s = qemu_get_nic_opaque(nc);

    if (size == 0 || size > CH32_ETH_PKT_MAX) {
        return -1;
    }
    /*
     * 仅在 guest 已启动 DMA 接收且配置了描述符环时暂存帧；否则 NIC 仍会收到
     * 宿主机报文，会把数据写到未初始化的 rd2，冲掉 SRAM 中的内核对象。
     */
    if ((s->mmio_w[CH32_ETH_OFF_DMAOMR / 4] & CH32_ETH_DMAOMR_SR) == 0) {
        /*
         * DMA 接收未启动：丢弃包但返回 size（而非 0），
         * 避免 tap 进入 read_poll=false 暂停状态。
         */
        return (ssize_t)size;
    }
    if (s->mmio_w[CH32_ETH_OFF_DMARDLAR / 4] == 0) {
        return (ssize_t)size;
    }
    if (s->rx_stash_len != 0) {
        /*
         * stash 中已有一帧未投递（上次 deliver 找不到可用描述符），
         * 返回 0 通知 tap 暂停读取，等待下次 poll 重试投递后再收包。
         */
        return 0;
    }
    memcpy(s->rx_stash, buf, size);
    s->rx_stash_len = (uint32_t)size;
    qemu_bh_schedule(s->bh);
    return (ssize_t)size;
}

static bool ch32_eth_net_can_receive(NetClientState *nc)
{
    Ch32EthState *s = qemu_get_nic_opaque(nc);

    /*
     * 仅检查 stash 是否已有一帧待投递，不检查 DMAOMR_SR。
     * 原因：若 can_receive 返回 false， QEMU tap 后端会暂停读取
     * （tap_read_poll(false)），后续无法通过空队列 flush 来恢复，
     * 导致 WDT 复位后 tap 永久暂停。对 DMAOMR_SR=0 的情况在
     * receive() 内返回 size 并丢弃包即可（不占 stash）。
     */
    return s->rx_stash_len == 0;
}

static NetClientInfo ch32_eth_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = ch32_eth_net_receive,
    .can_receive = ch32_eth_net_can_receive,
};

static uint32_t ch32_eth_dmasr_read(const Ch32EthState *s)
{
    uint32_t v = s->mmio_w[CH32_ETH_OFF_DMASR / 4];
    uint32_t omr = s->mmio_w[CH32_ETH_OFF_DMAOMR / 4];

    /*
     * 动态计算 NIS/AIS 汇总位：
     * NIS = Normal 中断源位（R, T 等）的 OR
     * AIS = Abnormal 中断源位（RBUS, TBUS, ER 等）的 OR
     * 这些位是只读的，固件 W1C 清它们无效——底层事件位清除后自动归零。
     */
    if (v & CH32_ETH_DMASR_NIS_SRC) {
        v |= CH32_ETH_DMA_IT_NIS;
    } else {
        v &= ~CH32_ETH_DMA_IT_NIS;
    }
    if (v & CH32_ETH_DMASR_AIS_SRC) {
        v |= CH32_ETH_DMA_IT_AIS;
    } else {
        v &= ~CH32_ETH_DMA_IT_AIS;
    }

    /*
     * DMASR 中 TPS(20:22)/RPS(17:19) 反映收发状态机；库与驱动会读此域。
     * 模型在 ST/SR 置位时给出「运行中」典型编码，其余为 Stopped。
     */
    v &= ~CH32_ETH_DMASR_TPS_M;
    if (omr & CH32_ETH_DMAOMR_ST) {
        v |= CH32_ETH_DMASR_TPS_FETCH;
    }
    v &= ~CH32_ETH_DMASR_RPS_M;
    if (omr & CH32_ETH_DMAOMR_SR) {
        v |= CH32_ETH_DMASR_RPS_WAIT;
    }
    return v;
}

static uint32_t ch32_eth_mmio_read_word(Ch32EthState *s, hwaddr addr)
{
    unsigned wi;

    if (addr >= CH32_ETH_MMIO_SIZE || (addr & 3)) {
        return 0;
    }
    wi = (unsigned)(addr / 4);
    if (addr == CH32_ETH_OFF_DMASR) {
        return ch32_eth_dmasr_read(s);
    }
    return s->mmio_w[wi];
}

static uint64_t ch32_eth_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32EthState *s = opaque;
    uint32_t w;

    if (addr + size > CH32_ETH_MMIO_SIZE || (addr & (size - 1))) {
        return 0;
    }
    w = ch32_eth_mmio_read_word(s, addr & ~3ull);
    if (size == 4) {
        return w;
    }
    if (size == 2) {
        return (w >> ((addr & 2) * 8)) & 0xffffu;
    }
    return (w >> ((addr & 3) * 8)) & 0xffu;
}

static void ch32_eth_mmio_write_word(Ch32EthState *s, hwaddr addr, uint32_t val)
{
    unsigned wi;

    if (addr >= CH32_ETH_MMIO_SIZE || (addr & 3)) {
        return;
    }
    wi = (unsigned)(addr / 4);

    if (addr == CH32_ETH_OFF_MACMIIAR) {
        s->mmio_w[wi] = val;
        if (val & CH32_ETH_MACMIIAR_MB) {
            ch32_eth_mdio_run(s, &s->mmio_w[wi], &s->mmio_w[CH32_ETH_OFF_MACMIIDR / 4]);
        }
        return;
    }
    if (addr == CH32_ETH_OFF_DMABMR) {
        if (val & CH32_ETH_DMABMR_SR) {
            /* 软件复位：立即清 SR，避免固件在 while(DMABMR&SR) 中阻塞 */
            val &= (uint32_t)~CH32_ETH_DMABMR_SR;
        }
        s->mmio_w[wi] = val;
        return;
    }
    if (addr == 0x100u) {
        /*
         * MMCCR（MAC MMC Control Register）：CR 位（bit0）写 1 启动计数器复位，
         * 硬件完成后自动清零。QEMU 立即清零，避免固件 while(ETH->MMCCR&CR) 死循环。
         */
        val &= ~0x00000001u;  /* 清 CR 位（counters reset 瞬间完成）*/
        s->mmio_w[wi] = val;
        return;
    }
    if (addr == CH32_ETH_OFF_DMASR) {
        s->mmio_w[wi] &= ~(val & CH32_ETH_DMASR_W1C_MASK);
        /* 固件清标志后重新同步中断电平 */
        ch32_eth_update_irq(s);
        return;
    }
    if (addr == CH32_ETH_OFF_DMATPDR || addr == CH32_ETH_OFF_DMARPDR) {
        s->mmio_w[wi] = val;
        qemu_bh_schedule(s->bh);
        return;
    }
    if (addr == CH32_ETH_OFF_DMAOMR) {
        /*
         * FTF：Flush TX FIFO，真机完成后清 0；模型立即清除，避免
         * ETH_FlushTransmitFIFO 后固件自旋等待。
         */
        val &= (uint32_t)~CH32_ETH_DMAOMR_FTF;
        s->mmio_w[wi] = val;
        if (val & (CH32_ETH_DMAOMR_ST | CH32_ETH_DMAOMR_SR)) {
            ch32_eth_poll_arm(s);
            /*
             * SR 刚被启动（固件初始化完成 RX DMA）：尝试恢复 tap 读取并投递所有已排队的包。
             * ch32_eth_rx_resume 在 flush 基础上额外调用 peer->poll(true)
             * 确保 tap fd 的 read_poll 被恢复，即使队列为空。
             */
            ch32_eth_rx_resume(s);
        } else {
            /*
             * DMA 停止（ST=SR=0）：
             * 10M 模式下 poll timer 用于检测 PHY PHYLINK deadline，
             * 不能因 DMA 未启动就删除——否则 ETH_Init 中途写 DMAOMR
             *（只设配置位，不含 ST/SR）会删掉等待注入的 poll timer。
             * 只有在非 10M 模式下才停止 poll。
             */
            if (!ch32_eth_is_phy10m_path(s)) {
                timer_del(&s->poll);
                s->poll_active = false;
            }
        }
        return;
    }
    s->mmio_w[wi] = val;
}

static void ch32_eth_mmio_write(void *opaque, hwaddr addr, uint64_t val64,
                                unsigned size)
{
    Ch32EthState *s = opaque;
    hwaddr waddr = addr & ~3ull;
    unsigned wi = (unsigned)(waddr / 4);
    uint32_t cur = s->mmio_w[wi];
    uint32_t val = (uint32_t)val64;

    if (addr + size > CH32_ETH_MMIO_SIZE || (addr & (size - 1))) {
        return;
    }
    if (size == 4) {
        ch32_eth_mmio_write_word(s, addr, val);
        return;
    }
    if (size == 2) {
        uint32_t m = 0xffffu << ((addr & 2) * 8);

        ch32_eth_mmio_write_word(s, waddr, (cur & ~m) | ((val & 0xffffu) << ((addr & 2) * 8)));
        return;
    }
    {
        uint32_t m = 0xffu << ((addr & 3) * 8);

        ch32_eth_mmio_write_word(s, waddr, (cur & ~m) | ((val & 0xffu) << ((addr & 3) * 8)));
    }
}

static const MemoryRegionOps ch32_eth_mmio_ops = {
    .read = ch32_eth_mmio_read,
    .write = ch32_eth_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * IWDG 等触发 qemu_system_reset_request 时，若不清空 DMA 影子寄存器与 poll/BH，
 * 复位后固件尚未重建描述符环，模型仍可能按旧 DMARDLAR/DMATDLAR 写 SRAM，
 * 破坏堆栈并表现为 ETH 打印中途反复「OrayOS-Tiny Starting...」。
 */
static void ch32_eth_reset(DeviceState *dev)
{
    Ch32EthState *s = CH32_ETH_DWMAC(dev);

    timer_del(&s->poll);
    s->poll_active = false;
    if (s->bh) {
        qemu_bh_cancel(s->bh);
    }
    memset(s->mmio_w, 0, sizeof(s->mmio_w));
    memset(s->phy_reg, 0, sizeof(s->phy_reg));
    /* PHY BCR 上电默认值：PHY10M=0x1000、RTL8211F=0x1140、CH182=0x3100 */
    s->phy_bcr = ch32_eth_phy_bcr_reset(s);
    s->phy_bsr = 0x7809u; /* link-down 初始值 */
    s->rx_stash_len = 0;
    s->irq_active = false;
    /*
     * PHY link-up 延迟：500ms 后认为链路建立。
     * 这使得 ETH_Configuration() 初始化时读到 link-down，
     * 而后续的 WCHNET_QueryPhySta() 读到 link-up 触发 ETH_PHYLink()。
     */
    s->phy_link_up_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500 * 1000000ULL;
    s->phy_link_irq_injected = false;
    /* 复位时清理 RTL8211F 扩展寄存器状态 */
    s->rtl8211f_page = 0x0000u;
    s->rtl8211f_mmd_ctrl = 0x0000u;
    s->rtl8211f_mmd_addr = 0x0000u;
    /* 复位时清零 chain 游标，下次 DMA 启动时重新从 DMATDLAR/DMARDLAR 初始化 */
    s->tx_cur_desc = 0;
    s->rx_cur_desc = 0;
    /* 复位后恢复 tap 读取轮询，确保 WDT 复位后 NIC 能立即接收新包 */
    if (s->nic) {
        ch32_eth_rx_resume(s);
    }
    /* 10M 模式：复位后重新启动 poll timer，以便检测新的 PHYLINK deadline */
    if (ch32_eth_is_phy10m_path(s)) {
        s->poll_active = true;
        timer_mod(&s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CH32_ETH_POLL_FAST_NS);
    }
    /* 复位时撤销 ETH 中断 */
    ch32_eth_meip_resync(s->machine);
}

static void ch32_eth_realize(DeviceState *dev, Error **errp)
{
    Ch32EthState *s = CH32_ETH_DWMAC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &ch32_eth_mmio_ops, s,
                          "ch32-eth-dwmac-mmio", CH32_ETH_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);

    s->nic = qemu_new_nic(&ch32_eth_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    s->bh = qemu_bh_new(ch32_eth_bh, s);
    timer_init_ns(&s->poll, QEMU_CLOCK_VIRTUAL, ch32_eth_poll_tick, s);

    /*
     * 解析 phy-model 字符串属性，填充 phy_variant。
     * 规则：phy-model 非空时按 "rtl8211f|phy10m|ch182" 匹配，未知值拒绝
     * realize；为空时默认 CH182（与 V317 旧行为一致）。
     */
    if (s->phy_model != NULL) {
        if (!strcmp(s->phy_model, "rtl8211f")) {
            s->phy_variant = CH32_ETH_PHY_RTL8211F;
        } else if (!strcmp(s->phy_model, "phy10m")) {
            s->phy_variant = CH32_ETH_PHY_PHY10M;
        } else if (!strcmp(s->phy_model, "ch182")) {
            s->phy_variant = CH32_ETH_PHY_CH182;
        } else {
            error_setg(errp,
                       "ch32-eth-dwmac: unknown phy-model '%s'"
                       " (expected ch182|phy10m|rtl8211f)",
                       s->phy_model);
            return;
        }
    } else {
        s->phy_variant = CH32_ETH_PHY_CH182;
    }

    s->phy_bsr = 0x7809u;
    /* phy_bcr 复位值依据 phy_variant 决定（PHY10M=0x1000、RTL8211F=0x1140、CH182=0x3100）*/
    s->phy_bcr = ch32_eth_phy_bcr_reset(s);
    s->phy_link_up_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500 * 1000000ULL;
    /*
     * 10M 模式：realize 时立即启动 poll timer，以便在
     * DMA 启动前就能检测 PHY link-up deadline 并注入 PHYLINK 中断。
     * EVT 固件的 PHY 初始化在 ETH_LibInit 中，它启动 DMA。
     */
    if (ch32_eth_is_phy10m_path(s)) {
        s->poll_active = true;
        timer_mod(&s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CH32_ETH_POLL_FAST_NS);
    }
}

static void ch32_eth_finalize(Object *obj)
{
    Ch32EthState *s = CH32_ETH_DWMAC(obj);

    timer_del(&s->poll);
    if (s->bh) {
        qemu_bh_delete(s->bh);
        s->bh = NULL;
    }
    if (s->nic) {
        qemu_del_nic(s->nic);
        s->nic = NULL;
    }
}

static const Property ch32_eth_properties[] = {
    DEFINE_NIC_PROPERTIES(Ch32EthState, conf),
    DEFINE_PROP_STRING("phy-model", Ch32EthState, phy_model),
};

static void ch32_eth_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ch32_eth_realize;
    device_class_set_legacy_reset(dc, ch32_eth_reset);
    device_class_set_props(dc, ch32_eth_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo ch32_eth_type_info = {
    .name = TYPE_CH32_ETH_DWMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Ch32EthState),
    .instance_finalize = ch32_eth_finalize,
    .class_init = ch32_eth_class_init,
};

static void ch32_eth_register_types(void)
{
    type_register_static(&ch32_eth_type_info);
}

type_init(ch32_eth_register_types)

/*
 * ch32_eth_set_machine - 设置 ETH 外设与 machine 的双向关联。
 * 由 ch32-v.c 在 machine init 中调用，使 ch32_eth_meip_resync 能访问 PFIC/CPU。
 */
void ch32_eth_set_machine(DeviceState *eth_dev, Ch32MachineState *m)
{
    Ch32EthState *s = CH32_ETH_DWMAC(eth_dev);

    s->machine = m;
    m->eth_state = s;
}
