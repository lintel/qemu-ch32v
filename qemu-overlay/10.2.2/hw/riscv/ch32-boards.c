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
 * File:     ch32-boards.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     WCH CH32 整机型号描述表（Ch32BoardDesc）。
 *
 *     从 ch32-v.c 拆出的声明型数据：每个支持的 machine（-M ch32v317 等）
 *     对应一个 const Ch32BoardDesc 实例，集中维护 flash/SRAM 尺寸、默认
 *     CPU、有效 CPU 白名单、默认网卡等 SoC 静态属性，供 ch32-v.c 的
 *     CH32_MACHINE_ONE 宏引用。新增型号只需在本文件追加定义。
 */

#include "ch32-machine-internal.h"

/*
 * valid_cpu_types：按芯片系列限制允许选择的 CPU。
 *   ch32_cpus_all   - V1/V2/V3/V4/V5 全系列（供 V203/V30x/V317/V407/H417 等）
 *   ch32_cpus_v3_line - 仅 V3A/V2A/BASE32（供 V103 老内核）
 *   ch32_cpus_v2_line - 仅 V2C/V2A/BASE32（供 V003 低端）
 */
static const char *const ch32_cpus_all[] = {
    TYPE_RISCV_CPU_WCH_QINGKE_V5F,
    TYPE_RISCV_CPU_WCH_QINGKE_V4F,
    TYPE_RISCV_CPU_WCH_QINGKE_V4J,
    TYPE_RISCV_CPU_WCH_QINGKE_V4C,
    TYPE_RISCV_CPU_WCH_QINGKE_V4B,
    TYPE_RISCV_CPU_WCH_QINGKE_V4A,
    /* V3V：CH32V407 / V467 真机内核（RV32IMABCV-X，含向量子集与 B 扩展）。 */
    TYPE_RISCV_CPU_WCH_QINGKE_V3V,
    TYPE_RISCV_CPU_WCH_QINGKE_V3F,
    TYPE_RISCV_CPU_WCH_QINGKE_V3C,
    TYPE_RISCV_CPU_WCH_QINGKE_V3B,
    TYPE_RISCV_CPU_WCH_QINGKE_V3A,
    TYPE_RISCV_CPU_WCH_QINGKE_V2C,
    TYPE_RISCV_CPU_WCH_QINGKE_V2A,
    TYPE_RISCV_CPU_BASE32,
    NULL,
};

static const char *const ch32_cpus_v3_line[] = {
    TYPE_RISCV_CPU_WCH_QINGKE_V3A,
    TYPE_RISCV_CPU_WCH_QINGKE_V2A,
    TYPE_RISCV_CPU_BASE32,
    NULL,
};

static const char *const ch32_cpus_v2_line[] = {
    TYPE_RISCV_CPU_WCH_QINGKE_V2C,
    TYPE_RISCV_CPU_WCH_QINGKE_V2A,
    TYPE_RISCV_CPU_BASE32,
    NULL,
};

const Ch32BoardDesc ch32_board_ch32v317 = {
    .tag = "ch32v317",
    .pretty = "WCH CH32V317 (QingKe V4F + CH32V317 peripherals stub)",
    /*
     * 与 OrayOS-Tiny 默认 defconfig 的 CONFIG_MCU_FLASH_SIZE=0x78000（480KiB）及
     * ProductInfo@0x08077C00 一致；448KiB 会导致读产品区 load fault。
     */
    .flash_size = 480 * KiB,
    /* 与 OrayOS-Tiny 链接脚本 RAM LENGTH=0x30000（192KiB）及 CH32V317 片内 SRAM 一致 */
    .default_ram_size = 192 * KiB,
    .default_cpu_type = TYPE_RISCV_CPU_WCH_QINGKE_V4F,
    .valid_cpu_types = ch32_cpus_all,
    .ram_id = "ch32v317.sram",
    /*
     * CH32V317 真机内置 100M CH182 PHY，同时支持 MII/RMII/RGMII 外接
     * 其它 PHY。QEMU 默认 NIC 保持旧行为 "ch32-eth-dwmac"（CH182 100M），
     * 这与现有固件、上游测试用例一致。如需用 RGMII + RTL8211F 千兆
     * 模拟，可显式使用：-nic model=ch32-eth-dwmac + phy-model="rtl8211f"，
     * 或在板级覆盖 default_nic="ch32-eth-dwmac-rtl8211f"。
     */
    .default_nic = "ch32-eth-dwmac",
    .hsi_hz = 8000000u,
    .hse_hz = 8000000u,
};

const Ch32BoardDesc ch32_board_ch32v307 = {
    .tag = "ch32v307",
    .pretty = "WCH CH32V307 (QingKe V4F + 10M PHY + CH32V30x peripherals)",
    .flash_size = 256 * KiB,
    .default_ram_size = 64 * KiB,
    .default_cpu_type = TYPE_RISCV_CPU_WCH_QINGKE_V4F,
    .valid_cpu_types = ch32_cpus_all,
    .ram_id = "ch32v307.sram",
    /*
     * CH32V307 真机内置 10M PHY（由 EXTEN_CTR.ETH_10M_EN 使能），同时支持
     * MII/RMII/RGMII 外接 PHY。QEMU 默认 NIC 保持旧行为
     * "ch32-eth-dwmac-10m"，machine_init 会设置 phy-model="phy10m"。
     * 如需用 RGMII + RTL8211F 千兆模拟，可显式使用：
     *   -nic model=ch32-eth-dwmac + phy-model="rtl8211f"，或板级覆盖
     *   default_nic="ch32-eth-dwmac-rtl8211f"。
     */
    .default_nic = "ch32-eth-dwmac-10m",
    .hsi_hz = 8000000u,
    .hse_hz = 8000000u,
};

const Ch32BoardDesc ch32_board_ch32v305 = {
    .tag = "ch32v305",
    .pretty = "WCH CH32V305 (QingKe V4F + CH32V30x peripherals stub)",
    .flash_size = 288 * KiB,
    .default_ram_size = 64 * KiB,
    .default_cpu_type = TYPE_RISCV_CPU_WCH_QINGKE_V4F,
    .valid_cpu_types = ch32_cpus_all,
    .ram_id = "ch32v305.sram",
    .default_nic = "ch32-eth-dwmac",
    .hsi_hz = 8000000u,
    .hse_hz = 8000000u,
};

const Ch32BoardDesc ch32_board_ch32v303 = {
    .tag = "ch32v303",
    .pretty = "WCH CH32V303 (QingKe V4F + CH32V30x peripherals stub)",
    .flash_size = 256 * KiB,
    .default_ram_size = 64 * KiB,
    .default_cpu_type = TYPE_RISCV_CPU_WCH_QINGKE_V4F,
    .valid_cpu_types = ch32_cpus_all,
    .ram_id = "ch32v303.sram",
    .default_nic = "ch32-eth-dwmac",
    .hsi_hz = 8000000u,
    .hse_hz = 8000000u,
};

const Ch32BoardDesc ch32_board_ch32v203 = {
    .tag = "ch32v203",
    .pretty = "WCH CH32V203 (QingKe V4B + CH32V20x peripherals stub)",
    .flash_size = 256 * KiB,
    .default_ram_size = 64 * KiB,
    .default_cpu_type = TYPE_RISCV_CPU_WCH_QINGKE_V4B,
    .valid_cpu_types = ch32_cpus_all,
    .ram_id = "ch32v203.sram",
    .default_nic = NULL,
    .hsi_hz = 8000000u,
    .hse_hz = 8000000u,
};

const Ch32BoardDesc ch32_board_ch32v203rb = {
    .tag = "ch32v203rb",
    .pretty = "WCH CH32V203RBT6 (QingKe V4B + ETH10M + CH32V20x_D8 peripherals)",
    /* CH32V203RBT6: 128 KiB Flash / 64 KiB SRAM
     * Extended to 256 KiB for WebServer firmware (HTML content).
     * QEMU does not enforce hardware flash size limits. */
    .flash_size = 256 * KiB,
    .default_ram_size = 64 * KiB,
    .default_cpu_type = TYPE_RISCV_CPU_WCH_QINGKE_V4B,
    .valid_cpu_types = ch32_cpus_all,
    .ram_id = "ch32v203rb.sram",
    .default_nic = "ch32-eth-10m",
    .hsi_hz = 8000000u,
    .hse_hz = 8000000u,
};

const Ch32BoardDesc ch32_board_ch32v103 = {
    .tag = "ch32v103",
    .pretty = "WCH CH32V103 (QingKe V3A + peripherals stub)",
    .flash_size = 64 * KiB,
    .default_ram_size = 20 * KiB,
    .default_cpu_type = TYPE_RISCV_CPU_WCH_QINGKE_V3A,
    .valid_cpu_types = ch32_cpus_v3_line,
    .ram_id = "ch32v103.sram",
    .default_nic = NULL,
    .hsi_hz = 8000000u,
    .hse_hz = 8000000u,
};

const Ch32BoardDesc ch32_board_ch32v003 = {
    .tag = "ch32v003",
    .pretty = "WCH CH32V003 (QingKe V2C + peripherals stub)",
    .flash_size = 16 * KiB,
    .default_ram_size = 2 * KiB,
    .default_cpu_type = TYPE_RISCV_CPU_WCH_QINGKE_V2C,
    .valid_cpu_types = ch32_cpus_v2_line,
    .ram_id = "ch32v003.sram",
    .default_nic = NULL,
    /* CH32V003 特殊：内置 HSI=24 MHz（RC 振荡器直接 24 MHz，无 PLL） */
    .hsi_hz = 24000000u,
    .hse_hz = 24000000u,
};

const Ch32BoardDesc ch32_board_ch32h417 = {
    .tag = "ch32h417",
    .pretty = "WCH CH32H417 (QingKe V5F + peripherals stub)",
    .flash_size = 896 * KiB,
    .default_ram_size = 768 * KiB,
    .default_cpu_type = TYPE_RISCV_CPU_WCH_QINGKE_V5F,
    .valid_cpu_types = ch32_cpus_all,
    .ram_id = "ch32h417.sram",
    .default_nic = NULL,
    .hsi_hz = 8000000u,
    .hse_hz = 8000000u,
};

/*
 * CH32V407 / V467（依据 CH32V407DS0 V1.0）：
 *   - 内核：青稞 RISC-V3V（RV32IMABCV-X，含向量子集 Zve64x+Zvbb 与 B 扩展）；
 *           对应 QEMU `wch-qingke-v3v`（Zve64x/Zvbb 尚未在 TCG 中实现）。
 *   - Code FLASH（用户区）：0x0800_0000 – 0x080F_8000 共 992 KB（480 KB 非零等待
 *     + 512 KB 零等待），另可将 SRAM 中 64 KB 重划为零等待 Flash。
 *   - 零等待 SRAM：0x2000_0000 – 0x2003_2000 共 200 KB。
 *   - SYSCLK 最高 200 MHz（QEMU 不建模时钟树）。
 *   - HSI RC **20 MHz**（出厂调校，DS §1.4.11）；HSE 外晶典型 **8 MHz**（5–32 MHz，
 *     与 EVT 常用晶振一致；PLL 走 HSE 时 SYSCLK 建模依赖该约定）。
 *   - 片内 PSRAM（仅 V467）/ FSMC / LTDC / ARGB / I3C / 双 USBHS / 10 USART 等
 *     尚未在本桩位中建模，仅保留最基本的 Flash/SRAM/USART1/PFIC 能力。
 */
const Ch32BoardDesc ch32_board_ch32v407 = {
    .tag = "ch32v407",
    .pretty = "WCH CH32V407/V467 (QingKe V3V + peripherals stub)",
    .flash_size = 992 * KiB,
    .default_ram_size = 200 * KiB,
    .default_cpu_type = TYPE_RISCV_CPU_WCH_QINGKE_V3V,
    .valid_cpu_types = ch32_cpus_all,
    .ram_id = "ch32v407.sram",
    .default_nic = NULL,
    .hsi_hz = 20000000u, /* CH32V407DS0 §1.4.11：HSI RC 20 MHz（非 V30x 的 8 MHz） */
    .hse_hz = 8000000u,  /* 典型外接 8 MHz；PLL/HSE 路径与 EVT 一致时可校准 SYSCLK */
};
