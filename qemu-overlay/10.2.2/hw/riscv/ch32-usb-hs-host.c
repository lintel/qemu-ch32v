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
 * File:     ch32-usb-hs-host.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32V30x/V407 USBHS 主机（USB High-Speed Host）：将 MMIO 上的 HOST_EP_PID
 *     事务桥接到 QEMU 通用 USB 栈。根端口上挂 usb-hub 及下游设备，供 CherryUSB
 *     等真实主机栈完成枚举。
 *
 *     事务在写 HOST_EP_PID 时多数为同步完成；若返回 USB_RET_ASYNC，由根端口
 *     complete 回调收尾。完成中断经 MIP_MEIP + wch_evt_mcause_override（NVIC 85）
 *     在 VTF 下进入 USBHS 向量（勿用 gpio≥64：riscv_cpu_set_irq 在无 RVH 时曾断言）。
 *
 *     说明：原 ch32-usb-host.c 曾同时承载 USBHS + USBFS 两套主机，为便于后续
 *     CH32V407（两个 USBHS 控制器 + 一个 USBFS）扩展为多实例，已将 USBHS 与
 *     USBFS 物理分文件：
 *       - 本文件 ch32-usb-hs-host.c：USBHS Host（IRQ 85）
 *       - ch32-usb-fs-host.c：       USBFS Host（IRQ 83）
 *     两文件互不依赖，所有共享 helper（ld32/ld16、pid4→token、dma_ok）均本地
 *     独立副本；后续重构 Ch32UsbhsHostState 多实例化时只需改本文件。
 */

#include "ch32-machine-internal.h"
#include "hw/core/cpu.h"
#include "qemu/iov.h"
#include "target/riscv/cpu_bits.h"

/*
 * USBHS 寄存器偏移已由 ch32-machine-internal.h 统一定义（表 22-1/22-3），
 * 本文件不再重复声明。全部 CH32_USBHS_OFF_* 旧称已改名为
 * CH32_USBHS_OFF_* 以与 USBFS 侧区别。
 */

/* R8_USB_INT_ST / 握手 PID — 与 EVT Peripheral/inc/ch32v30x_usb.h 一致 */
#define CH32_USBHS_UIS_IS_NAK       0x80u
#define CH32_USBHS_UIS_TOG_OK       0x40u
#define CH32_USBHS_UIS_TOKEN_OUT    0x00u
#define CH32_USBHS_UIS_TOKEN_IN     0x20u
#define CH32_USBHS_UIS_TOKEN_SETUP  0x30u

#define CH32_USB_PID_ACK            0x02u
#define CH32_USB_PID_NAK            0x0au
#define CH32_USB_PID_STALL          0x0eu
#define CH32_USB_PID_DATA0          0x03u
#define CH32_USB_PID_DATA1          0x0bu  /* 手册 22.2.3.7 MASK_UIS_H_RES DATA1 */

/*
 * R8_UH_RX_CTRL / R8_UH_TX_CTRL 位定义（手册 22.2.3.9 / 22.2.3.10，
 * 与 reference-project usb_ch32_usbhs_reg.h 对齐）：
 *   bit 6: R_DATA_NO / T_DATA_NO  —— IN 不期待数据 / OUT 只发 TOKEN（PING）
 *   bit 5: R_AUTO_TOG / T_AUTO_TOG —— 成功后硬件自动翻转 toggle
 *   bits [4:3]: R_TOG / T_TOG      —— 期望/发送 DATAx（0=DATA0, 1=DATA1, 2=DATA2）
 */
#define CH32_USBHS_UH_R_DATA_NO     0x40u
#define CH32_USBHS_UH_R_AUTO_TOG    0x20u
#define CH32_USBHS_UH_R_TOG_MASK    0x18u
#define CH32_USBHS_UH_R_TOG_DATA1   0x08u
#define CH32_USBHS_UH_T_DATA_NO     0x40u
#define CH32_USBHS_UH_T_AUTO_TOG    0x20u
#define CH32_USBHS_UH_T_TOG_MASK    0x18u
#define CH32_USBHS_UH_T_TOG_DATA1   0x08u

/*
 * R8_USB_SPEED_TYPE（WCH CH32F/V20x_V30x_V31x 应用手册 22.2.1.6）：
 *   00=全速, 01=高速, 10=低速, 11=保留。
 * 注意与 R8_USB_CTRL.RB_UC_SPEED_TYPE 的区别：后者仅是期望最高速度，
 * 本寄存器为实际协商结果。
 */
#define CH32_USBHS_SPEED_FULL       0x00u
#define CH32_USBHS_SPEED_HIGH       0x01u
#define CH32_USBHS_SPEED_LOW        0x02u

#define CH32_USBHS_UC_HOST_MODE 0x80u
#define CH32_USBHS_UH_TX_BUS_RESET 0x01u
#define CH32_USBHS_UIE_TRANSFER   0x02u
#define CH32_USBHS_UIE_DETECT     0x01u
#define CH32_USBHS_UIF_TRANSFER   0x02u
#define CH32_USBHS_UIF_DETECT     0x01u
#define CH32_USBHS_UMS_DEV_ATTACH 0x02u

#define CH32_USBHS_NVIC_IRQ 85u

static uint32_t ch32_usbhs_ld32(const uint8_t *r, unsigned o)
{
    return ldl_le_p((uint8_t *)r + o);
}

static uint16_t ch32_usbhs_ld16(const uint8_t *r, unsigned o)
{
    return lduw_le_p((uint8_t *)r + o);
}

void ch32_usbhost_meip_resync(Ch32MachineState *m)
{
    RISCVCPU *cpu = &m->cpus.harts[0];
    CPURISCVState *env = &cpu->env;
    const unsigned n = CH32_USBHS_NVIC_IRQ;
    const unsigned wd = n / 32u;
    const unsigned bit = n % 32u;
    bool line_on;
    bool need_mip;

    if (!m->ch32_usb_host_inited) {
        return;
    }

    line_on = (m->usbhs_reg[0] & CH32_USBHS_UC_HOST_MODE) != 0 &&
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

    need_mip = ((uint32_t)(m->usbhs_reg[CH32_USBHS_OFF_INT_FG] &
                           m->usbhs_reg[CH32_USBHS_OFF_INT_EN]) &
                (CH32_USBHS_UIF_TRANSFER | CH32_USBHS_UIF_DETECT)) != 0;
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
        /* USBHS owner 释放：重新扫描其他 pending MEIP 外设 */
        ch32_pfic_resync_pending_meip(m);
    }
    riscv_cpu_interrupt(env);
    qemu_cpu_kick(CPU(cpu));
}

static void ch32_usbhost_raise_transfer(Ch32MachineState *m)
{
    m->usbhs_reg[CH32_USBHS_OFF_INT_FG] |= CH32_USBHS_UIF_TRANSFER;
    ch32_usbhost_meip_resync(m);
}

static void ch32_usbhost_raise_detect(Ch32MachineState *m)
{
    m->usbhs_reg[CH32_USBHS_OFF_INT_FG] |= CH32_USBHS_UIF_DETECT;
    ch32_usbhost_meip_resync(m);
}

static uint8_t ch32_usbhost_int_st_token_field(int token)
{
    switch (token) {
    case USB_TOKEN_SETUP:
        return CH32_USBHS_UIS_TOKEN_SETUP;
    case USB_TOKEN_IN:
        return CH32_USBHS_UIS_TOKEN_IN;
    case USB_TOKEN_OUT:
        return CH32_USBHS_UIS_TOKEN_OUT;
    default:
        return 0;
    }
}

static void ch32_usbhost_apply_packet_result(Ch32MachineState *m, int token,
                                            int status, int actual_in)
{
    uint8_t tok = ch32_usbhost_int_st_token_field(token);
    uint8_t ist = tok;
    /*
     * INT_ST 必须与 WCH USBHS 主机一致，否则 USBHSH_Transact / CherryUSB
     * 用 UIS_H_RES_MASK 判 ACK、用 UIS_TOG_OK 判 IN 成功会永远失败。
     */
    if (status == USB_RET_SUCCESS) {
        if (token == USB_TOKEN_IN) {
            /*
             * IN 成功：将固件写入 RX_CTRL[4:3] 的期望 toggle 值反射到
             * INT_ST[3:0] 的 PID 字段（DATA0 / DATA1）。TOG_OK 恒为 1：
             * QEMU USB 透传场景下，toggle 由 libusb 自行维护，固件 toggle
             * 与总线实际 toggle 脱耦，返回「期望值匹配」符合固件视角。
             */
            uint8_t rx_ctrl =
                m->usbhs_reg[CH32_USBHS_OFF_HOST_RX_CTRL];
            uint8_t rtog = rx_ctrl & CH32_USBHS_UH_R_TOG_MASK;
            uint8_t pid_data = (rtog == CH32_USBHS_UH_R_TOG_DATA1) ?
                                   CH32_USB_PID_DATA1 : CH32_USB_PID_DATA0;
            ist = tok | CH32_USBHS_UIS_TOG_OK | pid_data;
        } else {
            ist = tok | CH32_USB_PID_ACK;
        }
    } else if (status == USB_RET_NAK) {
        ist = tok | CH32_USB_PID_NAK | CH32_USBHS_UIS_IS_NAK;
    } else if (status == USB_RET_STALL) {
        ist = tok | CH32_USB_PID_STALL;
    }
    m->usbhs_reg[CH32_USBHS_OFF_INT_ST] = ist;
    stw_le_p(m->usbhs_reg + CH32_USBHS_OFF_RX_LEN,
             (status == USB_RET_SUCCESS && token == USB_TOKEN_IN) ?
                 (uint16_t)actual_in : 0);
    ch32_usbhost_raise_transfer(m);
}

static bool ch32_usbhs_dma_ok(MachineState *ms, hwaddr gpa, size_t len)
{
    hwaddr ram_sz = memory_region_size(ms->ram);

    if (len > 4096) {
        return false;
    }
    /*
     * len==0：控制传输状态阶段的 OUT（主机发 0 字节）不访问 DMA 缓冲区，
     * 若判失败会导致 STATUS OUT 永远 IOERROR，CherryUSB 在 GET_DESCRIPTOR
     * 后等不到完成而 USB_ERR_TIMEOUT。
     */
    if (len == 0) {
        return true;
    }
    if (gpa < CH32_SRAM_BASE || gpa + len > CH32_SRAM_BASE + ram_sz) {
        return false;
    }
    return true;
}

static int ch32_usbhs_pid4_to_token(unsigned pid4)
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

static void ch32_usbhost_copy_in_to_guest(Ch32MachineState *m, USBPacket *p)
{
    MachineState *ms = MACHINE(OBJECT(m));
    hwaddr rxdma = ch32_usbhs_ld32(m->usbhs_reg, CH32_USBHS_OFF_HOST_RX_DMA);
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    int len = p->actual_length;
    uint8_t tmp[4096];
    size_t n;

    if (len <= 0 || p->pid != USB_TOKEN_IN) {
        return;
    }
    if (!ch32_usbhs_dma_ok(ms, rxdma, (size_t)len)) {
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

/*
 * 将 QEMU USB_SPEED_* 映射到 WCH R8_USB_SPEED_TYPE：
 * usb-host 后端透传真实设备速度（CH341 等 FS 设备不应被硬编码为 HS，
 * 否则 CherryUSB 会按 HS 协商 MPS/间隔，导致 ch341-uart 最终被断开）。
 */
static uint8_t ch32_usbhs_speed_from_dev(const USBDevice *dev)
{
    if (dev == NULL) {
        return CH32_USBHS_SPEED_FULL;
    }
    switch (dev->speed) {
    case USB_SPEED_HIGH: return CH32_USBHS_SPEED_HIGH;
    case USB_SPEED_LOW:  return CH32_USBHS_SPEED_LOW;
    case USB_SPEED_FULL:
    default:             return CH32_USBHS_SPEED_FULL;
    }
}

static void ch32_usb_rh_attach(USBPort *port)
{
    Ch32MachineState *m = port->opaque;

    m->usbhs_reg[9] |= CH32_USBHS_UMS_DEV_ATTACH;
    m->usbhs_reg[CH32_USBHS_OFF_SPEED_TYPE] =
        ch32_usbhs_speed_from_dev(port->dev);
    ch32_usbhost_raise_detect(m);
}

static void ch32_usb_rh_detach(USBPort *port)
{
    Ch32MachineState *m = port->opaque;

    m->usbhs_reg[9] &= (uint8_t)~CH32_USBHS_UMS_DEV_ATTACH;
    m->usbhs_reg[CH32_USBHS_OFF_SPEED_TYPE] = 0;
    ch32_usbhost_raise_detect(m);
}

static void ch32_usb_rh_child_detach(USBPort *port, USBDevice *child)
{
    (void)port;
    (void)child;
}

static void ch32_usb_rh_wakeup(USBPort *port)
{
    (void)port;
}

static void ch32_usb_rh_complete(USBPort *port, USBPacket *p)
{
    Ch32MachineState *m = port->opaque;

    if (p != &m->ch32_usb_pkt) {
        return;
    }
    if (p->pid == USB_TOKEN_IN && p->status == USB_RET_SUCCESS) {
        ch32_usbhost_copy_in_to_guest(m, p);
    }
    ch32_usbhost_apply_packet_result(m, p->pid, p->status, p->actual_length);
    usb_packet_cleanup(p);
}

static USBPortOps ch32_usb_rhport_ops = {
    .attach = ch32_usb_rh_attach,
    .detach = ch32_usb_rh_detach,
    .child_detach = ch32_usb_rh_child_detach,
    .wakeup = ch32_usb_rh_wakeup,
    .complete = ch32_usb_rh_complete,
};

static void ch32_usb_bus_wakeup_ep(USBBus *bus, USBEndpoint *ep,
                                   unsigned int stream)
{
    (void)bus;
    (void)ep;
    (void)stream;
}

static USBBusOps ch32_usb_bus_ops = {
    .wakeup_endpoint = ch32_usb_bus_wakeup_ep,
};

/* USBHS 哑端口 ops：仅占位使 bus->nfree>=2，防止 QEMU 自动创建 usb-hub */
static void ch32_usbhs_dummy_attach(USBPort *port)
{
    (void)port;
}
static void ch32_usbhs_dummy_detach(USBPort *port)       { (void)port; }
static void ch32_usbhs_dummy_child_detach(USBPort *port, USBDevice *c)
                                                         { (void)port; (void)c; }
static void ch32_usbhs_dummy_wakeup(USBPort *port)       { (void)port; }
static void ch32_usbhs_dummy_complete(USBPort *port, USBPacket *p)
                                                         { (void)port; (void)p; }

static USBPortOps ch32_usbhs_dummy_port_ops = {
    .attach       = ch32_usbhs_dummy_attach,
    .detach       = ch32_usbhs_dummy_detach,
    .child_detach = ch32_usbhs_dummy_child_detach,
    .wakeup       = ch32_usbhs_dummy_wakeup,
    .complete     = ch32_usbhs_dummy_complete,
};

void ch32_usbhost_resync_root_mmio(Ch32MachineState *m)
{
    USBDevice *d;

    if (!m->ch32_usb_host_inited) {
        return;
    }
    d = m->ch32_usb_rhport.dev;
    if (d == NULL || !d->attached) {
        return;
    }
    /*
     * ch32_usb_reset_defaults 会 memset 整块 USBHS MMIO，发生在设备已挂到根端口
     * 之后（-device 晚于 init_mr）。若不恢复 MIS_ST，固件读不到
     * UMS_DEV_ATTACH，CherryUSB 永远不会开始枚举。
     * 同时补 UIF_DETECT：固件 UC_CLR_ALL 后写 INT_EN 时通过此函数重触发
     * 设备连接事件，确保不会因 INT_FG 被清而丢失通知。
     */
    m->usbhs_reg[9] |= CH32_USBHS_UMS_DEV_ATTACH;
    m->usbhs_reg[CH32_USBHS_OFF_SPEED_TYPE] = ch32_usbhs_speed_from_dev(d);
    m->usbhs_reg[CH32_USBHS_OFF_INT_FG] |= CH32_USBHS_UIF_DETECT;
    ch32_usbhost_meip_resync(m);
}

void ch32_usbhost_machine_init(Ch32MachineState *m)
{
    if (m->ch32_usb_host_inited) {
        return;
    }
    /*
     * USBBus 的 parent 必须是 DeviceState；MachineState 仅含 Object，
     * 不能传 DEVICE(OBJECT(machine))。挂在已 realize 的 hart array 上即可。
     */
    usb_bus_new(&m->ch32_usb_bus, sizeof(m->ch32_usb_bus), &ch32_usb_bus_ops,
                DEVICE(&m->cpus));
    /* 根端口 */
    usb_register_port(&m->ch32_usb_bus, &m->ch32_usb_rhport, m, 0,
                      &ch32_usb_rhport_ops,
                      USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL |
                          USB_SPEED_MASK_HIGH);
    /*
     * 哑端口：使 bus->nfree>=2，防止 QEMU 在用户 -device usb-host 时
     * 因 nfree==1 而自动创建 usb-hub（该 hub 会被 CherryUSB 作为设备枚
     * 举到但无 Hub Class 驱动，导致下游设备不可见）。
     * 不再硬编码默认 usb-hub/usb-kbd：用户通过 -device 按需挂载。
     */
    usb_register_port(&m->ch32_usb_bus, &m->ch32_usb_dummy_port, m, 1,
                      &ch32_usbhs_dummy_port_ops,
                      USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL |
                          USB_SPEED_MASK_HIGH);
    usb_packet_init(&m->ch32_usb_pkt);
    m->ch32_usb_pkt_inited = true;
    m->ch32_usb_host_inited = true;
}

void ch32_usbhost_machine_finalize(Ch32MachineState *m)
{
    if (m->ch32_usb_pkt_inited) {
        usb_packet_cleanup(&m->ch32_usb_pkt);
        m->ch32_usb_pkt_inited = false;
    }
    m->ch32_usb_host_inited = false;
}

void ch32_usbhost_host_ctrl_write(Ch32MachineState *m, uint8_t v)
{
    uint8_t prev = m->ch32_usbhs_last_host_ctrl;

    m->ch32_usbhs_last_host_ctrl = v;
    if (!m->ch32_usb_host_inited || !m->ch32_usb_rhport.dev) {
        return;
    }
    /*
     * 仅在 UH_BUS_RESET 0→1 边沿触发 device reset：
     * 防止固件以字节粒度写寄存器或 memset(reg, 0, 8) 过程中，BUS_RESET
     * 位被短暂置 1 多次触发 usb_device_reset，潜在引发设备异常。
     */
    if ((v & CH32_USBHS_UH_TX_BUS_RESET) &&
        !(prev & CH32_USBHS_UH_TX_BUS_RESET)) {
        usb_device_reset(m->ch32_usb_rhport.dev);
    }
}

void ch32_usbhost_ep_pid_write(Ch32MachineState *m, uint8_t ep_pid)
{
    MachineState *ms = MACHINE(OBJECT(m));
    USBPacket *p = &m->ch32_usb_pkt;
    uint8_t devadr = m->usbhs_reg[CH32_USBHS_OFF_DEV_AD];
    unsigned pid4 = (unsigned)(ep_pid >> 4) & 0xfu;
    int token = ch32_usbhs_pid4_to_token(pid4);
    int epnum = ep_pid & 0xfu;
    USBDevice *dev;
    USBEndpoint *ep;
    hwaddr txdma = ch32_usbhs_ld32(m->usbhs_reg, CH32_USBHS_OFF_HOST_TX_DMA);
    uint16_t txlen = ch32_usbhs_ld16(m->usbhs_reg, CH32_USBHS_OFF_HOST_TX_LEN);
    uint16_t rxmax = ch32_usbhs_ld16(m->usbhs_reg, CH32_USBHS_OFF_HOST_RX_MAX_LEN);
    uint8_t rx_ctrl = m->usbhs_reg[CH32_USBHS_OFF_HOST_RX_CTRL];
    uint8_t tx_ctrl = m->usbhs_reg[CH32_USBHS_OFF_HOST_TX_CTRL];
    uint8_t buf[2048];
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    MemTxResult mr;

    if (!m->ch32_usb_host_inited) {
        return;
    }
    if (!(m->usbhs_reg[0] & CH32_USBHS_UC_HOST_MODE)) {
        return;
    }
    if (token < 0) {
        ch32_usbhost_apply_packet_result(m, USB_TOKEN_OUT, USB_RET_STALL, 0);
        return;
    }

    BQL_LOCK_GUARD();
    dev = usb_find_device(&m->ch32_usb_rhport, devadr);
    if (dev == NULL || !dev->attached) {
        ch32_usbhost_apply_packet_result(m, token, USB_RET_NODEV, 0);
        return;
    }

    ep = usb_ep_get(dev, token, epnum);

    /*
     * 固件可能在上一个异步事务（usb-host 后端常见）完成前再次写
     * HOST_EP_PID。此时包仍处于 ASYNC/QUEUED 状态，直接调用
     * usb_packet_setup 会触发 assert(!usb_packet_is_inflight(p))。
     * 先取消飞行中的包并清理，再重新初始化。
     *
     * 对非控制端点（BULK / INT），取消后直接返回 NAK：
     * usb-host 后端经 libusb 的 BULK_TIMEOUT=0 永远不会超时，固件
     * 的 NAK 重试循环会无限提交新事务却永远收不到完成中断。
     * 返回 NAK 与真机行为一致（设备无数据时在总线层立即 NAK）。
     */
    if (usb_packet_is_inflight(p)) {
        usb_cancel_packet(p);
        usb_packet_cleanup(p);
        if (epnum != 0) {
            ch32_usbhost_apply_packet_result(m, token, USB_RET_NAK, 0);
            return;
        }
    }
    usb_packet_init(p);
    switch (token) {
    case USB_TOKEN_SETUP:
        if (!ch32_usbhs_dma_ok(ms, txdma, 8)) {
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
        /*
         * TX_CTRL.T_DATA_NO：OUT 只发 TOKEN，不携带数据（PING、
         * 控制传输 STATUS 以及某些握手细节）。强制将 txlen
         * 清零，请求 libusb 发送 ZLP（与真机“只发 TOKEN”语义最接近）。
         */
        if (tx_ctrl & CH32_USBHS_UH_T_DATA_NO) {
            txlen = 0;
        }
        if (txlen > sizeof(buf) || !ch32_usbhs_dma_ok(ms, txdma, txlen)) {
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
        size_t want = rxmax ? rxmax : sizeof(buf);

        /*
         * RX_CTRL.R_DATA_NO：IN 方向不期待数据包主体（PING check 等）。
         * QEMU USB 栈不支持 PING token，此处用 0 长度 IN 模拟：
         * ep 有数据时返回 SUCCESS 不搬运字节，无数据时返回 NAK。
         */
        if (rx_ctrl & CH32_USBHS_UH_R_DATA_NO) {
            want = 0;
        }
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        usb_packet_setup(p, USB_TOKEN_IN, ep, 0, 0, false, false);
        usb_packet_addbuf(p, buf, want);
        usb_handle_packet(dev, p);
        /*
         * 同步完成路径：统一调用 ch32_usbhost_copy_in_to_guest，
         * 与异步完成（ch32_usb_rh_complete）路径保持一致。
         */
        if (p->status == USB_RET_SUCCESS && p->actual_length > 0) {
            ch32_usbhost_copy_in_to_guest(m, p);
        }
        break;
    }
    default:
        g_assert_not_reached();
    }

    if (p->status == USB_RET_ASYNC) {
        return;
    }
    ch32_usbhost_apply_packet_result(m, token, p->status, p->actual_length);
    usb_packet_cleanup(p);
}
