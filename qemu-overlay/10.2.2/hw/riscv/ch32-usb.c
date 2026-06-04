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
 * File:     ch32-usb.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32V30x USB 高速（USBHS，设备 USBHSD / 主机 USBHSH）与 USB 全速 OTG（USBFSD）
 *     以及 USB 全速主机（USBFSH，EVT HOST_KM/HOST_Udisk 等）寄存器 MMIO 模型。
 *
 *     布局与位定义对齐 reference-project/OrayOS-Tiny 的 ch32v30x.h（USBHSD_TypeDef、
 *     USBFSD_TypeDef）及 ch32v30x_usb.h / usb_ch32_usbhs_reg.h；行为参考
 *     reference-project/ch32fun 的 fsusb.c、hsusb_v30x.c（INT_FG 写 1 清除、
 *     MIS_ST 中 SIE_FREE 等）。
 *
 *     USBHS 主机模式：HOST_EP_PID 写提交由 ch32-usb-host.c 桥接至 QEMU USBBus
 *     （根端口默认 usb-hub），可真实枚举下游设备；非主机字段仍为 MMIO 桩。
 *
 *     USBFS 主机模式：同理桥接到 QEMU USBBus，供 EVT USBFS HOST_KM 等示例使用。
 *     IRQ=83（USBFS_IRQn），与 USBHS IRQ=85 相互独立。
 */

#include "ch32-machine-internal.h"
#include "qemu/timer.h"

/* 与 ch32v30x_usb.h / usb_ch32_usbhs_reg.h 一致 */
#define CH32_USBHS_UC_CLR_ALL   0x02u
#define CH32_USBHS_UC_RESET_SIE 0x04u
#define CH32_USBHS_INTFG_W1C    0x7fu

#define CH32_USBHS_MIS_SIE_FREE 0x20u

#define CH32_USBFS_UC_CLR_ALL   0x02u
#define CH32_USBFS_UC_RESET_SIE 0x04u
#define CH32_USBFS_INTFG_W1C    0x1fu

/*
 * USBHS/USBFSH 寄存器偏移已上提至 ch32-machine-internal.h 作为权威定义（表 22-1/23-5），
 * 本文件不再重复声明。USBFS OTG CR/SR 同样上提。
 */

#define CH32_USBFS_UC_HOST_MODE     0x80u  /* BASE_CTRL bit7 */
#define CH32_USBFS_UH_BUS_RESET     0x02u  /* HOST_CTRL bit1 */
#define CH32_USBFS_UIF_TRANSFER     0x02u  /* INT_FG bit1 */
#define CH32_USBFS_UIF_DETECT       0x01u  /* INT_FG bit0 */
#define CH32_USBFS_UMS_DEV_ATTACH   0x01u  /* MIS_ST bit0 */
#define CH32_USBFS_NVIC_IRQ         83u
#define CH32_USBFS_MIS_SIE_FREE     0x20u  /* MIS_ST bit5 = SIE_FREE，始终为 1 */

static uint8_t ch32_usbhs_read_byte(Ch32MachineState *m, hwaddr addr)
{
    if (addr >= sizeof(m->usbhs_reg)) {
        return 0;
    }
    if (addr == 9) {
        return m->usbhs_reg[addr] | CH32_USBHS_MIS_SIE_FREE;
    }
    /*
     * R16_USB_FRAME_NO（addr=4、5）按虚拟时钟微帧动态生成：
     *   USB HS 微帧 = 125µs，低 11 位=SOF帧号（每 8 个微帧进 1），高 3 位=微帧。
     * 固件 usbh_get_frame_number()/同步传输调度对于恒 0 的帧号会失效；此处
     * 按纯虚拟时间满足 “单调递增 · 宽度 16 位回绕” 语义，对 BULK/CTRL 无影响。
     */
    if (addr == CH32_USBHS_OFF_FRAME_NO ||
        addr == CH32_USBHS_OFF_FRAME_NO + 1) {
        uint64_t us = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000ull;
        uint16_t micro = (uint16_t)(us / 125ull);
        uint16_t frame = micro & 0x7ffu;
        uint16_t uframe = (micro >> 11) & 0x7u;
        uint16_t v = (uint16_t)((uframe << 11) | frame);

        stw_le_p(m->usbhs_reg + CH32_USBHS_OFF_FRAME_NO, v);
    }
    /*
     * usb-host 后端的异步完成依赖 QEMU 事件循环处理 libusb FD。
     * 固件在 USBHSH_Transact 中紧密轮询 INT_FG 等待 UIF_TRANSFER，
     * TCG 不会主动退出 cpu_exec，事件循环永远无法运行。
     * 强制 cpu_exit 让 cpu_exec 返回，主循环得以处理 I/O 事件。
     */
    if (addr == 10 && usb_packet_is_inflight(&m->ch32_usb_pkt)) {
        cpu_exit(CPU(&m->cpus.harts[0]));
    }
    return m->usbhs_reg[addr];
}

static uint64_t ch32_usbhs_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;
    uint64_t v = 0;

    for (unsigned i = 0; i < size; i++) {
        v |= (uint64_t)ch32_usbhs_read_byte(m, addr + i) << (8 * i);
    }
    return v;
}

static void ch32_usbhs_write_byte(Ch32MachineState *m, hwaddr addr, uint8_t v)
{
    if (addr >= sizeof(m->usbhs_reg)) {
        return;
    }
    if (addr == 10) {
        m->usbhs_reg[10] &= ~(v & CH32_USBHS_INTFG_W1C);
        ch32_usbhost_meip_resync(m);
        return;
    }
    m->usbhs_reg[addr] = v;
    if (addr == 0) {
        if (v & CH32_USBHS_UC_CLR_ALL) {
            m->usbhs_reg[10] = 0;
            m->usbhs_reg[11] = 0;
        } else if (v & CH32_USBHS_UC_RESET_SIE) {
            m->usbhs_reg[10] &= (uint8_t)~CH32_USBHS_INTFG_W1C;
        }
        ch32_usbhost_meip_resync(m);
    } else if (addr == 2) {
        /*
         * 固件一般先 UC_CLR_ALL（清 INT_FG）再写 INT_EN。此时设备可能已存在，
         * 但 UIF_DETECT 已被清除。resync_root_mmio 重新设置 DEV_ATTACH +
         * UIF_DETECT，确保固件不会错过设备连接事件。
         */
        ch32_usbhost_resync_root_mmio(m);
        ch32_usbhost_meip_resync(m);
    }
}

static void ch32_usbhs_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Ch32MachineState *m = opaque;
    const hwaddr host_ctrl_off = 1;
    const hwaddr host_ep_pid_off = 224;

    for (unsigned i = 0; i < size; i++) {
        ch32_usbhs_write_byte(m, addr + i, (uint8_t)(val >> (8 * i)));
    }
    /*
     * 对 HOST_EP_PID 的 32 位写会逐字节经过 write_byte；此处按「本次 MMIO」
     * 只触发一次事务，避免跨字写重复 usb_handle_packet。
     */
    if (addr <= host_ep_pid_off && addr + size > host_ep_pid_off) {
        uint8_t ep = m->usbhs_reg[host_ep_pid_off];

        if (ep != 0) {
            ch32_usbhost_ep_pid_write(m, ep);
        }
    }
    if (addr <= host_ctrl_off && addr + size > host_ctrl_off) {
        ch32_usbhost_host_ctrl_write(m, m->usbhs_reg[host_ctrl_off]);
    }
}

const MemoryRegionOps ch32_usbhs_ops = {
    .read = ch32_usbhs_read,
    .write = ch32_usbhs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint8_t ch32_usbfs_read_byte(Ch32MachineState *m, hwaddr addr)
{
    if (addr >= sizeof(m->usbfs_reg)) {
        return 0;
    }
    /*
     * MIS_ST (addr=5)：动态注入 SIE_FREE（bit5=0x20），HOST 模式时 bit0=DEV_ATTACH
     * 已由 usbfs_host 回调维护。
     */
    if (addr == CH32_USBFSH_OFF_MIS_ST) {
        return m->usbfs_reg[addr] | CH32_USBFS_MIS_SIE_FREE;
    }
    /* INT_FG 轮询：与 USBHS 相同，usb-host 异步包在飞时强制事件循环 */
    if (addr == CH32_USBFSH_OFF_INT_FG &&
        usb_packet_is_inflight(&m->ch32_usbfs_pkt)) {
        cpu_exit(CPU(&m->cpus.harts[0]));
    }
    return m->usbfs_reg[addr];
}

static uint64_t ch32_usbfs_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;
    uint64_t v = 0;

    if (addr >= CH32_USBFS_SIZE) {
        return 0;
    }
    for (unsigned i = 0; i < size; i++) {
        hwaddr a = addr + i;
        uint8_t b = 0;

        if (a < sizeof(m->usbfs_reg)) {
            b = ch32_usbfs_read_byte(m, a);
        }
        v |= (uint64_t)b << (8 * i);
    }
    return v;
}

static void ch32_usbfs_write_byte(Ch32MachineState *m, hwaddr addr, uint8_t v)
{
    if (addr >= sizeof(m->usbfs_reg)) {
        return;
    }
    /* INT_FG (addr=6)：写 1 清 */
    if (addr == CH32_USBFSH_OFF_INT_FG) {
        m->usbfs_reg[CH32_USBFSH_OFF_INT_FG] &= ~(v & CH32_USBFS_INTFG_W1C);
        ch32_usbfs_host_meip_resync(m);
        return;
    }
    m->usbfs_reg[addr] = v;
    if (addr == CH32_USBFSH_OFF_BASE_CTRL) {
        if (v & CH32_USBFS_UC_CLR_ALL) {
            m->usbfs_reg[CH32_USBFSH_OFF_INT_FG] = 0;
            m->usbfs_reg[CH32_USBFSH_OFF_INT_ST] = 0;
        } else if (v & CH32_USBFS_UC_RESET_SIE) {
            m->usbfs_reg[CH32_USBFSH_OFF_INT_FG] &= (uint8_t)~CH32_USBFS_INTFG_W1C;
        }
        ch32_usbfs_host_meip_resync(m);
    } else if (addr == CH32_USBFSH_OFF_INT_EN) {
        /*
         * FIXME：这里可能是原厂USBIP的BUG。
         *
         * 固件一般清了 INT_FG 再写 INT_EN，此时设备可能已存在。
         * resync_root_mmio 将重新设置 DEV_ATTACH 并触发 UIF_DETECT，
         * 确保固件不会错过设备连接事件。
         */
        ch32_usbfs_host_resync_root_mmio(m);
        ch32_usbfs_host_meip_resync(m);
    }
}

static void ch32_usbfs_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Ch32MachineState *m = opaque;
    const hwaddr host_ep_pid_off = CH32_USBFSH_OFF_HOST_EP_PID;
    const hwaddr host_ctrl_off   = CH32_USBFSH_OFF_HOST_CTRL;

    if (addr >= CH32_USBFS_SIZE) {
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        hwaddr a = addr + i;

        if (a < sizeof(m->usbfs_reg)) {
            ch32_usbfs_write_byte(m, a, (uint8_t)(val >> (8 * i)));
        }
    }
    /*
     * HOST_EP_PID (0x34)：写入触发 USB 事务（仅在 HOST 模式下）。
     * 本次 MMIO 只触发一次，避免多字节写重复提交。
     */
    if (addr <= host_ep_pid_off && addr + size > host_ep_pid_off) {
        uint8_t ep = m->usbfs_reg[host_ep_pid_off];

        if (ep != 0 &&
            (m->usbfs_reg[CH32_USBFSH_OFF_BASE_CTRL] & CH32_USBFS_UC_HOST_MODE)) {
            ch32_usbfs_host_ep_pid_write(m, ep);
        }
    }
    /* HOST_CTRL (0x01)：总线复位等 */
    if (addr <= host_ctrl_off && addr + size > host_ctrl_off) {
        if (m->usbfs_reg[CH32_USBFSH_OFF_BASE_CTRL] & CH32_USBFS_UC_HOST_MODE) {
            ch32_usbfs_host_ctrl_write(m, m->usbfs_reg[host_ctrl_off]);
        }
    }
}

const MemoryRegionOps ch32_usbfs_ops = {
    .read = ch32_usbfs_read,
    .write = ch32_usbfs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

void ch32_usb_reset_defaults(Ch32MachineState *m)
{
    /*
     * 在 memset 寄存器镜像之前，先取消飞行中的 async USBPacket
     * 并复位已连接设备的 endpoint toggle。否则固件 `reset`
     * 软重启时：
     *   1) libusb（usb-host 后端）或标准 QEMU USB 设备还有在飞行
     *      的事务，其 complete 回调给刚被 memset 的 INT_FG/INT_ST
     *      加上新的 IF，通过 MEIP 打到刚启动的新固件成为幽灵中断；
     *   2) 设备端 EP.toggle 没随复位清零，新固件以 DATA0 起点但
     *      设备等待 DATA1，查询描述符的 DATA 阶段陷入不匹配。
     * 合起来表现为“每次 reset 后 USB 有概率不识别”。
     *
     * 顺序：cancel 在 memset 前——即使 cancel 回调装设了新 IF，
     * 紧跟的 memset 会洗破干净。usb_device_reset 同步在前。
     */
    if (m->ch32_usb_host_inited) {
        USBPacket *p = &m->ch32_usb_pkt;
        if (m->ch32_usb_pkt_inited && usb_packet_is_inflight(p)) {
            usb_cancel_packet(p);
            usb_packet_cleanup(p);
            usb_packet_init(p);
        }
        if (m->ch32_usb_rhport.dev && m->ch32_usb_rhport.dev->attached) {
            usb_device_reset(m->ch32_usb_rhport.dev);
        }
    }
    if (m->ch32_usbfs_host_inited) {
        USBPacket *p = &m->ch32_usbfs_pkt;
        if (m->ch32_usbfs_pkt_inited && usb_packet_is_inflight(p)) {
            usb_cancel_packet(p);
            usb_packet_cleanup(p);
            usb_packet_init(p);
        }
        if (m->ch32_usbfs_rhport.dev && m->ch32_usbfs_rhport.dev->attached) {
            usb_device_reset(m->ch32_usbfs_rhport.dev);
        }
    }

    memset(m->usbhs_reg, 0, sizeof(m->usbhs_reg));
    memset(m->usbfs_reg, 0, sizeof(m->usbfs_reg));
    /*
     * MIS_SIE_FREE 在 ch32_usbhs_read_byte/ch32_usbfs_read_byte 中动态 OR 返回，
     * 无需预先写入寄存器（保留会导致语义混淆）。
     */
    /*
     * USBFS OTG_SR 默认值已由 memset 清零，表示无 VBUS / 未进入 Session，
     * ID_DIG=0 默认 A 端。当前项目无 OTG 角色切换需求，保持纯存储即可。
     */
    m->ch32_usbhs_last_host_ctrl = 0;
    m->ch32_usbfs_last_host_ctrl = 0;
    ch32_usbhost_resync_root_mmio(m);
    ch32_usbfs_host_resync_root_mmio(m);
}

void ch32_usb_init_mr(Ch32MachineState *m, Object *owner)
{
    memory_region_init_io(&m->usbhs_iomem, owner, &ch32_usbhs_ops, m,
                          "ch32-usbhs", CH32_USBHS_IO_SIZE);
    memory_region_init_io(&m->usbfs_iomem, owner, &ch32_usbfs_ops, m,
                          "ch32-usbfs", CH32_USBFS_SIZE);
    ch32_usb_reset_defaults(m);
}
