# QEMU 上游增量补丁（WCH / CH32 / QingKe）

本目录存放对 **官方 QEMU 源码树** 的 **小范围、可 `patch(1)` 应用** 的修改，目标版本见仓库根目录 **`build-wch-qemu.sh`** 中的 **`WCH_QEMU_VERSION`**（当前默认 **10.2.2**）。

## 与 `qemu-overlay/` 的分工

| 方式 | 路径 | 用途 |
|------|------|------|
| **整文件覆盖** | `qemu-overlay/<版本>/hw/riscv/ch32-*.c`、`ch32-machine-internal.h`、`qemu-overlay/<版本>/target/riscv/*` | CH32 整机模型（**`ch32-v.c`** 板级入口 + **`ch32-flash.c`** / **`ch32-rcc.c`** / **`ch32-stk-pfic.c`** 等外设拆分）、QingKe **XW/HPE** 译码与 CPU 类型、WCH 自定义 CSR（`0x800`/`0x804`/`0xbc0`）、`mtvec mode=3`（VTF）支持等；构建脚本会将 `hw/riscv/ch32*.c / ch32*.h` 与 `target/riscv/` 下所有覆盖文件整体拷入官方树。 |
| **本目录 `*.patch`** | `patches/qemu/*.patch` | 在官方树中插入 **Kconfig / meson** 入口——这些文件体积小、与 WCH 代码无关，放入 overlay 意义不大，故用 patch 方式管理。 |

**构建顺序**（`build-wch-qemu.sh`）：解压官方包 → **先**拷贝 `qemu-overlay/` → **再**按文件名排序应用本目录下 **`*.patch`**（不含本 README）。

> **注意**：`target/riscv/csr.c`（含 `write_mtvec` mode=3 修改与 WCH CSR 0x800/0x804/0xbc0 实现）
> 已作为完整文件放入 `qemu-overlay/10.2.2/target/riscv/csr.c`，由构建脚本覆盖安装，
> **无需** 额外 patch 文件。

## 补丁列表（按应用顺序）

| 文件 | 目标文件 | 说明 |
|------|----------|------|
| **`0001-hw-riscv-wch-ch32-kconfig-meson.patch`** | `hw/riscv/Kconfig`<br>`hw/riscv/meson.build` | 注册 **`CONFIG_CH32V317`** Kconfig 符号（`select UNIMP` + `select RISCV_ACLINT`），并在 meson.build 中将全部 CH32 外设源文件加入 `CONFIG_CH32V317` 编译条目。当前包含：`ch32-v.c`、`ch32-flash.c`、`ch32-rcc.c`、`ch32-stk-pfic.c`、`ch32-rtc.c`、`ch32-rng.c`、`ch32-adc.c`、`ch32-wwdg.c`、`ch32-bkp.c`、`ch32-pwr.c`、`ch32-sdio.c`、`ch32-crc.c`、`ch32-eth-dwmac.c`、`ch32-eth-10m.c`、`ch32-usart.c`、`ch32-iwdt.c`、`ch32-gpio-afio.c`、`ch32-usb.c`、`ch32-usb-host.c`、`ch32-dma.c`、`ch32-tim.c`、**`ch32-spi.c`**、**`ch32-i2c.c`**。 |

## overlay 文件对应关系

下表说明 `qemu-overlay/10.2.2/` 中各文件覆盖的官方树路径及主要功能，帮助维护者
快速定位需要修改的源文件：

### `hw/riscv/`（整机模型与外设）

| 覆盖文件 | 主要功能 |
|----------|----------|
| `ch32-v.c` | 板级入口：多台 `-M ch32vXXX` 机器注册、Flash/SRAM/外设实例化、启动模式（`boot-mode`）、HEX/bin 加载 |
| `ch32-machine-internal.h` | 整机内部共享宏与类型 |
| `ch32-flash.c` | Flash 仿真 RAM（读写透明）、启动别名（`0x00000000`→Flash/SRAM/SysMem） |
| `ch32-rcc.c` | RCC 时钟控制寄存器建模（HSI/HSE/PLL 就绪位自动置位、CFGR SWS 跟随 SW、APB1/APB2 时钟使能） |
| `ch32-stk-pfic.c` | PFIC（IENR/IRER/IPSR/IPRR/IACTR/IPRIOR/VTFIDR/VTFADDR/GISR/SCTLR）完整 MMIO；SysTick（STK）；VTF 快速中断；`mret` 回调清 IACTR；CFGR 软件复位 |
| `ch32-usart.c` | USART1 MMIO（STATR/DATAR 布局）；`-serial` 后端对接；RXNE 中断 |
| `ch32-eth-dwmac.c` | EMAC+DMA 以太网（CH182 PHY 100M 或内置 10M PHY）；TX/RX COE；tap 后端；WDT 后恢复 |
| `ch32-eth-10m.c` | CH32V203RBT6 ENC28J60 兼容 ETH10M 控制器 |
| `ch32-gpio-afio.c` | GPIO/AFIO/EXTI 寄存器桩 |
| `ch32-dma.c` | DMA 寄存器桩 |
| `ch32-tim.c` | TIM 定时器寄存器桩 |
| `ch32-spi.c` | SPI 寄存器桩 |
| `ch32-i2c.c` | I2C 寄存器桩 |
| `ch32-usb.c` | USBHS（高速，含主机模式真实枚举）+ USBFS（全速 OTG 桩）MMIO |
| `ch32-usb-host.c` | USBHS 主机事务桥（连接 QEMU USB 栈） |
| `ch32-rcc.c` | 见上 |
| `ch32-rtc.c` | RTC 寄存器建模（CNTL/CNTH/ALRL/ALRH、写保护、SECF/ALRF 标志） |
| `ch32-rng.c` | RNG 寄存器（CTLR/STATR/DATAR，DRDY 位自动置位） |
| `ch32-adc.c` | ADC 寄存器桩 |
| `ch32-wwdg.c` | WWDG 寄存器桩 |
| `ch32-bkp.c` | BKP 备份域寄存器桩 |
| `ch32-pwr.c` | PWR 电源控制寄存器桩 |
| `ch32-sdio.c` | SDIO 寄存器桩 |
| `ch32-crc.c` | CRC 计算单元（硬件 CRC32，IDAT 写触发计算） |
| `ch32-iwdt.c` | IWDG 寄存器建模（写键序列 0x5555→0xCCCC 启动、写 0xAAAA 喂狗、超时 QEMUTimer 触发系统复位） |

### `target/riscv/`（CPU 模型）

| 覆盖文件 | 主要功能 |
|----------|----------|
| `cpu.c` | 所有 WCH QingKe CPU 型号注册（V2A/V2C/V3A～V3V/V4A～V4J/V5F）；`TYPE_RISCV_CPU_WCH_QINGKE_BASE` 抽象基类；`TYPE_RISCV_CPU_BASE32`（`rv32`）恢复继承 `DYNAMIC_CPU` |
| `cpu.h` | CPU 状态扩展（`wch_intsyscr`、HPE 快照寄存器等）|
| `cpu-qom.h` | WCH QingKe CPU 类型名宏（`TYPE_RISCV_CPU_WCH_QINGKE_BASE` 等）|
| `cpu_cfg_fields.h.inc` | `ext_xw`、`ext_hpe`、`ext_xwchmcpy` 等 WCH 专有 CPU 配置字段 |
| `cpu_helper.c` | HPE（硬件压栈/弹栈）入口/出栈实现 |
| `csr.c` | WCH 自定义 CSR（`0x800` gintenr、`0x804` intsyscr、`0xbc0` wch_misc）；`write_mtvec` 支持 mode=3（VTF/HPE）|
| `op_helper.c` | `mret` 时 HPE 弹栈、PFIC mret 回调触发 |
| `translate.c` | WCH XW/mcpy 译码入口（`#include "insn_trans/trans_rvwch.c.inc"`）|
| `insn_trans/trans_rvwch.c.inc` | XW 16 位压缩访存（`c.lbu`/`c.lhu`/`c.sb`/`c.sh` 等）；mcpy 指令（`xwchmcpy`）TCG 实现 |
| `wch-qingke.h` | WCH QingKe 内部共享头（HPE 快照寄存器偏移、宏等）|

## 维护：新增外设文件时如何更新补丁

向 `qemu-overlay/10.2.2/hw/riscv/` 添加新 `ch32-xxx.c` 后，需同步更新 `0001` 补丁：

```bash
# 1. 将新文件名加入 build 目录的 meson.build（手动编辑）
# 2. 在干净的原始 QEMU 目录上重新生成 diff：
cd /path/to/WCH-QingKe-RISC-V-Toolchain
tar -xJf downloads/archives/qemu-10.2.2.tar.xz -C /tmp \
    qemu-10.2.2/hw/riscv/Kconfig qemu-10.2.2/hw/riscv/meson.build
diff -u /tmp/qemu-10.2.2/hw/riscv/Kconfig    build/qemu-10.2.2/hw/riscv/Kconfig    >  patches/qemu/0001-hw-riscv-wch-ch32-kconfig-meson.patch
diff -u /tmp/qemu-10.2.2/hw/riscv/meson.build build/qemu-10.2.2/hw/riscv/meson.build >> patches/qemu/0001-hw-riscv-wch-ch32-kconfig-meson.patch
# 3. 将 a/b 路径格式化为标准 patch 格式（去掉 /tmp/qemu-10.2.2/ 前缀改为 a/）
# 4. 验证：patch -p1 --dry-run < patches/qemu/0001-hw-riscv-wch-ch32-kconfig-meson.patch
```

> **`target/riscv/csr.c` 说明**：该文件由 overlay 整体覆盖，无需独立 patch。
> 修改 `write_mtvec`、WCH CSR 实现等时，直接编辑
> `qemu-overlay/10.2.2/target/riscv/csr.c` 并同步到 `build/` 目录即可。
