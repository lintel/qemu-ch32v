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
 * File:     ch32-machine-internal.h
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 整机内部状态与外设 MMIO 声明（供 ch32-v.c 与拆分模块包含）。
 */

#ifndef HW_RISCV_CH32_MACHINE_INTERNAL_H
#define HW_RISCV_CH32_MACHINE_INTERNAL_H

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/sysbus.h"
#include "hw/loader.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "system/tcg.h"
#include "chardev/char-fe.h"
#include "chardev/char.h"
#include "migration/vmstate.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/irq.h"
#include "net/net.h"
#include "hw/usb.h"
#include "qemu/main-loop.h"
#include "qemu/log.h"
#include "target/riscv/cpu-qom.h"
#include "target/riscv/cpu.h"
#include "target/riscv/cpu_bits.h"
#include "qemu/thread.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "elf.h"
#include "qom/object.h"
#include <inttypes.h>
#include <string.h>

#define CH32_MACHINE(obj) OBJECT_CHECK(Ch32MachineState, (obj), TYPE_MACHINE)

#define CH32_FLASH_BASE   0x08000000ULL
#define CH32_SRAM_BASE    0x20000000ULL
#define CH32_USART1_BASE  0x40013800ULL
#define CH32_PERIPH_BUS_BASE   0x40000000ULL
/*
 * 默认 2MiB 外设 RAM 占位；OrayOS-Tiny 网络栈会扫过 2MiB 边界，扩至 12MiB 可避免常见
 * Load fault（代价为宿主机多 ~10MiB RAM）。高优先级 MMIO 仍覆盖真外设。
 */
#define CH32_PERIPH_BUS_SIZE   0x00c00000ULL
#define CH32_ADC1_BASE   0x40012400ULL
#define CH32_ADC2_BASE   0x40012800ULL
#define CH32_ADC3_BASE   0x40013c00ULL
#define CH32_ADC_EVT_SIZE 0x80u
#define CH32_ADC_FLAG_EOC 0x02u
#define CH32_RCC_BASE     0x40021000ULL
#define CH32_RCC_SIZE     0x80
#define CH32_PFIC_BASE    0xE000E000ULL
#define CH32_PFIC_SIZE    0x1000
/* PFIC CFGR 寄存器偏移（相对 CH32_PFIC_BASE）*/
#define CH32_PFIC_CFGR_OFF    0x48u
#define CH32_PFIC_GISR_OFF    0x4Cu
#define CH32_PFIC_VTFIDR_OFF  0x50u
#define CH32_PFIC_VTFADDR_OFF 0x60u
#define CH32_PFIC_VTF_NUM     4u
#define CH32_PFIC_SCTLR_OFF   0xD10u
#define CH32_PFIC_IENR0_OFF 0x100u

#define CH32_PFIC_IRER0_OFF 0x180u
#define CH32_PFIC_IPSR0_OFF 0x200u
#define CH32_PFIC_IPRR0_OFF 0x280u
#define CH32_SYSTICK_NVIC_BIT 12u
#define CH32_STK_BASE     0xE000F000ULL
#define CH32_STK_SIZE     0x40
#define CH32_ESIG_BASE    0x1ffff700ULL
#define CH32_ESIG_SIZE    0x900
/*
 * DBGMCU 调试控制寄存器（CFGR0 + CFGR1，共 8 字节）。
 * 地址来源：核层私有外设区（0xE000_0000 起），与 CH32V10x hw.h \u53ca
 * ARM Cortex-M 调试组件表一致。
 * 注：0x40001000 是 TIM6 的地址，前如将 DBGMCU 注册在此属于地址错误。
 */
#define CH32_DBGMCU_BASE  0xE000D000ULL
#define CH32_DBGMCU_SIZE  0x10
#define CH32_AFIO_EXTI_BASE 0x40010000ULL
#define CH32_AFIO_EXTI_SIZE 0x800
/*
 * GPIO block 必须以 4KiB 页对齐注册，否则 QEMU TCG TLB multipage section
 * 的 offset_within_region 低12位非零，导致 iotlb_to_section 断言失败。
 * 真实 GPIO 从 0x40010800 开始，这里向下对齐到 0x40010000，handler 中
 * 对 addr < 0x800 的访问直接忽略（AFIO 区域，由高优先级 AFIO 处理器覆盖）。
 */
#define CH32_GPIO_BLOCK_BASE 0x40010000ULL
#define CH32_GPIO_BLOCK_SIZE 0x3000
#define CH32_GPIO_BLOCK_OFFSET 0x800
#define CH32_DMA1_BLOCK_BASE 0x40020000ULL
#define CH32_DMA1_BLOCK_SIZE 0x1000

/* DMA 通道数 */
#define CH32_DMA1_NUM_CH  7
#define CH32_DMA2_NUM_CH  11  /* Ch1~7 in DMA2, Ch8~11 in DMA2_EXTEN */

typedef struct Ch32DmaChannel {
    uint32_t cfgr;
    uint32_t cntr;
    uint32_t paddr;
    uint32_t maddr;
} Ch32DmaChannel;

typedef struct Ch32DmaState {
    MemoryRegion iomem;
    uint32_t intfr[3];   /* [0]=DMA1, [1]=DMA2, [2]=DMA2_EXTEN */
    Ch32DmaChannel ch1[CH32_DMA1_NUM_CH];
    Ch32DmaChannel ch2[CH32_DMA2_NUM_CH];
} Ch32DmaState;

#define CH32_AHB_MISC_BASE 0x40023000ULL
#define CH32_AHB_MISC_SIZE 0x1000
/* CRC 控制器（基址偏移 = 0x0000，size=0x10 足够容纳 3 个寄存器） */
#define CH32_CRC_BASE     0x40023000ULL
#define CH32_CRC_SIZE     0x10
#define CH32_RNG_BASE     0x40023c00ULL
#define CH32_RNG_SIZE     0x10
#define CH32_ETH_MMIO_BASE  0x40028000ULL
#define CH32_ETH_MMIO_SIZE  0x1800
#define CH32_EXTEN_CTR_OFF     (0x40023800ull - CH32_AHB_MISC_BASE)
#define CH32_EXTEN_CTR2_OFF    (0x40023808ull - CH32_AHB_MISC_BASE)
#define CH32_EXTEN_CTR_RESET   0x00000A40u
#define CH32_EXTEN_CTR2_RESET  0u
#define CH32_USBHS_BASE   0x40023400ULL
#define CH32_USBHS_IO_SIZE 0x400u
#define CH32_USBHS_REGBUF_SIZE 0x400u
#define CH32_USBFS_BASE   0x50000000ULL
#define CH32_USBFS_SIZE   0x8000
#define CH32_USBFS_REGBUF_SIZE 0x200u

/*
 * USBHS 寄存器字节偏移（手册表 22-1/22-3，与
 * reference-project/OrayOS-Tiny ch32v30x.h USBHSH_TypeDef packed 布局对齐）。
 * 本头文件为 overlay 内所有 USBHS 代码的权威定义源，
 * 其它文件不得重复声明。
 */
#define CH32_USBHS_OFF_USB_CTRL          0x00u  /* R8_USB_CTRL */
#define CH32_USBHS_OFF_INT_EN            0x02u  /* R8_USB_INT_EN */
#define CH32_USBHS_OFF_DEV_AD            0x03u  /* R8_USB_DEV_AD */
#define CH32_USBHS_OFF_FRAME_NO          0x04u  /* R16_USB_FRAME_NO */
#define CH32_USBHS_OFF_SPEED_TYPE        0x08u  /* R8_USB_SPEED_TYPE */
#define CH32_USBHS_OFF_MIS_ST            0x09u  /* R8_USB_MIS_ST */
#define CH32_USBHS_OFF_INT_FG            0x0Au  /* R8_USB_INT_FG */
#define CH32_USBHS_OFF_INT_ST            0x0Bu  /* R8_USB_INT_ST */
#define CH32_USBHS_OFF_RX_LEN            0x0Cu  /* R16_USB_RX_LEN */
#define CH32_USBHS_OFF_HOST_RX_DMA       0x24u  /* R32_UH_RX_DMA */
#define CH32_USBHS_OFF_HOST_TX_DMA       0x64u  /* R32_UH_TX_DMA */
#define CH32_USBHS_OFF_HOST_RX_MAX_LEN   0xA0u  /* R16_UH_RX_MAX_LEN */
#define CH32_USBHS_OFF_HOST_EP_PID       0xE0u  /* R8_UH_EP_PID */
#define CH32_USBHS_OFF_HOST_RX_CTRL      0xE3u  /* R8_UH_RX_CTRL */
#define CH32_USBHS_OFF_HOST_TX_LEN       0xE4u  /* R16_UH_TX_LEN */
#define CH32_USBHS_OFF_HOST_TX_CTRL      0xE6u  /* R8_UH_TX_CTRL */

/*
 * USBFSH 寄存器字节偏移（手册表 23-1/23-5，packed USBFSH_TypeDef）。
 * 适用于 CH32V30x USBFS Host 模式。
 */
#define CH32_USBFSH_OFF_BASE_CTRL        0x00u
#define CH32_USBFSH_OFF_HOST_CTRL        0x01u
#define CH32_USBFSH_OFF_INT_EN           0x02u
#define CH32_USBFSH_OFF_DEV_ADDR         0x03u
#define CH32_USBFSH_OFF_MIS_ST           0x05u
#define CH32_USBFSH_OFF_INT_FG           0x06u
#define CH32_USBFSH_OFF_INT_ST           0x07u
#define CH32_USBFSH_OFF_RX_LEN           0x08u  /* u16 */
#define CH32_USBFSH_OFF_HOST_RX_DMA      0x18u  /* u32 */
#define CH32_USBFSH_OFF_HOST_TX_DMA      0x1Cu  /* u32 */
#define CH32_USBFSH_OFF_HOST_SETUP       0x32u  /* u16 */
#define CH32_USBFSH_OFF_HOST_EP_PID      0x34u
#define CH32_USBFSH_OFF_HOST_RX_CTRL     0x37u
#define CH32_USBFSH_OFF_HOST_TX_LEN      0x38u  /* u16 */
#define CH32_USBFSH_OFF_HOST_TX_CTRL     0x3Au

/* USBFS OTG 寄存器（手册表 23-1 §23.2.4） */
#define CH32_USBFS_OFF_OTG_CR            0x54u
#define CH32_USBFS_OFF_OTG_SR            0x58u
#define CH32_FLASHREG_BASE 0x40022000ULL
#define CH32_FLASHREG_SIZE 0x400
#define CH32_RTC_BASE     0x40002800ULL
#define CH32_RTC_SIZE     0x80
/* RTC 寄存器偏移（单位字节，均为 16-bit 宽）*/
#define CH32_RTC_CTLRH_OFF  0x00u  /* 中断使能 */
#define CH32_RTC_CTLRL_OFF  0x04u  /* 状态/控制 */
#define CH32_RTC_PSCRH_OFF  0x08u  /* 预分频重装高 */
#define CH32_RTC_PSCRL_OFF  0x0Cu  /* 预分频重装低 */
#define CH32_RTC_DIVH_OFF   0x10u  /* 分频余值高（只读） */
#define CH32_RTC_DIVL_OFF   0x14u  /* 分频余值低（只读） */
#define CH32_RTC_CNTH_OFF   0x18u  /* 计数器高 */
#define CH32_RTC_CNTL_OFF   0x1Cu  /* 计数器低（注：NOT 0x1A） */
#define CH32_RTC_ALRMH_OFF  0x20u  /* 闹钟高 */
#define CH32_RTC_ALRML_OFF  0x24u  /* 闹钟低 */
/* CTLRL 标志位 */
#define CH32_RTC_FLAG_RTOFF 0x0020u  /* bit5：上次写入已完成 */
#define CH32_RTC_FLAG_RSF   0x0008u  /* bit3：同步完成 */
#define CH32_RTC_FLAG_SECF  0x0001u  /* bit0：秒事件 */
#define CH32_RTC_FLAG_ALRF  0x0002u  /* bit1：闹钟事件 */
/* CTLRH 使能位 */
#define CH32_RTC_SECIE      0x0001u  /* CTLRH.SECIE：秒中断使能 */
#define CH32_RTC_ALRIE      0x0002u  /* CTLRH.ALRIE：闹钟中断使能 */
#define CH32_QEMU_CLINT_BASE 0x02000000ULL
#define CH32_WWDG_BASE    0x40002C00ULL
#define CH32_WWDG_SIZE    0x10
#define CH32_IWDG_BASE    0x40003000ULL
#define CH32_IWDG_SIZE    0x10
#define CH32_USART2_BASE  0x40004400ULL
#define CH32_USART3_BASE  0x40004800ULL
#define CH32_UART4_BASE   0x40004C00ULL
#define CH32_UART5_BASE   0x40005000ULL
#define CH32_UART6_BASE   0x40001800ULL
#define CH32_UART7_BASE   0x40001C00ULL
#define CH32_UART8_BASE   0x40002000ULL
#define CH32_USART_STUB_SIZE 0x400
#define CH32_BKP_BASE     0x40006C00ULL
#define CH32_BKP_SIZE     0x100
#define CH32_PWR_BASE     0x40007000ULL
#define CH32_PWR_SIZE     0x10
#define CH32_SDIO_BASE    0x40018000ULL
#define CH32_SDIO_SIZE    0x400

/* USART PFIC IRQ 号（CH32V307/V317 手册 Table 6-2）*/
#define CH32_USART1_NVIC_IRQ  53u
#define CH32_USART2_NVIC_IRQ  54u
#define CH32_USART3_NVIC_IRQ  55u
/* UART4~8（手册表 9-2 / 18 章基址表，CH32V307） */
#define CH32_UART4_NVIC_IRQ   68u
#define CH32_UART5_NVIC_IRQ   69u
#define CH32_UART6_NVIC_IRQ   87u
#define CH32_UART7_NVIC_IRQ   88u
#define CH32_UART8_NVIC_IRQ   89u
/* CTLR1.RXNEIE 位（bit5）：接收数据非空中断使能 */
#define CH32_USART1_RXNEIE_BIT 5u
/* WWDG PFIC IRQ 号 */
#define CH32_WWDG_NVIC_IRQ    16u
/* SDIO PFIC IRQ 号 */
#define CH32_SDIO_NVIC_IRQ    65u
/* RTC PFIC IRQ 号 */
#define CH32_RTC_NVIC_IRQ     19u

#define TYPE_CH32_ETH_DWMAC   "ch32-eth-dwmac"
/* ETH_IRQn = 77（CH32V30x 手册 Table 6-2）: word=2, bit=13 */
#define CH32_ETH_IRQ_N  77u

#define TYPE_CH32_ETH_10M  "ch32-eth-10m"
/* ETH10M MMIO 大小（0x40028000 ~ 0x4002802F）*/
#define CH32_ETH_10M_MMIO_SIZE  0x30ULL
/* ETH_IRQn = 61（CH32V20x_D8 手册）: word=1, bit=29 */
#define CH32_ETH_10M_IRQ_N  61u

/* TIM2~TIM7 实例数量 */
#define CH32_TIM_COUNT  6u

/*
 * Ch32TimState - TIM2~TIM7 单实例状态（嵌入 Ch32MachineState）。
 * 此处定义使 ch32-machine-internal.h 的消费者无需包含 ch32-tim.c 内部头。
 */
typedef struct Ch32TimState {
    MemoryRegion iomem;
    QEMUTimer   *timer;

    /* Shadow registers (16-bit values stored as 32-bit for simplicity) */
    uint16_t ctlr1;
    uint16_t ctlr2;
    uint16_t smcfgr;
    uint16_t dmaintenr;
    uint16_t intfr;
    uint16_t chctlr1;     /* GPTM只有，BCTM(TIM6/7)无捕获比较 */
    uint16_t chctlr2;
    uint16_t ccer;
    uint16_t psc;
    uint16_t atrlr;          /* auto-reload register (shadow when ARPE=1) */
    uint16_t atrlr_preload;  /* preload buffer */
    /* rptcr/bdtr: 仅 ADTM(TIM1/TIM8)有，GPTM/BCTM 无效（读返回0，写忽略） */
    uint16_t rptcr;
    uint16_t ch1cvr, ch2cvr, ch3cvr, ch4cvr;  /* GPTM 只有 */
    uint16_t bdtr;            /* 仅 ADTM 有 */
    uint16_t dmacfgr;
    uint16_t dmaadr;
    uint16_t aux;             /* GPTM 只有：offset 0x50，WCH 双边沿捕获扩展 */

    /* CNT: updated lazily based on virtual clock */
    uint64_t cnt_base;   /* CNT value at epoch */
    uint64_t epoch_ns;   /* QEMU virtual clock ns at epoch */

    /*
     * IRQ state: whether we have injected MEIP for this timer.
     * Cleared when firmware writes INTFR.UIF=0.
     */
    bool irq_active;

    /* Pointer back to machine (set during init) */
    struct Ch32MachineState *machine;
    unsigned idx;  /* 0=TIM2, 1=TIM3, ... */
} Ch32TimState;

/*
 * CH32 启动模式（手册表 1-1）：
 *   CH32_BOOT_FLASH   - BOOT0=0，BOOT1=X：程序闪存存储器启动（默认）
 *                       0x00000000 重映射到 Flash（即 0x08000000 内容）
 *   CH32_BOOT_SYSMEM  - BOOT0=1，BOOT1=0：系统存储器启动
 *                       0x00000000 重映射到系统存储器（0x1FFF8000 处）
 *   CH32_BOOT_SRAM    - BOOT0=1，BOOT1=1：内部 SRAM 启动
 *                       0x00000000 仅能以 0x20000000 地址访问（即只有 SRAM 可访）
 */
typedef enum Ch32BootMode {
    CH32_BOOT_FLASH  = 0,
    CH32_BOOT_SYSMEM = 1,
    CH32_BOOT_SRAM   = 2,
} Ch32BootMode;

/* 系统存储器（Bootloader ROM）地址范围（0x1FFF8000） */
#define CH32_SYSMEM_BASE  0x1FFF8000ULL
/* 系统存储器大小：28KiB（手册：0x1FFF8000~0x1FFFF000 = 0x7000 = 28KiB） */
#define CH32_SYSMEM_SIZE  0x7000ULL
/* 启动重映射地址：0x00000000（三种模式的共同目标） */
#define CH32_BOOT_REMAP_BASE 0x00000000ULL
/* 重映射大小：与源区域大小相同，在初始化时动态确定 */


typedef struct Ch32RccShadow {
    uint32_t words[CH32_RCC_SIZE / sizeof(uint32_t)];
} Ch32RccShadow;

typedef struct Ch32StkState {
    uint32_t ctlr;
    uint32_t sr;
    uint32_t cmpl;
    uint32_t cmph;
} Ch32StkState;

typedef struct Ch32RtcShadow {
    uint16_t hwords[CH32_RTC_SIZE / 2];
} Ch32RtcShadow;

/* BKP 备份寄存器数量（CH32V307: DATAR1~42 = 42个）*/
#define CH32_BKP_NREGS 42

#define CH32_GPIO_NPORTS 8

typedef struct Ch32GpioPort {
    uint32_t cfglr;
    uint32_t cfghr;
    uint32_t indr_in;
    uint32_t outdr;
    uint32_t lckr;
} Ch32GpioPort;

/*
 * Ch32BoardDesc - 整机型号静态描述表。
 * 定义在 ch32-boards.c（每个型号一个 const 实例），被 ch32-v.c 的
 * CH32_MACHINE_ONE 宏与 ESIG 填充函数（ch32-boot.c）引用。
 */
typedef struct Ch32BoardDesc {
    const char *tag;
    const char *pretty;
    uint64_t flash_size;
    ram_addr_t default_ram_size;
    const char *default_cpu_type;
    const char *const *valid_cpu_types;
    const char *ram_id;
    const char *default_nic;
    /*
     * Per-板时钟配置（Hz）——仅作为 RCC 章程合成输入，
     * QEMU 不建模真实的模拟时钟树。
     *   hsi_hz - 内置高速 RC 振荡器频率
     *   hse_hz - 外部高速晶振频率（评估板典型值）
     * 为 0 时 RCC 将回退到编译期缺省值 8 MHz，保持向后兼容。
     */
    uint32_t hsi_hz;
    uint32_t hse_hz;
} Ch32BoardDesc;

extern const Ch32BoardDesc ch32_board_ch32v317;
extern const Ch32BoardDesc ch32_board_ch32v307;
extern const Ch32BoardDesc ch32_board_ch32v305;
extern const Ch32BoardDesc ch32_board_ch32v303;
extern const Ch32BoardDesc ch32_board_ch32v203;
extern const Ch32BoardDesc ch32_board_ch32v203rb;
extern const Ch32BoardDesc ch32_board_ch32v103;
extern const Ch32BoardDesc ch32_board_ch32v003;
extern const Ch32BoardDesc ch32_board_ch32v407;
extern const Ch32BoardDesc ch32_board_ch32h417;

/*
 * USART1 内嵌状态（不作为独立 SysBus 设备，直接嵌入 Ch32MachineState，
 * 使 ch32-usart.c 可直接访问 pfic_ienr 和 CPU env 以驱动 MEIP 中断）。
 */

/* RX/TX FIFO 深度：64 字节。
 * 真实硬件只有 1 字节 DATAR，但 QEMU 主机速度远快于固件中断响应，
 * 需要较大缓冲去消化这种速度差异。 */
#define CH32_USART1_RXFIFO_DEPTH 64
#define CH32_USART1_TXFIFO_DEPTH 64

/*
 * RX 字节投喂间隔（纳秒）：模拟 115200 bps 的字节间隔。
 * 115200 bps, 10 bits/byte (1 start + 8 data + 1 stop) → ~86.8 μs/byte
 * 用定时器逐字节投喂，确保每字节都能单独触发 IRQ，与真实串口行为一致。
 * RT-Thread finsh 依赖每字节一次 IRQ + 一次 rx_indicate，若一次 IRQ 消费
 * 多字节则信号量计数不足，导致 finsh_getchar 阻塞、msh_exec 永不被调用。
 */
#define CH32_USART1_RX_BYTE_NS 86806LL  /* 1e9 / 115200 * 10 ≈ 86806 ns */

typedef struct Ch32Usart1State {
    MemoryRegion iomem;
    CharFrontend chr;

    /* 寄存器字段（对应手册表 18-2 ~ 18-9） */
    uint16_t statr;   /* +0x00 STATR：状态寄存器（软件可写 0 清 TC/RXNE/CTS/LBD） */
    uint32_t brr;     /* +0x08 BRR */
    uint32_t ctlr1;   /* +0x0C CTLR1 */
    uint32_t ctlr2;   /* +0x10 CTLR2 */
    uint32_t ctlr3;   /* +0x14 CTLR3 */
    uint32_t gpr;     /* +0x18 GPR */
    uint32_t ctlr4;   /* +0x1C CTRL4 */

    /* RX FIFO：环形缓冲 */
    uint8_t  rx_buf[CH32_USART1_RXFIFO_DEPTH];
    unsigned rx_head; /* 下一个读取位置 */
    unsigned rx_tail; /* 下一个写入位置 */

    /* TX FIFO：环形缓冲 */
    uint8_t  tx_buf[CH32_USART1_TXFIFO_DEPTH];
    unsigned tx_head; /* 下一个读取位置（待发送） */
    unsigned tx_tail; /* 下一个写入位置 */

    /* TX drain 定时器：异步将 TX FIFO 数据冲刷到 char backend */
    QEMUTimer *tx_timer;

    /*
     * RX 节拍定时器：逐字节投喂 rx_stage_buf，模拟串口字节间隔。
     * 防止 TCP/PTY backend 一次推入多字节导致 RT-Thread finsh 信号量计数不足。
     */
    QEMUTimer *rx_timer;
    uint8_t  rx_stage_buf[CH32_USART1_RXFIFO_DEPTH]; /* 暂存待投喂字节 */
    unsigned rx_stage_head; /* 下一个待投喂位置 */
    unsigned rx_stage_tail; /* 下一个写入位置 */

    /* 内部状态 */
    bool     rx_pending;  /* FIFO 非空（= rx_head != rx_tail） */
    uint8_t  rdr;         /* DATAR 读取快取字段（FIFO 头字节） */

    bool     tx_empty;    /* TX FIFO 为空（TXE=1） */
    bool     tx_complete; /* TC：所有字节已发送完成 */
} Ch32Usart1State;

typedef struct Ch32EthState Ch32EthState;

/*
 * Ch32Usart23State - USART2/3 与 UART4~8 共用轻量级仿真状态（无 char backend）。
 *
 * 支持寄存器读写、CTLR3 DMAT/DMAR 语义，以及 DMA TX/RX 握手：
 *   - TX DMA：固件写 DATAR，若 DMAT=1 且 DMA 通道 EN=1 则等待 DMA 自驱（通用引擎已处理）
 *   - RX DMA：固件写 DATAR 作模拟源，DMAR=1 时由 pump 搬运（不占 RXNE）
 *   - 中断：PFIC/MEIP 仅同步 **RXNE+RXNEIE**（8 字节 FIFO + `ch32_usart_lite_push_rx_byte` 注入）；
 *     不派发 TXE/TCIE，避免轻量模型下 TXE|TC 恒真导致 MEIP 风暴
 *   - 无 char backend：TX 字节直接丢弃；STATR 动态位含 RXNE（FIFO 非空）
 */
#define CH32_USART_LITE_RXFIFO_DEPTH 8u

typedef struct Ch32Usart23State {
    MemoryRegion iomem;
    uint16_t statr;    /* 影子（轻量模型主要用动态 STATR；复位 0x00C0） */
    uint32_t brr;
    uint32_t ctlr1;    /* CTLR1：UE/TE/RE/RXNEIE/TXEIE/TCIE */
    uint32_t ctlr2;
    uint32_t ctlr3;    /* CTLR3：DMAT(bit7)/DMAR(bit6) */
    uint32_t gpr;
    uint32_t ctlr4;
    uint8_t  datar_last; /* RDR 影子 / RX DMA 模拟源字节 */
    bool     rx_pending; /* DMAR=1 时待 DMA 搬运（不走 RX FIFO） */
    uint8_t  rx_fifo[CH32_USART_LITE_RXFIFO_DEPTH];
    uint8_t  rx_fifo_h;
    uint8_t  rx_fifo_t;
} Ch32Usart23State;

/* SPI/I2C stub 实例数量 */
#define CH32_SPI_NUM  3u
#define CH32_I2C_NUM  2u

/* SPI 实例状态（回环模式） */
typedef struct Ch32SpiState {
    uint16_t ctlr1;
    uint16_t ctlr2;
    uint16_t statr;
    uint16_t datar;
    uint16_t crcr;
    uint16_t rcrcr;
    uint16_t tcrcr;
    uint16_t i2scfgr;
    uint16_t i2spr;
    uint16_t hscr;
    uint16_t rx_shadow; /* 回环接收缓冲 */
    bool     rx_ne;     /* RXNE 影子标志 */
} Ch32SpiState;

/* I2C 实例状态（Master 状态机） */
typedef struct Ch32I2cState {
    uint16_t ctlr1;
    uint16_t ctlr2;
    uint16_t oaddr1;
    uint16_t oaddr2;
    uint16_t datar;
    uint16_t star1;
    uint16_t star2;
    uint16_t ckcfgr;
    uint16_t rtr;
    bool start_pending;
    bool addr_pending;
    bool stop_pending;
} Ch32I2cState;

typedef struct Ch32MachineState {
    MachineState parent;
    RISCVHartArrayState cpus;
    MemoryRegion flash;
    MemoryRegion boot_remap; /* 0x00000000 启动重映射：根据 boot_mode 映射到 Flash/SYSMEM */
    MemoryRegion sysmem_rom; /* 系统存储器（Bootloader ROM）@ 0x1FFF8000 */
    MemoryRegion sram_gap;
    Ch32RccShadow rcc_shadow;
    MemoryRegion rcc_iomem;
    Ch32RtcShadow rtc_shadow;
    MemoryRegion rtc_iomem;
    MemoryRegion rng_iomem;
    uint32_t rng_seq;
    MemoryRegion pfic_io;          /* 统一 PFIC IO handler（0x1000 字节） */
    uint32_t pfic_ienr[8];          /* 中断使能寄存器（ISR 读此值） */
    uint32_t pfic_ipr[8];           /* 中断挂起状态（IPR/IPSR/IPRR 共用） */
    uint32_t pfic_iactr[8];         /* 中断激活状态（IACTR，进入 IRQ 时置位，mret 清除） */
    uint8_t  pfic_iprior[256];      /* 中断优先级（IPRIOR，字节粒度） */
    uint32_t pfic_ithresdr;         /* 中断优先级阈值配置寄存器 */
    uint32_t pfic_sctlr;            /* 系统控制寄存器（SCTLR @0xD10） */
    uint8_t  pfic_vtfidr[4];         /* VTF 通道 IRQ ID（字节粒度，对应 VTFIDR[0..3]）*/
    uint32_t pfic_vtfaddr[CH32_PFIC_VTF_NUM]; /* VTF 中断地址寄存器（4路） */
    Ch32StkState stk;
    MemoryRegion stk_iomem;
    QEMUTimer *stk_timer;
    uint64_t stk_epoch_ns;
    uint64_t stk_cnt_at_epoch;
    MemoryRegion esig_rom;
    MemoryRegion dbg_ram;
    MemoryRegion gpio_iomem;
    MemoryRegion afio_exti_iomem;
    Ch32GpioPort gpio_ports[CH32_GPIO_NPORTS];
    uint32_t afio_ecr;
    uint32_t afio_pcfr1;
    uint32_t afio_pcfr2;
    uint32_t afio_exticr[4];
    uint32_t exti_shadow[(CH32_AFIO_EXTI_SIZE - 0x400) / 4];
    Ch32DmaState dma;
    MemoryRegion ahb_misc_ram;
    MemoryRegion usbhs_iomem;
    MemoryRegion usbfs_iomem;
    uint8_t usbhs_reg[CH32_USBHS_REGBUF_SIZE];
    uint8_t usbfs_reg[CH32_USBFS_REGBUF_SIZE];
    /* USBFS 主机（USB Full-Speed Host）状态 */
    USBBus  ch32_usbfs_bus;
    USBPort ch32_usbfs_rhport;
    USBPort ch32_usbfs_dummy_port; /* 哑端口：使 bus->nfree>=2，避免 usb_claim_port 自动创建 hub */
    USBPacket ch32_usbfs_pkt;
    bool ch32_usbfs_pkt_inited;
    bool ch32_usbfs_host_inited;
    Ch32Usart1State usart1;
    MemoryRegion flash_reg_iomem;
    uint32_t flash_reg_shadow[256];
    bool flash_unlocked;
    uint8_t flash_key_step;
    uint8_t flash_ob_key_step;
    uint32_t flash_statr;
    char *flash_image;
    uint64_t flash_size_cfg;
    /* 启动模式（手册表 1-1，默认 CH32_BOOT_FLASH） */
    Ch32BootMode boot_mode;
    MemoryRegion periph_bus_ram;
    MemoryRegion adc_evt_iomem[3];

    /*
     * ETH 外设反向指针：供 ch32_eth_meip_resync 访问 PFIC 和 CPU 状态。
     * ch32-v.c 在 machine init 中将创建的 Ch32EthState 写入此字段。
     */
    Ch32EthState *eth_state;

    /*
     * ETH10M 外设反向指针（CH32V20x_D8 专有 10M MAC）。
     * ch32-v.c 在 machine init 中将创建的 Ch32Eth10mState 写入此字段。
     */
    struct Ch32Eth10mState *eth10m_state;

    /* WWDG 窗口看门狗 */
    MemoryRegion wwdg_iomem;
    uint32_t wwdg_ctlr;   /* 写入的计数器值（bit6:0=T，bit6=WDGA） */
    uint32_t wwdg_cfgr;   /* 窗口值+预分频+EWI */
    uint32_t wwdg_statr;  /* bit0=EWIF 早期唤醒中断标志 */

    /* BKP 备份寄存器域 */
    MemoryRegion bkp_iomem;
    uint16_t bkp_datar[CH32_BKP_NREGS]; /* DATAR1~42（16-bit 每个）*/
    uint32_t bkp_octlr;   /* 校准控制寄存器 */
    uint32_t bkp_tpcsr;   /* 入侵检测控制/状态 */

    /* PWR 电源控制 */
    MemoryRegion pwr_iomem;
    uint32_t pwr_ctlr;    /* PWR_CTLR：bit8=DBP（后备域写使能）*/
    uint32_t pwr_csr;     /* PWR_CSR */

    /* SDIO stub */
    MemoryRegion sdio_iomem;
    uint32_t sdio_reg[20]; /* POWER/CLKCR/ARG/CMD/RESPCMD/RESP1-4/... */
    uint32_t sdio_sta;     /* STA 寄存器（独立维护） */

    /* SPI1/SPI2/SPI3 stub */
    MemoryRegion spi1_iomem;
    MemoryRegion spi2_iomem;
    MemoryRegion spi3_iomem;
    Ch32SpiState spi[CH32_SPI_NUM];

    /* I2C1/I2C2 stub */
    MemoryRegion i2c1_iomem;
    MemoryRegion i2c2_iomem;
    Ch32I2cState i2c[CH32_I2C_NUM];

    /* USART2/3 + UART4~8 轻量级仿真（支持 DMA1/DMA2，无 char backend）*/
    Ch32Usart23State usart2;
    Ch32Usart23State usart3;
    Ch32Usart23State usart4;
    Ch32Usart23State usart5;
    Ch32Usart23State usart6;
    Ch32Usart23State usart7;
    Ch32Usart23State usart8;

    /* RTC 实时计数基准（基于 QEMU 虚拟时钟）*/
    uint64_t rtc_base_ns;    /* 写入 CNT 时的 QEMU 时钟快照 */
    uint32_t rtc_cnt0;       /* 写入时的秒计数基准 */
    bool     rtc_cnt_set;    /* 是否已初始化 */
    uint64_t rtc_secf_ns;    /* 最近一次 SECF 产生的时钟快照（以秒对齐） */
    uint32_t rtc_alr;        /* 闹钟比较值（32-bit） */

    /* RNG 控制寄存器 */
    uint32_t rng_ctlr;

    /* CRC 计算单元状态 */
    MemoryRegion crc_iomem;
    uint32_t crc_dr;    /* DATAR：当前累计 CRC，复位值 0xFFFFFFFF */
    uint8_t  crc_idr;   /* IDATAR：独立 8 位缓冲，不受 RST 影响 */

    MemoryRegion iwdt_iomem;
    QEMUTimer *iwdt_timer;
    QEMUTimer *iwdt_flag_timer;
    uint32_t iwdt_statr;
    uint32_t iwdt_rldr;
    uint32_t iwdt_pscr;
    bool iwdt_enabled;
    bool iwdt_unlock;

    /*
     * USBHS 主机：USBBus + 根端口连接 QEMU 内置设备（默认 usb-hub），MMIO 侧将
     * HOST_EP_PID 事务同步提交给 usb_handle_packet；中断经 MIP_MEIP +
     * env->wch_evt_mcause_override（NVIC 85）在 VTF 下映射到 EVT（见 cpu_helper）。
     */
    USBBus ch32_usb_bus;
    USBPort ch32_usb_rhport;
    USBPort ch32_usb_dummy_port; /* 哑端口：使 bus->nfree>=2，防 QEMU 自动创建 hub */
    USBPacket ch32_usb_pkt;
    bool ch32_usb_pkt_inited;
    bool ch32_usb_host_inited;

    /*
     * USBHS/USBFS HOST_CTRL 上次写入值缓存：用于 UH_BUS_RESET 边沿检测，
     * 避免固件 memset 寄存器区时多次触发 usb_device_reset。
     */
    uint8_t ch32_usbhs_last_host_ctrl;
    uint8_t ch32_usbfs_last_host_ctrl;

    /*
     * TIM2~TIM7 状态数组（嵌入 machine，避免模块级全局静态数组导致的状态规范问题）。
     * ch32-tim.c 笔记 m->tims[i] 车首，其余日志 / 迁移框架可直接访问。
     */
    Ch32TimState tims[CH32_TIM_COUNT];

    /*
     * 指向当前 machine 对应的 Ch32BoardDesc（由 ch32_machine_init 写入）。
     * 外设（如 RCC）可由此读取 per-板参数（hsi_hz/hse_hz/flash_size 等），
     * 避免暴露 board 表到每个外设。
     */
    const Ch32BoardDesc *board;
} Ch32MachineState;

extern const MemoryRegionOps ch32_flashreg_ops;
extern const MemoryRegionOps ch32_rcc_ops;
extern const MemoryRegionOps ch32_stk_ops;
extern const MemoryRegionOps ch32_pfic_ops;  /* 统一 PFIC IO handler */
extern const MemoryRegionOps ch32_rtc_ops;
extern const MemoryRegionOps ch32_rng_ops;
extern const MemoryRegionOps ch32_crc_ops;
extern const MemoryRegionOps ch32_adc_evt_ops;
extern const MemoryRegionOps ch32_gpio_ops;
extern const MemoryRegionOps ch32_afio_exti_ops;
extern const MemoryRegionOps ch32_iwdt_ops;
extern const MemoryRegionOps ch32_dma_ops;
extern const MemoryRegionOps ch32_usbhs_ops;
extern const MemoryRegionOps ch32_usbfs_ops;
extern const MemoryRegionOps ch32_wwdg_ops;
extern const MemoryRegionOps ch32_bkp_ops;
extern const MemoryRegionOps ch32_pwr_ops;
extern const MemoryRegionOps ch32_sdio_ops;
extern const MemoryRegionOps ch32_spi_ops;
extern const MemoryRegionOps ch32_i2c_ops;
void ch32_stk_tick(void *opaque);
void ch32_stk_pfic_reset(Ch32MachineState *m);
void ch32_pfic_irq_active_set(Ch32MachineState *m, unsigned irq_n);
void ch32_pfic_irq_active_clear(Ch32MachineState *m, unsigned irq_n);
/*
 * ch32_pfic_resync_pending_meip - MEIP owner 释放后，重扫所有外设中断。
 * 定义在 ch32-stk-pfic.c 。
 */
void ch32_pfic_resync_pending_meip(Ch32MachineState *m);
/*
 * ch32_pfic_on_mret - VTF mret 回调：清 IACTR/override/MEIP，重扫挂起中断。
 * ch32_pfic_register_mret_cb - 将 mret 回调注册到 CPU env，machine init 时调用。
 */
void ch32_pfic_on_mret(CPURISCVState *env, uint32_t irq_n, void *opaque);
void ch32_pfic_register_mret_cb(Ch32MachineState *m);
void ch32_gpio_afio_reset(Ch32MachineState *m);
void ch32_gpio_init_mr(Ch32MachineState *m, Object *owner);
void ch32_afio_exti_init_mr(Ch32MachineState *m, Object *owner);
void ch32_iwdt_reset(Ch32MachineState *m);
void ch32_iwdt_init_mr(Ch32MachineState *m, Object *owner);
void ch32_usb_init_mr(Ch32MachineState *m, Object *owner);
void ch32_usb_reset_defaults(Ch32MachineState *m);
void ch32_usbhost_machine_init(Ch32MachineState *m);
void ch32_usbhost_machine_finalize(Ch32MachineState *m);
void ch32_usbhost_ep_pid_write(Ch32MachineState *m, uint8_t ep_pid);
void ch32_usbhost_host_ctrl_write(Ch32MachineState *m, uint8_t v);
void ch32_usbhost_meip_resync(Ch32MachineState *m);
void ch32_usbhost_resync_root_mmio(Ch32MachineState *m);
/* USBFS 主机（Full-Speed Host，IRQ=83） */
void ch32_usbfs_host_machine_init(Ch32MachineState *m);
void ch32_usbfs_host_machine_finalize(Ch32MachineState *m);
void ch32_usbfs_host_ep_pid_write(Ch32MachineState *m, uint8_t ep_pid);
void ch32_usbfs_host_ctrl_write(Ch32MachineState *m, uint8_t v);
void ch32_usbfs_host_meip_resync(Ch32MachineState *m);
void ch32_usbfs_host_resync_root_mmio(Ch32MachineState *m);
void ch32_dma_init_mr(Ch32MachineState *m, Object *owner);
void ch32_dma_reset(Ch32MachineState *m);
void ch32_dma_meip_resync(Ch32MachineState *m);
void ch32_usart1_init_mr(Ch32MachineState *m, Object *owner);
void ch32_usart1_meip_resync(Ch32MachineState *m);
void ch32_usart1_reset(Ch32MachineState *m);
void ch32_usart1_dma_rx_pump(Ch32MachineState *m);
void ch32_usart_lite_init_mr(Ch32MachineState *m, Object *owner);
void ch32_usart_lite_reset(Ch32MachineState *m);
void ch32_usart_lite_meip_resync(Ch32MachineState *m);
void ch32_usart_lite_push_rx_byte(Ch32MachineState *m, unsigned idx, uint8_t b);
void ch32_bkp_reset(Ch32MachineState *m);

/*
 * RCC 动态频率接口（实现在 ch32-rcc.c）。
 * 根据 CFGR0 当前值的 SW/PLLSRC/PLLMUL/HPRE/PPRE1 动态计算 HCLK / PCLK1。
 * HSI/HSE 均按 CH32V30x 典型 8 MHz 建模；PLL_input = PLLSRC ? HSE : HSI/2。
 * STK (ch32-stk-pfic.c) 与 TIM2~TIM7 (ch32-tim.c) 的频率计算错误由此两函数划一。
 */
uint32_t ch32_rcc_get_hclk_hz(const Ch32MachineState *m);
uint32_t ch32_rcc_get_pclk1_hz(const Ch32MachineState *m);
/*
 * RCC 写 CFGR0（SW/HPRE/PPRE1/PLLSRC/PLLMUL）导致 HCLK/PCLK1 变更时，
 * STK 与 TIM 的 cnt_base/epoch_ns 需以旧频率结算后重锚，再按新频率接续。
 */
void ch32_stk_rcc_clock_changed(Ch32MachineState *m);
void ch32_tim_rcc_clock_changed(Ch32MachineState *m);

void ch32_spi_reset(Ch32MachineState *m);
void ch32_spi_init_mr(Ch32MachineState *m, Object *owner);
void ch32_i2c_reset(Ch32MachineState *m);
void ch32_i2c_init_mr(Ch32MachineState *m, Object *owner);

/* TIM2~TIM7 基址宏 */
#define CH32_TIM2_BASE  0x40000000ULL
#define CH32_TIM3_BASE  0x40000400ULL
#define CH32_TIM4_BASE  0x40000800ULL
#define CH32_TIM5_BASE  0x40000C00ULL
#define CH32_TIM6_BASE  0x40001000ULL
#define CH32_TIM7_BASE  0x40001400ULL

/* TIM2~TIM7 IRQ 号 */
#define CH32_TIM2_IRQ_N  44u
#define CH32_TIM3_IRQ_N  45u
#define CH32_TIM4_IRQ_N  46u
#define CH32_TIM5_IRQ_N  66u
#define CH32_TIM6_IRQ_N  70u
#define CH32_TIM7_IRQ_N  71u

/* 以太网 ETH_IRQn=77 中断到 PFIC/MEIP 同步（在 ch32-eth-dwmac.c 中实现） */
void ch32_eth_meip_resync(Ch32MachineState *m);
/*
 * ch32_eth_set_machine - 设置 ETH 外设与 machine 的双向关联。
 * 在 ch32-v.c machine init 中调用，使 ch32_eth_meip_resync 能访问 PFIC/CPU。
 */
void ch32_eth_set_machine(DeviceState *eth_dev, Ch32MachineState *m);

/* ETH10M IRQn=61 中断到 PFIC/MEIP 同步（在 ch32-eth-10m.c 中实现） */
void ch32_eth_10m_meip_resync(Ch32MachineState *m);
/*
 * ch32_eth_10m_set_machine - 设置 ETH10M 外设与 machine 的双向关联。
 * 在 ch32-v.c machine init 中调用。
 */
void ch32_eth_10m_set_machine(DeviceState *eth_dev, Ch32MachineState *m);

/* TIM2~TIM7 MMIO 仿真（在 ch32-tim.c 中实现） */
void ch32_tim_init_all(Ch32MachineState *m, Object *owner);
void ch32_tim_reset(Ch32MachineState *m);
void ch32_tim_meip_resync_all(Ch32MachineState *m);

/*
 * Intel HEX 加载器（实现在 ch32-hex-loader.c）：
 * 将 HEX 文件按地址写入 flash_ram[0..flash_sz)，受限于两个窗口：
 *   [CH32_FLASH_BASE, CH32_FLASH_BASE+flash_sz)（含 ELA 记录）
 *   [0, flash_sz)（无 ELA，WCH MRS 工具链常见）
 * 含 type-05 记录时更新 *entry；返回 false 表示失败（已 error_report）。
 * out_have_ela：不为 NULL 时输出是否见过 type-04，供调用方决定复位 PC。
 */
bool ch32_load_hex_image(uint8_t *flash_ram, uint64_t flash_sz,
                         const char *path, uint64_t *entry,
                         bool *out_have_ela);

/*
 * Flash 首指修正与入口推断（实现在 ch32-boot.c）：
 *   ch32_decode_jal_target - 解码 RV32I JAL 立即数并返回目标地址
 *   ch32_flash_patch_vector_jal_if_needed -
 *       WCH ISP 生成的固件首 4 字节常为“jal x0, +0x20200”，若该目标
 *       不是合法的 auipc gp 入口则将其修正为“jal x0, +0x20201C”。
 */
uint32_t ch32_decode_jal_target(uint64_t pc, uint32_t inst);
void ch32_flash_patch_vector_jal_if_needed(uint8_t *flash_ram, uint64_t flash_sz);

/*
 * ESIG/DBGMCU IDCODE 内容填充（实现在 ch32-boot.c）：
 * 根据 bd->tag 写入 chip_id、flash_kb、UID（高预测定值）。
 */
void ch32_esig_populate(uint8_t *p, size_t sz, const Ch32BoardDesc *bd,
                        uint64_t flash_size_bytes);

/*
 * =========================================================
 *  MMIO 访问日志辅助（qemu_log_mask 薄封装）
 * ---------------------------------------------------------
 *  依据项目规范：所有 CH32 外设 MMIO 访问必须对非法偏移、
 *  错误宽度等异常路径打 LOG_GUEST_ERROR / LOG_UNIMP，避免
 *  静默失败。全部外设使用统一的薄封装，方便日后
 *  统一调整日志格式与过滤策略。
 *
 *  helper 均为 static inline，对 qemu_log_mask 的快速返回路径
 *  不引入额外开销（无匹配时 mask 只涉一次整数与比较）。
 * =========================================================
 */

/* 越界或非对齐等非法偏移 */
static inline void ch32_mmio_log_bad_offset(const char *peripheral,
                                            hwaddr addr, unsigned size,
                                            bool is_write)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "ch32 %s: bad %s offset 0x%" HWADDR_PRIx " size=%u\n",
                  peripheral, is_write ? "write" : "read", addr, size);
}

/* 非法宽度（预期 1/2/4 之外的 size） */
static inline void ch32_mmio_log_bad_width(const char *peripheral,
                                           hwaddr addr, unsigned size,
                                           bool is_write)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "ch32 %s: bad %s width %u @ 0x%" HWADDR_PRIx "\n",
                  peripheral, is_write ? "write" : "read", size, addr);
}

/* 尚未模拟的寄存器/模块访问 */
static inline void ch32_mmio_log_unimp(const char *peripheral,
                                       hwaddr addr, unsigned size,
                                       bool is_write)
{
    qemu_log_mask(LOG_UNIMP,
                  "ch32 %s: unimp %s @ 0x%" HWADDR_PRIx " size=%u\n",
                  peripheral, is_write ? "write" : "read", addr, size);
}

#endif /* HW_RISCV_CH32_MACHINE_INTERNAL_H */
