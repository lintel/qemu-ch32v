[English](README_en.md) | 中文

**欢迎通过社区参与开发，支持国产MCU生态，义无反顾！**

# QEMU CH32V — 沁恒 QingKe RISC-V MCU Qemu仿真系统

基于 **QEMU 10.2.2** 的 **WCH CH32 / QingKe** MCU 整机仿真环境，支持 CH32V003 ~ CH32V407 / CH32H417 全系列。在 PC 上即可完成固件的指令级验证、USART 串口调试、以太网协议栈联调、USB 设备枚举，以及 GDB 断点调试——无需真机硬件。

## ✨ 特性

- **计划支持9 款MCU**：CH32V003 / V103 / V203 / V203RB / V303 / V305 / V307 / V317 / V407 / H417
- **QingKe 全系 CPU**：V2A ~ V5F（含 XW 压缩访存、HPE 硬件压栈、mcpy 指令）
- **PFIC / SysTick / VTF 中断系统**：完整 MMIO 建模，支持 8 级嵌套与 VTF 快速中断
- **以太网仿真**：片内 EMAC+DMA，CH182 / 10M / RTL8211F 三种 PHY 模型，tap 后端双向通信
- **USBHS / USBFS 主机模式**：真实枚举下游 USB 设备（键盘、CH341 透传等）
- **PMP 物理内存保护**：4 组规则，NAPOT/NA4/TOR 模式，WCH 差异行为建模
- **多种启动模式**：Flash / System Memory / SRAM，与手册表 1-1 一致
- **固件装载**：ELF（`-kernel`）、裸 `.bin`、Intel HEX（`flash-image=`）
- **GDB 调试**：内置 GDB stub，支持断点、单步、寄存器/内存查看

## 📦 仓库结构

```
qemu-ch32v/
├── build-wch-qemu.sh          # 一键构建脚本（中文提示）
├── build-wch-qemu-en.sh       # 一键构建脚本（英文提示）
├── qemu-overlay/10.2.2/       # CH32 整机模型 & QingKe CPU 源码
│   ├── hw/riscv/ch32-*.c/.h   #   板级入口 + 外设模块（25+ 文件）
│   └── target/riscv/          #   QingKe CPU 定义、XW/HPE/mcpy 译码
├── patches/qemu/              # QEMU 上游增量补丁（Kconfig / meson）
├── docs/
│   ├── QingKe-MCU-QEMU使用手册.md     # 完整使用手册
│   ├── CH32FV2x_V3xRM-寄存器详述/     # 外设寄存器位域详述（29 份）
│   └── Datasheet/                      # 官方数据手册 PDF
└── LICENSE                    # CC0 1.0
```

## 🚀 快速开始

### 1. 构建 QEMU

**环境要求**：Linux x86_64，Debian/Ubuntu 上脚本会自动安装依赖。

```bash
git clone <本仓库>
cd qemu-ch32v

# 查看帮助
./build-wch-qemu.sh --help

# 构建（默认安装到 dist/qemu-10.2.2-riscv32/）
./build-wch-qemu.sh -j$(nproc)

# 加入 PATH
export PATH="$PWD/dist/qemu-10.2.2-riscv32/bin:$PATH"
qemu-system-riscv32 --version
```

构建流程：下载官方 QEMU 10.2.2 源码包 → 覆盖 CH32 模型源码 → 应用补丁 → configure（仅 `riscv32-softmmu`）→ 编译安装。

### 2. 运行固件

```bash
# CH32V317 整机
qemu-system-riscv32 \
  -M ch32v317 \
  -display none \
  -serial stdio \
  -kernel your-app.elf

# CH32V203 整机
qemu-system-riscv32 \
  -M ch32v203 \
  -display none \
  -serial stdio \
  -kernel your-app.elf

# 带以太网（需先配置 tap，详见使用手册 §9）
qemu-system-riscv32 \
  -M ch32v317 \
  -display none \
  -serial stdio \
  -nic "tap,ifname=tap0,script=no,downscript=no" \
  -kernel your-app.elf

# 使用 Intel HEX 或裸二进制
qemu-system-riscv32 \
  -M ch32v317,flash-image=firmware.hex \
  -display none \
  -serial stdio

# GDB 调试
qemu-system-riscv32 \
  -M ch32v317 \
  -display none \
  -serial stdio \
  -kernel your-app.elf \
  -s -S
# 另一终端：riscv32-wch-elf-gdb your-app.elf -ex "target remote :1234"
```

## 🖥️ 支持的机器与 CPU

### 机器（`-M`）

| 机器 | Flash | SRAM | 默认 CPU | 以太网 |
|------|-------|------|----------|--------|
| `ch32v317` | 480 KiB | 192 KiB | `wch-qingke-v4f` | ✅ CH182 100M |
| `ch32v307` | 256 KiB | 64 KiB | `wch-qingke-v4f` | ✅ 内置 10M |
| `ch32v305` | 288 KiB | 64 KiB | `wch-qingke-v4f` | ✅ CH182 100M |
| `ch32v303` | 256 KiB | 64 KiB | `wch-qingke-v4f` | ✅ CH182 100M |
| `ch32v203` | 256 KiB | 64 KiB | `wch-qingke-v4b` | — |
| `ch32v203rb` | 128 KiB | 64 KiB | `wch-qingke-v4b` | ✅ ETH10M |
| `ch32v103` | 64 KiB | 20 KiB | `wch-qingke-v3a` | — |
| `ch32v003` | 16 KiB | 2 KiB | `wch-qingke-v2c` ⚠ | — |
| `ch32v407` | 992 KiB | 200 KiB | `wch-qingke-v3v` | ✅ |
| `ch32h417` | 896 KiB ⚠ | 192 KiB ⚠ | `wch-qingke-v5f` ⚠ | — |

> ⚠ `ch32v003` 真机为 V2A，QEMU 用 V2C 占位；`ch32h417` 为占位实现。

### CPU（`-cpu`）

| CPU | 架构 | 特权模式 | XW | HPE | mcpy |
|-----|------|----------|----|----|------|
| `wch-qingke-v2a` / `v2c` | RV32EC | M | ✅ | ✅ | — |
| `wch-qingke-v3a` | RV32IMAC | M + U | — | ✅ | — |
| `wch-qingke-v3b` | RV32I[M]C | M + U | ✅ | ✅ | — |
| `wch-qingke-v3c` | RV32IMCB | M + U | ✅ | ✅ | — |
| `wch-qingke-v3f` | RV32IMAFCB | M + U | ✅ | ✅ | ✅ |
| `wch-qingke-v3v` | RV32IMACB | M + U | ✅ | ✅ | ✅ |
| `wch-qingke-v4a` | RV32IMAC | M + U | — | ✅ | — |
| `wch-qingke-v4b` / `v4c` / `v4j` | RV32IMAC | M + U | ✅ | ✅ | — |
| `wch-qingke-v4f` | RV32IMAFC | M + U | ✅ | ✅ | — |
| `wch-qingke-v5f` | RV32IMAFCB | M + U | ✅ | ✅ | — |

### MCU 型号支持一览

下表列出沁恒 CH32 / QingKe 各 MCU 系列与本仿真环境的对应关系：

| MCU 系列 | 代表型号 | QingKe 内核 | QEMU 机器（`-M`） | 支持状态 |
|----------|----------|-------------|-------------------|----------|
| **CH32V003** | CH32V003F4P6, CH32V003A4M6, CH32V003J4M6 | V2A (RV32EC) | `ch32v003` | ⚠ CPU 用 V2C 占位 |
| **CH32V002/004/005/006/007** | CH32V006K8U6 等 | V2C (RV32EC+Zmmul) | `ch32v003` | ⚠ 共用 V003 机器 |
| **CH32V103** | CH32V103C8T6, CH32V103R8T6 | V3A (RV32IMAC) | `ch32v103` | ✅ |
| **CH32L103** | CH32L103C8T6 | V3A (RV32IMAC) | `ch32v103` | ✅ 共用 V103 机器 |
| **CH32V203** | CH32V203C8T6, CH32V203C6T6, CH32V203K8T6 | V4B (RV32IMAC+XW) | `ch32v203` | ✅ |
| **CH32V203RBT6** | CH32V203RBT6 (D8 封装，含 ETH10M) | V4B (RV32IMAC+XW) | `ch32v203rb` | ✅ 含 10M 以太网 |
| **CH32V208** | CH32V208WBU6, CH32V208GBU6 | V4C (RV32IMAC+XW) | `ch32v203` | ⚠ 共用 V203，BLE 不仿真 |
| **CH32V303** | CH32V303VCT6, CH32V303RCT6, CH32V303CBT6 | V4F (RV32IMAFC+XW) | `ch32v303` | ✅ |
| **CH32V305** | CH32V305FBP6, CH32V305RBT6 | V4F (RV32IMAFC+XW) | `ch32v305` | ✅ 含以太网 |
| **CH32V307** | CH32V307VCT6, CH32V307RCT6, CH32V307WCU6 | V4F (RV32IMAFC+XW) | `ch32v307` | ✅ 含 10M 以太网 |
| **CH32V317** | CH32V317WCU6, CH32V317VCT6 | V4F (RV32IMAFC+XW) | `ch32v317` | ✅ 含 100M 以太网 |
| **CH32V407** | CH32V407VET6 等 | V3V (RV32IMACB+XW) | `ch32v407` | ⚠ 向量子集未实现 |
| **CH32V467** | CH32V467VET6 等 | V3V (RV32IMACB+XW) | `ch32v407` | ⚠ 共用 V407，PSRAM 未仿真 |
| **CH32H417** | CH32H417VET6 等 | V5F (RV32IMAFCB+XW) | `ch32h417` | ⚠ 单核占位 |
| **CH571/CH573** | CH573F, CH571D 等 | V3A (RV32IMAC) | `ch32v103` | ⚠ BLE 不仿真 |
| **CH581/CH582/CH583** | CH582M, CH583F 等 | V4A (RV32IMAC) | `ch32v203` | ⚠ BLE 不仿真 |
| **CH584/CH585** | CH584M 等 | V4C (RV32IMAC+XW) | `ch32v203` | ⚠ BLE 不仿真 |
| **CH32X033/X035** | CH32X035G8U6 等 | V3B (RV32I[M]C+XW) | `ch32v103` | ⚠ 外设差异较大 |
| **CH591/CH592** | CH592F 等 | V3B (RV32I[M]C+XW) | `ch32v103` | ⚠ BLE 不仿真 |
| **CH643/CH645** | CH643Q, CH645Q 等 | V4C (RV32IMAC+XW) | `ch32v203` | ⚠ RGB LED 不仿真 |

**图例**：✅ = 主要验证目标，外设与内存布局对齐　|　⚠ = 可运行但存在限制（共用机器、BLE/无线不仿真、部分外设缺失）

> **提示**：所有机器均支持 `-cpu` 切换 CPU 类型（见上表白名单），可按实际芯片内核选择对应的 `-cpu wch-qingke-*`。不在上表中的型号，可选择内核与外设最接近的机器尝试运行。

## 📖 文档

- **[QingKe-MCU-QEMU 使用手册](docs/QingKe-MCU-QEMU使用手册.md)** — 完整使用文档，涵盖构建、机器/CPU 选型、固件要求、网络仿真、USB 仿真、GDB 调试、能力边界与故障排查
- **[外设寄存器详述](docs/CH32FV2x_V3xRM-寄存器详述/)** — CH32 各外设寄存器位域级文档（29 份，对照应用手册 V2.4）
- **[补丁说明](patches/qemu/README.md)** — QEMU 上游增量补丁与 overlay 文件对应关系
- **[官方数据手册](docs/Datasheet/)** — CH32V203 / V208 / V307 数据手册 PDF

## ⚙️ 构建选项

```bash
./build-wch-qemu.sh [选项]

选项:
  --prefix PATH          安装目录（默认 dist/qemu-10.2.2-riscv32）
  --downloads-dir PATH   源码包缓存目录
  --build-dir PATH       构建根目录
  -j N                   并行编译数（默认 nproc）
  --no-install-deps      跳过 apt 依赖安装
  --skip-archive-sha256  不校验源码包 SHA256
  --force-rebuild        强制重新解压源码
  --clean                删除编译目录后重新编译

环境变量:
  WCH_QEMU_VERSION       QEMU 版本号（默认 10.2.2）
  WCH_QEMU_URL           下载地址
  WCH_QEMU_SHA256        期望 SHA256
  JOBS                   并行编译数
```

## 🔧 已建模外设

| 外设 | 状态 | 说明 |
|------|------|------|
| Flash / SRAM | ✅ 完整 | 线性映射，支持启动别名 |
| USART1 | ✅ 完整 | STATR/DATAR，对接 `-serial` |
| PFIC / SysTick / VTF | ✅ 完整 | IENR/IRER/IPSR/IPRR/IACTR/IPRIOR 等完整 MMIO |
| 以太网 MAC+DMA | ✅ 完整 | TX/RX COE，三种 PHY 模型 |
| USBHS 主机 | ✅ 完整 | 设备枚举、DMA 传输、中断路由 |
| USBFS 主机 | ✅ 完整 | Low/Full 速设备枚举 |
| RCC | ✅ 建模 | HSI/HSE/PLL 就绪位、SWS 跟随、时钟使能 |
| IWDG | ✅ 建模 | 键序列、喂狗、超时复位 |
| RTC | ✅ 建模 | CNT/ALR、写保护、SECF/ALRF |
| CRC | ✅ 建模 | 硬件 CRC32 计算 |
| RNG | ✅ 建模 | DRDY 自动置位 |
| PMP | ✅ 建模 | 4 组规则，WCH 差异行为 |
| GPIO / AFIO / EXTI | 🔲 寄存器桩 | 读写往返 |
| DMA / TIM / SPI / I2C | 🔲 寄存器桩 | 读写往返 |
| ADC / DAC / WWDG / BKP / PWR / SDIO | 🔲 寄存器桩 | 读写往返 |
| USBHS 设备模式 / USBFS OTG | 🔲 寄存器桩 | 不触发传输 |
| Flash 控制器（擦写） | ✅ 建模 | — |
| PFIC 优先级仲裁 | ❌ 未实现 | 按 IRQ 编号顺序服务 |
| WFE 低功耗 | ❌ 未实现 | — |

## 📄 许可证

本仓库代码以 **CC0 1.0 Universal** 发布。QEMU 本体遵循其上游许可证（GPLv2）。
