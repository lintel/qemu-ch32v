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
 * File:     ch32-usb-fs-host.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32V30x USBFS 主机（USB Full-Speed Host）：将 MMIO 上的 HOST_EP_PID
 *     事务桥接到 QEMU 通用 USB 栈。寄存器布局与 EVT ch32v30x.h USBFSH_TypeDef
 *     (packed) 完全对齐：
 *       BASE_CTRL@0x00, HOST_CTRL@0x01, INT_EN@0x02, DEV_ADDR@0x03
 *       MIS_ST@0x05,   INT_FG@0x06,   INT_ST@0x07
 *       RX_LEN@0x08(u16), HOST_RX_DMA@0x18(u32), HOST_TX_DMA@0x1C(u32)
 *       HOST_SETUP@0x32(u16), HOST_EP_PID@0x34(u8)
 *       HOST_RX_CTRL@0x37(u8), HOST_TX_LEN@0x38(u16), HOST_TX_CTRL@0x3A(u8)
 *
 *     完成中断经 MIP_MEIP + wch_evt_mcause_override（NVIC 83）在 VTF 下进入
 *     USBFS 向量；与 USBHS 路径结构完全对称，仅在寄存器偏移/位定义上差异。
 *
 *     说明：原 ch32-usb-host.c 曾同时承载 USBHS + USBFS 两套主机，为便于后续
 *     CH32V407（两个 USBHS 控制器 + 一个 USBFS）扩展为多实例，已将 USBHS 与
 *     USBFS 物理分文件：
 *       - ch32-usb-hs-host.c：USBHS Host（IRQ 85）
 *       - 本文件 ch32-usb-fs-host.c：USBFS Host（IRQ 83）
 *     两文件互不依赖，所有共享 helper（pid4→token、dma_ok）均本地独立副本。
 */

#include "ch32-machine-internal.h"
#include "hw/core/cpu.h"
#include "qemu/iov.h"
#include "target/riscv/cpu_bits.h"

/* USBFSH INT_ST 位（与 ch32v30x_usb.h 一致） */
#define CH32_USBFS_UIS_IS_NAK      0x80u
#define CH32_USBFS_UIS_TOG_OK      0x40u
#define CH32_USBFS_UIS_TOKEN_OUT   0x00u
#define CH32_USBFS_UIS_TOKEN_IN    0x20u
#define CH32_USBFS_UIS_TOKEN_SETUP 0x30u
#define CH32_USBFS_PID_ACK         0x02u
#define CH32_USBFS_PID_NAK         0x0au
#define CH32_USBFS_PID_STALL       0x0eu
#define CH32_USBFS_PID_DATA0       0x03u
#define CH32_USBFS_PID_DATA1       0x0bu  /* 手册 23 MASK_UIS_H_RES DATA1 */

/*
 * R8_UH_RX_CTRL / R8_UH_TX_CTRL 位定义（手册 23.2.3.16 / 23.2.3.17，
 * 与 reference-project usb_ch32_usbfs_reg.h 对齐）：
 *   bit 3: R_AUTO_TOG / T_AUTO_TOG —— 成功后硬件自动翻转 toggle
 *   bit 2: R_TOG      / T_TOG      —— 期望/发送 DATAx（0=DATA0，1=DATA1）
 *   bit 0: R_RES      / T_RES      —— 握手类型（ACK / NO-RESPONSE）
 * 注：USBFS 较 USBHS 简化，不存在 DATA_NO / DATA2 概念。
 */
#define CH32_USBFS_UH_R_AUTO_TOG   0x08u
#define CH32_USBFS_UH_R_TOG        0x04u
#define CH32_USBFS_UH_T_AUTO_TOG   0x08u
#define CH32_USBFS_UH_T_TOG        0x04u

/*
 * USBFSH 寄存器偏移已由 ch32-machine-internal.h 统一定义（表 23-1/23-5），
 * 本文件不再重复声明。历史上为避免与 ch32-usb.c 容器包装
 * 宏重名冲突而添加的 _X 后缀已清除。
 *
 * USBFS 位掩码（UC_HOST_MODE/UH_BUS_RESET/UIF_* 等）保持本地定义，
 * 等后续需要跨文件共享时再一次性上提。
 */
#define CH32_USBFS_UH_BUS_RESET       0x02u  /* HOST_CTRL bit1 */
#define CH32_USBFS_UC_HOST_MODE       0x80u  /* BASE_CTRL bit7 */
#define CH32_USBFS_UIF_TRANSFER       0x02u  /* INT_FG bit1 */
#define CH32_USBFS_UIF_DETECT         0x01u  /* INT_FG bit0 */
#define CH32_USBFS_UMS_DEV_ATTACH     0x01u  /* MIS_ST bit0 */
#define CH32_USBFS_NVIC_IRQ           83u

/*
 * USBFS Host 本地 helper：不依赖 USBHS 文件，保持拆分后两个文件完全独立。
 * （将来 CH32V407 若需要 USBFS 多实例化，这些 helper 的本地化也便于直接
 *  演化为参数化形式，而不影响 USBHS。）
 */
static bool ch32_usbfs_dma_ok(MachineState *ms, hwaddr gpa, size_t len)
{
    hwaddr ram_sz = memory_region_size(ms->ram);

    if (len > 4096) {
        return false;
    }
    /* len==0：控制传输状态阶段的 OUT 不访问 DMA 缓冲区。 */
    if (len == 0) {
        return true;
    }
    if (gpa < CH32_SRAM_BASE || gpa + len > CH32_SRAM_BASE + ram_sz) {
        return false;
    }
    return true;
}

static int ch32_usbfs_pid4_to_token(unsigned pid4)
{
    switch (pid4 & 0xfu) {
    case 0xd:
        return USB_TOKEN_SETUP;
    case 0x9:
        return USB_TOKEN_IN;
    case 0x1:
        return USB_TOKEN_OUT;
    default:
        return -1;
    }
}

void ch32_usbfs_host_meip_resync(Ch32MachineState *m)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;
    const unsigned n = CH32_USBFS_NVIC_IRQ;
    const unsigned wd = n / 32u;
    const unsigned bit = n % 32u;
    bool line_on;
    bool need_mip;

    if (!m->ch32_usbfs_host_inited) {
        return;
    }
    line_on = (m->usbfs_reg[CH32_USBFSH_OFF_BASE_CTRL] & CH32_USBFS_UC_HOST_MODE) != 0 &&
              wd < sizeof(m->pfic_ienr) / sizeof(m->pfic_ienr[0]) &&
              ((m->pfic_ienr[wd] >> bit) & 1u);

    BQL_LOCK_GUARD();
    if (!line_on) {
        env->mie &= ~MIP_MEIP;
        if (env->wch_evt_mcause_override == n) {
            riscv_cpu_update_mip(env, MIP_MEIP, 0);
            env->wch_evt_mcause_override = 0;
            ch32_pfic_irq_active_clear(m, n);
        }
        riscv_cpu_interrupt(env);
        qemu_cpu_kick(CPU(cpu));
        return;
    }
    env->mie |= MIP_MEIP;

    need_mip = ((uint32_t)(m->usbfs_reg[CH32_USBFSH_OFF_INT_FG] &
                           m->usbfs_reg[CH32_USBFSH_OFF_INT_EN]) &
                (CH32_USBFS_UIF_TRANSFER | CH32_USBFS_UIF_DETECT)) != 0;
    if (need_mip) {
        /*
         * MEIP 仲裁：如果其他外设已持有 MEIP override，则不覆盖。
         * 等对方 IRQ 处理完调用 ch32_pfic_resync_pending_meip 时，
         * 会重新进入本函数完成注入（与 ETH/ETH10M/TIM 路径一致）。
         */
        if (env->wch_evt_mcause_override != 0u &&
            env->wch_evt_mcause_override != n) {
            return;
        }
        env->wch_evt_mcause_override = n;
        ch32_pfic_irq_active_set(m, n);
        riscv_cpu_update_mip(env, MIP_MEIP, MIP_MEIP);
    } else if (env->wch_evt_mcause_override == n) {
        riscv_cpu_update_mip(env, MIP_MEIP, 0);
        env->wch_evt_mcause_override = 0;
        ch32_pfic_irq_active_clear(m, n);
        /* USBFS-Host owner 释放：重新扫描其他 pending MEIP 外设 */
        ch32_pfic_resync_pending_meip(m);
    }
    riscv_cpu_interrupt(env);
    qemu_cpu_kick(CPU(cpu));
}

static void ch32_usbfs_host_raise_transfer(Ch32MachineState *m)
{
    m->usbfs_reg[CH32_USBFSH_OFF_INT_FG] |= CH32_USBFS_UIF_TRANSFER;
    ch32_usbfs_host_meip_resync(m);
}

static void ch32_usbfs_host_raise_detect(Ch32MachineState *m)
{
    m->usbfs_reg[CH32_USBFSH_OFF_INT_FG] |= CH32_USBFS_UIF_DETECT;
    ch32_usbfs_host_meip_resync(m);
}

static uint8_t ch32_usbfs_int_st_token(int token)
{
    switch (token) {
    case USB_TOKEN_SETUP: return CH32_USBFS_UIS_TOKEN_SETUP;
    case USB_TOKEN_IN:    return CH32_USBFS_UIS_TOKEN_IN;
    case USB_TOKEN_OUT:   return CH32_USBFS_UIS_TOKEN_OUT;
    default:              return 0;
    }
}

static void ch32_usbfs_apply_pkt_result(Ch32MachineState *m, int token,
                                        int status, int actual_in)
{
    uint8_t tok = ch32_usbfs_int_st_token(token);
    uint8_t ist = tok;

    if (status == USB_RET_SUCCESS) {
        if (token == USB_TOKEN_IN) {
            /*
             * IN 成功：将固件写入 RX_CTRL.R_TOG 的期望值反射到
             * INT_ST[3:0] 的 PID 字段。TOG_OK 恒为 1（透传场景同 USBHS）。
             */
            uint8_t rx_ctrl =
                m->usbfs_reg[CH32_USBFSH_OFF_HOST_RX_CTRL];
            uint8_t pid_data = (rx_ctrl & CH32_USBFS_UH_R_TOG) ?
                                   CH32_USBFS_PID_DATA1 :
                                   CH32_USBFS_PID_DATA0;
            ist = tok | CH32_USBFS_UIS_TOG_OK | pid_data;
        } else {
            ist = tok | CH32_USBFS_PID_ACK;
        }
    } else if (status == USB_RET_NAK) {
        ist = tok | CH32_USBFS_PID_NAK | CH32_USBFS_UIS_IS_NAK;
    } else if (status == USB_RET_STALL) {
        ist = tok | CH32_USBFS_PID_STALL;
    }
    m->usbfs_reg[CH32_USBFSH_OFF_INT_ST] = ist;
    stw_le_p(m->usbfs_reg + CH32_USBFSH_OFF_RX_LEN,
             (status == USB_RET_SUCCESS && token == USB_TOKEN_IN) ?
                 (uint16_t)actual_in : 0);
    ch32_usbfs_host_raise_transfer(m);
}

static void ch32_usbfs_copy_in_to_guest(Ch32MachineState *m, USBPacket *p)
{
    MachineState *ms = MACHINE(OBJECT(m));
    hwaddr rxdma = ldl_le_p(m->usbfs_reg + CH32_USBFSH_OFF_HOST_RX_DMA);
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    int len = p->actual_length;
    uint8_t tmp[4096];
    size_t n;

    if (len <= 0 || p->pid != USB_TOKEN_IN) {
        return;
    }
    if (!ch32_usbfs_dma_ok(ms, rxdma, (size_t)len)) {
        p->status = USB_RET_IOERROR;
        return;
    }
    n = qemu_iovec_to_buf(&p->iov, 0, tmp, sizeof(tmp));
    if (n < (size_t)len) {
        p->status = USB_RET_IOERROR;
        return;
    }
    if (address_space_write(&address_space_memory, rxdma, attrs, tmp, len)
        != MEMTX_OK) {
        p->status = USB_RET_IOERROR;
    }
}

static void ch32_usbfs_rh_attach(USBPort *port)
{
    Ch32MachineState *m = port->opaque;

    m->usbfs_reg[CH32_USBFSH_OFF_MIS_ST] |= CH32_USBFS_UMS_DEV_ATTACH;
    ch32_usbfs_host_raise_detect(m);
}

static void ch32_usbfs_rh_detach(USBPort *port)
{
    Ch32MachineState *m = port->opaque;

    m->usbfs_reg[CH32_USBFSH_OFF_MIS_ST] &= (uint8_t)~CH32_USBFS_UMS_DEV_ATTACH;
    ch32_usbfs_host_raise_detect(m);
}

static void ch32_usbfs_rh_child_detach(USBPort *port, USBDevice *child)
{
    (void)port;
    (void)child;
}

static void ch32_usbfs_rh_wakeup(USBPort *port)
{
    (void)port;
}

static void ch32_usbfs_rh_complete(USBPort *port, USBPacket *p)
{
    Ch32MachineState *m = port->opaque;

    if (p != &m->ch32_usbfs_pkt) {
        return;
    }
    if (p->pid == USB_TOKEN_IN && p->status == USB_RET_SUCCESS) {
        ch32_usbfs_copy_in_to_guest(m, p);
    }
    ch32_usbfs_apply_pkt_result(m, p->pid, p->status, p->actual_length);
    usb_packet_cleanup(p);
}

static USBPortOps ch32_usbfs_rhport_ops = {
    .attach = ch32_usbfs_rh_attach,
    .detach = ch32_usbfs_rh_detach,
    .child_detach = ch32_usbfs_rh_child_detach,
    .wakeup = ch32_usbfs_rh_wakeup,
    .complete = ch32_usbfs_rh_complete,
};

static void ch32_usbfs_bus_wakeup_ep(USBBus *bus, USBEndpoint *ep,
                                     unsigned int stream)
{
    (void)bus;
    (void)ep;
    (void)stream;
}

static USBBusOps ch32_usbfs_bus_ops = {
    .wakeup_endpoint = ch32_usbfs_bus_wakeup_ep,
};

/* 哑端口 ops：什么也不做，仅用于占位使 bus->nfree>=2 */
static void ch32_usbfs_dummy_attach(USBPort *port) { (void)port; }
static void ch32_usbfs_dummy_detach(USBPort *port) { (void)port; }
static void ch32_usbfs_dummy_child_detach(USBPort *port, USBDevice *c) { (void)port; (void)c; }
static void ch32_usbfs_dummy_wakeup(USBPort *port) { (void)port; }
static void ch32_usbfs_dummy_complete(USBPort *port, USBPacket *p) { (void)port; (void)p; }

static USBPortOps ch32_usbfs_dummy_port_ops = {
    .attach        = ch32_usbfs_dummy_attach,
    .detach        = ch32_usbfs_dummy_detach,
    .child_detach  = ch32_usbfs_dummy_child_detach,
    .wakeup        = ch32_usbfs_dummy_wakeup,
    .complete      = ch32_usbfs_dummy_complete,
};

void ch32_usbfs_host_resync_root_mmio(Ch32MachineState *m)
{
    USBDevice *d;

    if (!m->ch32_usbfs_host_inited) {
        return;
    }
    d = m->ch32_usbfs_rhport.dev;
    if (d == NULL || !d->attached) {
        return;
    }
    /*
     * 复位后或固件写 INT_EN 前已清 INT_FG，重新为固件啇进设备连接事件。
     * 设置 MIS_ST.DEV_ATTACH + INT_FG.UIF_DETECT，凝拟硬件连接事件。
     */
    m->usbfs_reg[CH32_USBFSH_OFF_MIS_ST]  |= CH32_USBFS_UMS_DEV_ATTACH;
    m->usbfs_reg[CH32_USBFSH_OFF_INT_FG]  |= CH32_USBFS_UIF_DETECT;
    ch32_usbfs_host_meip_resync(m);
}

void ch32_usbfs_host_machine_init(Ch32MachineState *m)
{
    if (m->ch32_usbfs_host_inited) {
        return;
    }
    /*
     * USBFS Host 使用独立的 USBBus（与 USBHS 的 ch32_usb_bus 分开）。
     * 注册 2 个端口：根端口 + 哑端口。
     * 哑端口使 bus->nfree>=2，避免 usb_claim_port 在根端口剩一个空闲口时
     * 自动创建 usb-hub（与 USBHS bus 上的 hub 产生 compat vmstate 冲突）。
     */
    usb_bus_new(&m->ch32_usbfs_bus, sizeof(m->ch32_usbfs_bus),
                &ch32_usbfs_bus_ops, DEVICE(&m->cpus));
    /* 根端口：真实 USB 事务 */
    usb_register_port(&m->ch32_usbfs_bus, &m->ch32_usbfs_rhport, m, 0,
                      &ch32_usbfs_rhport_ops,
                      USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL);
    /* 哑端口：不报告任何事件，仅用于占位（使 bus->nfree>=2） */
    usb_register_port(&m->ch32_usbfs_bus, &m->ch32_usbfs_dummy_port, m, 1,
                      &ch32_usbfs_dummy_port_ops,
                      USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL);
    /*
     * 不再硬编码默认设备（原 usb-tablet）：用户通过 -device 按需挂载。
     * bus->nfree==2，-device 不会触发自动 hub 创建。
     */
    usb_packet_init(&m->ch32_usbfs_pkt);
    m->ch32_usbfs_pkt_inited = true;
    m->ch32_usbfs_host_inited = true;
}

void ch32_usbfs_host_machine_finalize(Ch32MachineState *m)
{
    if (m->ch32_usbfs_pkt_inited) {
        usb_packet_cleanup(&m->ch32_usbfs_pkt);
        m->ch32_usbfs_pkt_inited = false;
    }
    m->ch32_usbfs_host_inited = false;
}

void ch32_usbfs_host_ctrl_write(Ch32MachineState *m, uint8_t v)
{
    uint8_t prev = m->ch32_usbfs_last_host_ctrl;

    m->ch32_usbfs_last_host_ctrl = v;
    if (!m->ch32_usbfs_host_inited || !m->ch32_usbfs_rhport.dev) {
        return;
    }
    /*
     * 仅在 UH_BUS_RESET 0→1 边沿触发：与 USBHS 对称，避免多次 reset。
     */
    if ((v & CH32_USBFS_UH_BUS_RESET) &&
        !(prev & CH32_USBFS_UH_BUS_RESET)) {
        usb_device_reset(m->ch32_usbfs_rhport.dev);
    }
}

void ch32_usbfs_host_ep_pid_write(Ch32MachineState *m, uint8_t ep_pid)
{
    MachineState *ms = MACHINE(OBJECT(m));
    USBPacket *p = &m->ch32_usbfs_pkt;
    uint8_t devadr = m->usbfs_reg[CH32_USBFSH_OFF_DEV_ADDR];
    unsigned pid4 = (unsigned)(ep_pid >> 4) & 0xfu;
    int token = ch32_usbfs_pid4_to_token(pid4);
    int epnum = ep_pid & 0xfu;
    USBDevice *dev;
    USBEndpoint *ep;
    hwaddr txdma = ldl_le_p(m->usbfs_reg + CH32_USBFSH_OFF_HOST_TX_DMA);
    uint16_t txlen = lduw_le_p(m->usbfs_reg + CH32_USBFSH_OFF_HOST_TX_LEN);
    uint8_t buf[512];
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    MemTxResult mr;

    if (!m->ch32_usbfs_host_inited) {
        return;
    }
    if (token < 0) {
        ch32_usbfs_apply_pkt_result(m, USB_TOKEN_OUT, USB_RET_STALL, 0);
        return;
    }

    BQL_LOCK_GUARD();
    dev = usb_find_device(&m->ch32_usbfs_rhport, devadr);
    if (dev == NULL || !dev->attached) {
        ch32_usbfs_apply_pkt_result(m, token, USB_RET_NODEV, 0);
        return;
    }

    ep = usb_ep_get(dev, token, epnum);

    /*
     * 固件可能在上一个异步事务完成前再次写 HOST_EP_PID。
     * 先取消飞行中的包并清理，防止 usb_packet_setup 断言失败。
     * 对非 EP0（BULK/INT）返回 NAK，与真机行为一致（见 ch32-usb-hs-host.c 同位置注释）。
     */
    if (usb_packet_is_inflight(p)) {
        usb_cancel_packet(p);
        usb_packet_cleanup(p);
        if (epnum != 0) {
            ch32_usbfs_apply_pkt_result(m, token, USB_RET_NAK, 0);
            return;
        }
    }
    usb_packet_init(p);
    switch (token) {
    case USB_TOKEN_SETUP:
        if (!ch32_usbfs_dma_ok(ms, txdma, 8)) {
            p->status = USB_RET_IOERROR;
            break;
        }
        mr = address_space_read(&address_space_memory, txdma, attrs, buf, 8);
        if (mr != MEMTX_OK) {
            p->status = USB_RET_IOERROR;
            break;
        }
        usb_packet_setup(p, USB_TOKEN_SETUP, ep, 0, 0, false, false);
        usb_packet_addbuf(p, buf, 8);
        usb_handle_packet(dev, p);
        break;
    case USB_TOKEN_OUT:
        if (txlen > sizeof(buf) || !ch32_usbfs_dma_ok(ms, txdma, txlen)) {
            p->status = USB_RET_IOERROR;
            break;
        }
        mr = address_space_read(&address_space_memory, txdma, attrs, buf,
                                txlen);
        if (mr != MEMTX_OK) {
            p->status = USB_RET_IOERROR;
            break;
        }
        usb_packet_setup(p, USB_TOKEN_OUT, ep, 0, 0, false, false);
        usb_packet_addbuf(p, buf, txlen);
        usb_handle_packet(dev, p);
        break;
    case USB_TOKEN_IN: {
        size_t want = sizeof(buf);

        usb_packet_setup(p, USB_TOKEN_IN, ep, 0, 0, false, false);
        usb_packet_addbuf(p, buf, want);
        usb_handle_packet(dev, p);
        if (p->status == USB_RET_SUCCESS && p->actual_length > 0) {
            ch32_usbfs_copy_in_to_guest(m, p);
        }
        break;
    }
    default:
        g_assert_not_reached();
    }

    if (p->status == USB_RET_ASYNC) {
        return;
    }
    ch32_usbfs_apply_pkt_result(m, token, p->status, p->actual_length);
    usb_packet_cleanup(p);
}
