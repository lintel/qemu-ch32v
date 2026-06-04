# QingKe MCU QEMU 使用手册

| 时间 | 说明 |
|------|------|
| 2026.04.20 | 初稿：QEMU 10.2.2 构建与 `virt` 示例；真机外设差异；OrayOS-Tiny-Bin 与真机 / QEMU 分工 |
本手册说明如何在本仓库配套脚本下，从**官方源码包**构建 **QEMU 10.2.2**，用于仿真**沁恒 CH32 / QingKe MCU**：命令行速查、`-M`/`-cpu` 选型、固件要求、网络仿真、GDB 调试，以及当前模型的**能力边界**。

---

## 目录

1. [适用对象与前提](#1-适用对象与前提)
2. [构建 QEMU](#2-构建-qemu)
3. [能力边界（必读）](#3-能力边界必读)
4. [支持的机器（`-M`）](#4-支持的机器-m)
5. [CPU 类型（`-cpu`）与工具链 `-mcpu`](#5-cpu-类型-cpu与工具链-mcpu)
6. [指令级实现与处理器手册对照](#6-指令级实现与处理器手册对照)
7. [固件要求](#7-固件要求)
8. [快速运行](#8-快速运行)
9. [网络仿真（以太网 / tap 后端）](#9-网络仿真以太网--tap-后端)
10. [USB 仿真](#10-usb-仿真)
11. [常用命令行参数](#11-常用命令行参数)
12. [GDB 调试](#12-gdb-调试)
13. [当前模型能力汇总](#13-当前模型能力汇总)
14. [故障排查](#14-故障排查)
15. [相关文档与脚本](#15-相关文档与脚本)

---

## 1. 适用对象与前提

- **适用**：已使用或计划使用本仓库 QingKe 工具链（`riscv32-wch-elf-*`）开发 CH32 / QingKe 固件，希望在 PC 上做**指令级**或**轻量裸机**验证。
- **不适用**：期望「一块仿真板」完整替代 EVT 里所有外设、中断与时序（当前模型为**最小 SoC**，见 §13）。
- **前提**：已用 `build-wch-qemu.sh` 构建并安装 QEMU，且 `qemu-system-riscv32` 在 `PATH` 中，或用环境变量 `QEMU` 指向该二进制。

---

## 2. 构建 QEMU

### 2.1 一键构建与安装

```bash
cd /path/to/WCH-QingKe-RISC-V-Toolchain

# 查看帮助（含默认路径与全部选项）
./build-wch-qemu.sh --help

# 构建（Debian/Ubuntu 会自动安装缺失依赖，需 sudo）
./build-wch-qemu.sh -j$(nproc)

# 将 QEMU 加入 PATH
export PATH="$PWD/dist/qemu-10.2.2-riscv32/bin:$PATH"

# 快速验证
qemu-system-riscv32 --version
```

- **默认安装前缀**：`dist/qemu-10.2.2-riscv32/`
- **默认目标**：`riscv32-softmmu`（生成 `qemu-system-riscv32`）
- **环境变量**：`WCH_QEMU_VERSION`、`WCH_QEMU_URL`、`WCH_QEMU_SHA256`、`WCH_SKIP_ARCHIVE_SHA256=1`、`JOBS` 等

若 `configure` / Meson 报告缺少 `pixman`、`slirp` 等，在 Debian/Ubuntu 上请安装 `libpixman-1-dev`、`libslirp-dev`（完整列表见脚本内 `WCH_DEB_PKGS_QEMU`）。

### 2.2 回归验证

构建成功后，编译并运行测试固件：

```bash
# 编译 CH32V317 最小测试固件（含 XW/HPE 扩展自检）
make -C qemu-test-firmware/ch32v317-minimal

# 编译 CH32V203 最小测试固件
make -C qemu-test-firmware/ch32v203-minimal

# 运行完整回归（机器名、CPU 名、串口输出、XW/HPE 自检）
chmod +x scripts/verify-qemu-qingke.sh
./scripts/verify-qemu-qingke.sh
```

并可单独运行特权模式验证固件（验证 V3/V4/V5 的 M+U 模式）：

```bash
qemu-system-riscv32 \
  -M ch32v317 -cpu wch-qingke-v4f -m 192k \
  -monitor none -display none \
  -serial stdio \
  -kernel qemu-test-firmware/ch32v317-minimal/ch32v317-priv-verify.elf
# 期望输出：All tests PASSED（5 项，包含 misa.U=1 、U 模式 CSR 访问触发 illegal instruction）
```

### 2.3 脚本工作流程

1. 下载 / 校验 `qemu-10.2.2.tar.xz`（SHA256 可覆盖）
2. 解压到 `build/qemu-10.2.2/`
3. 覆盖 `qemu-overlay/10.2.2/hw/riscv/ch32-*.c / ch32-*.h`（CH32 整机模型）
4. 覆盖 `qemu-overlay/10.2.2/target/riscv/`（QingKe/XW/HPE TCG）
5. 按文件名排序应用 `patches/qemu/*.patch`（Kconfig/meson、`csr.c` 等）
6. configure（仅 `riscv32-softmmu`）→ make → make install

---

## 3. 能力边界（必读）

### 3.1 通用 `virt`（上游机器）

标准 QEMU `virt` 机器与 CH32 不兼容：

- **不能**把按片内 Flash 链接的 EVT `.elf` 直接丢进 `-M virt` 跑通（DRAM 基址 `0x80000000`、中断控制器与 CH32 完全不一致）。
- **QingKe 专有扩展**在 `virt` 上**不会**按真机语义建模（无 XW 译码、无 HPE、无 PFIC）。

| 能力 | `virt` 说明 |
|------|-------------|
| **串口** | `-M virt -nographic -serial mon:stdio` → 16550A（非 CH32 USART 地址） |
| **装载** | `loader`/`kernel` 装入 DRAM（`0x80000000` 起） |
| **以太网** | `virtio-net` + `-netdev user`；非片内 ETH MAC |

`virt` 适合另做链接脚本的算法 / 协议栈实验，见 §8.2。

### 3.2 本仓库 CH32 整机（`-M ch32v317` 等）

整机模型含 ESIG / DBGMCU / GPIO+AFIO+EXTI / IWDG / DMA / CRC / USBHS 占位、Flash 控制器、USBFS 占位 RAM、SysTick（STK）等。

| 能力 | 说明 |
|------|------|
| **内存图** | Flash 仿真 RAM @ `0x08000000`；SRAM @ `0x20000000`；系统存储器（Bootloader ROM）@ `0x1FFF8000`（28KiB，全 0xFF 占位）；默认大小随 `-M` 变化（见 §4），可用 `-m <size>` 覆盖 SRAM、`-machine ...,flash-size=<bytes>` 覆盖 Flash（4KiB～16MiB） |
| **启动模式（`boot-mode`）** | 根据手册表 1-1：`flash`（默认，BOOT0=0）0x00000000 → Flash；`sysmem`（BOOT0=1/BOOT1=0）0x00000000 → 系统存储器；`sram`（BOOT0=1/BOOT1=1）CPU 从 SRAM 启动 |
| **固件装载** | `-kernel <elf>`：`load_elf_ram_sym` 装载并设 `resetvec`；`-machine ...,flash-image=<bin|hex>`：从 `0x08000000` 顺序写入（`.bin`）或解析 Intel HEX（`.hex`/.HEX`），首条指令为 jal 跳板时自动跟入口 |
| **USART1** | MMIO @ `0x40013800`（STATR/DATAR 布局与 `ch32v30x.h` 一致），对接 `-serial` |
| **ACLINT（仅 QEMU）** | 非片内 PFIC。于 `0x02000000` 挂 RISC-V ACLINT（MSWI+MTIMER），便于 `ch32v317-qingke-ext.elf` HPE 自检；**勿当作 CH32 真机外设地址** |
| **以太网 MAC** | 片内 EMAC+DMA + CH182 PHY 仿真（`ch32-eth-dwmac.c`）；`-nic tap,...` 接宿主机 tap，支持 ARP/ICMP/TCP/UDP 全双工；TX/RX COE；ETH 中断 IRQn=77（见 §9） |
| **外设黑洞** | `0x40000000` 起 2MiB 未实现访问由 `unimp` 吞掉，避免轻触外设即 fault |
| **CPU `-cpu wch-qingke-v4f`（默认）** | RV32IMAFC+C；`xw=on` `hpe=on`；TCG 译码 XW 压缩访存；HPE 快照 16 个整型 caller GPR，`mret` 恢复 |
| **CPU `-cpu wch-qingke-v4a`** | RV32IMAC，无 XW，仍 HPE |
| **仍非完整 SoC** | 无 PFIC / SysTick / VTF / WFE / 片内 Flash 控制器 / DMA；USB 等仍为占位 |

---

## 4. 支持的机器（`-M`）

实现位于 `qemu-overlay/<版本>/hw/riscv/ch32-v.c`（板级入口）及 `ch32-*.c` / `ch32-machine-internal.h`（外设模块）。

| `-M` 名称 | 默认 Flash（`0x08000000`） | 默认 SRAM（`-m`，`0x20000000`） | 默认 `-cpu` |
|-----------|---------------------------|----------------------------------|-------------|
| `ch32v317` | 480 KiB | 192 KiB | `wch-qingke-v4f` |
| `ch32v307` | 256 KiB | 64 KiB | `wch-qingke-v4f` |
| `ch32v305` | 288 KiB | 64 KiB | `wch-qingke-v4f` |
| `ch32v303` | 256 KiB | 64 KiB | `wch-qingke-v4f` |
| `ch32v203` | 256 KiB | 64 KiB | `wch-qingke-v4b` |
| `ch32v203rb` | 128 KiB | 64 KiB | `wch-qingke-v4b` |
| `ch32v103` | 64 KiB | 20 KiB | `wch-qingke-v3a` |
| `ch32v003` | 16 KiB | 2 KiB | `wch-qingke-v2c`（占位）⚠ |
| `ch32v407` | 992 KiB | 200 KiB | `wch-qingke-v3v` | 默认值已对齐 CH32V407DS0 V1.0：`ch32v407.flash=0x08000000–0x080F7FFF`（992 KiB，= 480 KB 非零等待 + 512 KB 零等待）、`ch32v407.sram=0x20000000–0x20031FFF`（200 KiB）；200 MHz / FSMC / LTDC / ARGB / 双 USBHS / I3C / V 向量子集等仍未建模 ⚠ |
| `ch32h417` | 896 KiB ⚠ | 192 KiB ⚠ | `wch-qingke-v5f`（单核占位）⚠ |

**注意**：具体封装料号的 Flash/SRAM 可能与上表有出入，以**数据手册**为准；`-m` 可覆盖 SRAM 大小，但须与固件链接脚本一致。查看完整说明：

> ⚠ **`ch32v003` CPU 占位说明**：真机 CH32V003 为 **QingKe V2A**（RV32EC+XW）；QEMU 当前用 `wch-qingke-v2c`（RV32EC+Zmmul+XW）占位。
>
> **`ch32v407` 参数修正（CH32V407DS0 V1.0）**：
> - **内核**：`wch-qingke-v3v`（真机青稞 **RISC-V3V**，**RV32IMABCV-X**，含 **B** 与 **向量子集 `Zve64x+Zvbb`**）。Zve64x/Zvbb **QEMU 尚未在 TCG 中实现**，RVV 指令会触发非法指令异常；IMAC + B + XW + mcpy 路径可用。
> - **Flash**：**992 KiB**（`0x0800_0000 – 0x080F_7FFF`，= 480 KB 非零等待 + 512 KB 零等待；另可将 200 KiB SRAM 中 64 KiB 重划为零等待 Flash）。
> - **SRAM**：**200 KiB 零等待**（`0x2000_0000 – 0x2003_1FFF`）。
> - **SYSCLK**：真机最高 **200 MHz**（QEMU 不建模时钟树）。
> - **仍未建模**：FSMC（Bank1/2）、LTDC、ARGB、I3C、DVP、SDIO、RNG、OPA、双 USBHS、V467 的片内 **PSRAM 4/8 MB**（窗口 `0x8000_0000 – 0x8080_0000`）、V 向量子集。
>
> ⚠ **`ch32h417` 仍为占位实现，与真机规格存在较大差异，尚待完善。**
>

```bash
qemu-system-riscv32 -M help
```

---

## 5. CPU 类型（`-cpu`）与工具链 `-mcpu`

QEMU `-cpu` 名称与 GCC `-mcpu=wch-qingke-*`。

| 场景 | QEMU `-cpu` | 工具链关系 | 特权模式 |
|------|-------------|-----------|----------|
| CH32V30x / V4F 料号，含 XW 指令 | `wch-qingke-v4f`（多板默认） | `-mcpu=wch-qingke-v4f`，`imafcxw` | **M + U** |
| 仅需 IMAC、不要 XW | `wch-qingke-v4a` | `-mcpu=wch-qingke-v4a` | **M + U** |
| CH32V203 等 V4B 线 | `wch-qingke-v4b`（`ch32v203` 默认） | RV32IMAC，无浮点，无 PMP；亦可选 `v4c` / `v4j` | **M + U** |
| V4C / V4J | `wch-qingke-v4c` / `wch-qingke-v4j` | RV32IMAC，无浮点，含 PMP（区域数=4）；V4J 另含 I-Cache（QEMU 不仿真） | **M + U** |
| CH32V103 等 V3A | `wch-qingke-v3a`（`ch32v103` 默认） | `ch32v103` 仅允许白名单内 CPU | **M + U** |
| V3B / V3C / V3F / V3V | `wch-qingke-v3b` / `v3c` / `v3f` / `v3v` | V3B/V3C 无 PMP；V3C/F/V 含扩展 B；V3F 含浮点；V3V 含 mcpy，向量子集（Zve64x_zvbb）QEMU 暂不建模 | **M + U** |
| CH32V003 等 V2 | `wch-qingke-v2c`（`ch32v003` 默认，占位） | 真机 CH32V003 为 **V2A**（RV32EC+XW）；QEMU 当前用 V2C（多 Zmmul）占位；**RV32E**，固件须按 16 GPR 链接 | **仅 M** |
| CH32H417 等 V5 | `wch-qingke-v5f`（`ch32h417` 默认） | 真机 CH32H417 为双核（V5F+V3F）；QEMU 当前单核 V5F 占位；含 B+Zba/Zbb/Zbc/Zbs 等 | **M + U** |
| CH32V407 / V467 | `wch-qingke-v3v`（`ch32v407` **默认**） | 真机 **青稞 RISC-V3V**（`RV32IMABCV-X`，含 **V 向量子集 `Zve64x+Zvbb`** 与 **B**）；工具链 `-mcpu=wch-qingke-v3v`；QEMU TCG 支持 IMAC + **B** + XW + mcpy 路径，**尚未实现 Zve64x/Zvbb**（RVV 指令会触发非法指令异常） | **M + U** |

列出本机已注册的全部 CPU：

```bash
qemu-system-riscv32 -cpu help
```

### 5.1 各 `-M` 下允许的 `-cpu`（白名单）

- **`ch32v317` / `ch32v307` / `ch32v305` / `ch32v303` / `ch32v203` / `ch32v407` / `ch32h417`**：允许本仓库注册的**全部 QingKe 类型**及通用 `rv32`（`ch32_cpus_all`）。注意：`rv32`（`TYPE_RISCV_CPU_BASE32`）已恢复为正确的标准 32 位通用基类（继承 `DYNAMIC_CPU`，有 MMU/SV32），与 `spike`/`virt` 等标准机器共存无冲突。
- **`ch32v103`**：仅 `wch-qingke-v3a`、`wch-qingke-v2a`、`rv32`（避免误选带浮点或 V4 核跑 V3 片）。
- **`ch32v003`**：`wch-qingke-v2c`、`wch-qingke-v2a`、`rv32e`、`rv32`。

若出现 `Invalid CPU model`，多为 `-cpu` 不在该 `-M` 的白名单内。

---

## 6. 指令级实现与处理器手册对照

下表描述**本仓库 QEMU TCG 已实现的 CPU 能力**（实现位置：`qemu-overlay/<版本>/target/riscv/cpu.c` 的 `DEFINE_RISCV_CPU(... wch-qingke-*)` 以及 `insn_trans/trans_rvwch.c.inc`），用于判断「固件能否在 `-cpu wch-qingke-*` 上指令级执行」，**不**表示 SoC 外设已完整建模（见 §13）。

| 项目 | 与手册关系 | 本仓库 QEMU 行为（摘要） |
|------|-----------|--------------------------|
| **已注册的 `-cpu` 名** | 与 GCC `-mcpu=wch-qingke-*` 营销名一致 | 见 `hw/riscv/ch32-v.c` 中 `ch32_cpus_*` 白名单；含 `wch-qingke-v5f`～`v2a` 与通用 `rv32` 等；**V3B/V3C/V3F/V3V 已全部添加** |
| **标准 RISC-V 字母集** | I/M/A/C/F 等 | 见各 CPU 的 `misa_ext`：V4F：I+M+A+F+C+**U**；V4B/V4C/V4J：I+M+A+C+**U**（无浮点）；V3A：I+M+A+C+**U**；**V3B：I+(M)+C+(B)+**U（手册 RV32I[M]C[B]，**无 A**，M/B 可选）；**V3C：I+M+C+B+**U（手册 RV32IMCB，**无 A**）；V3F：I+M+A+F+C+B+**U**；V3V：I+M+A+C+B+**U**（另含向量子集 Zve64x_zvbb，QEMU 未建模）；V2A/V2C：E+C（仅 M 模式，无 U）；V5F：另含 RVB 与 `zba`/`zbb`/`zbc`/`zbs` 和 **U** |
| **特权模式（`RVU`）** | 手册 §1.3 | V3/V4/V5 系列均含 `RVU`，支持 M + U 两种特权模式；`mret` 时根据 `mstatus.MPP` 选择返回 M 或 U 模式；U 模式访问 M-CSR 触发非法指令异常（mcause=2）；V2 无 `RVU`（仅 M 模式） |
| **`zicsr` / `zifencei`** | CSR、`fence.i` | 各 QingKe 型号均 `cfg.ext_zicsr` / `ext_zifencei = true` |
| **XW（扩展压缩访存）** | 《MCU 说明》 | `cfg.ext_xw` 为真时，`trans_rvwch.c.inc` 译码 XW 16 位指令（`c.lbu`/`c.lhu`/`c.sb`/`c.sh` 及 SP 相对等），与工具链 `-M xw` 码点一致 |
| **mcpy（`xwchmcpy` / opcode `0x0F` func3=7）** | 《MCU 说明》 | **已在 TCG 实现**（`decode_wch_mcpy32`）：仅当 `cfg.ext_xwchmcpy=true`（V3B/V3C/V3F/V3V）时激活；**V4F/V4B/V4C/V4J/V3A 等无此扩展**，客机执行时触发 fault（CH32V317 真机实测 mcause=7） |
| **HPE（硬件压栈）** | 手册快速中断，与 `mret` 配合 | `cfg.ext_hpe` 为真；`wch-qingke.h` + `cpu_helper.c` 简化实现入栈/出栈与异步中断路径，**不**保证与芯片周期级一致 |
| **VTF / PFIC / 向量** | 免表通道、PFIC 等 | 非标准 ISA 部分；见 CH32 机器 PFIC/STK 桩与 §13 |
| **Zve / 向量（如 V3V）** | 手册 V3V 行 | **未**在 `wch-qingke-v5f` 等中实现；**不要**期待运行含 Zve 的 `v3v` 固件 |
| **A 扩展** | 部分型号对 lr/sc 有简化描述 | QEMU 为常规 RISC-V A 扩展语义；并发迁移时务须真机验证 |

**自检建议**：用 `riscv32-wch-elf-objdump -d` 反汇编目标 `.elf`，若出现 `mcpy` / `wchqk.mcpy` 等，需确认目标 CPU 是否为 V3B/V3C/V3F/V3V（含 `xwchmcpy` 扩展），否则 QEMU 上执行会触发 fault。

---

## 7. 固件要求

1. **链接布局**：代码与只读数据落在 Flash 映射 `0x08000000`；RAM 段在 `0x20000000`（与 EVT / WCH 常见 CH32 工程一致）。
2. **入口**：ELF 入口地址应在 Flash 区域内；QEMU 用 `load_elf_ram_sym` 装载后，将 `resetvec` 设为 ELF 解析到的入口。
3. **ABI 与寄存器宽度**：
   - **V2A / V2C**（如 `ch32v003`）：使用 `ilp32e` 与 RV32E 工具链配置；不要用 RV32I 固件跑在 `wch-qingke-v2a` 上。
   - **V4F** 等：通常为 `ilp32f`（含硬浮点）。
4. **串口自检**：最小示例通过写 **USART1 DATAR（偏移 `0x4`）** 输出字符，基址 `0x40013800`。

> **不要**把按上述布局链接的 ELF 直接用于 `-M virt`（DRAM 基址与外设完全不同）。

---

## 8. 快速运行

### 8.1 CH32V317 整机（推荐用于片内布局固件）

```bash
export PATH="$PWD/dist/qemu-10.2.2-riscv32/bin:$PATH"
make -C qemu-test-firmware/ch32v317-minimal
./scripts/run-qemu-ch32v317.sh
# 或指定 ELF：
./scripts/run-qemu-ch32v317.sh /path/to/your-app.elf
```

等价手动命令：

```bash
qemu-system-riscv32 \
  -M ch32v317 \
  -cpu wch-qingke-v4f \
  -m 192k \
  -monitor none \
  -display none \
  -serial stdio \
  -kernel qemu-test-firmware/ch32v317-minimal/ch32v317-minimal.elf
```

**说明**：

| 项 | 说明 |
|----|------|
| **固件装载** | `-kernel <elf>`：装入 Flash 并设 `resetvec`（通常为 ELF 入口，如 `0x08001000`）<br>`-machine ch32v317,flash-image=<file.bin|file.hex>`：裸镜像或 Intel HEX 写入 Flash（首条指令为 `jal` 跳板时自动跟入口）<br>两者同时给出时优先 `-kernel` 并告警 |
| **启动模式** | `-machine ...,boot-mode=<flash\|sysmem\|sram>`：选择启动模式（见 §1.3 与 §11）；默认 `flash`，无需显式指定 |
- **串口**：`-serial stdio` 将 USART1 接到当前终端（推荐自动化用，避免 `-nographic -serial mon:stdio` 在管道下的 monitor 复用问题）；退出 QEMU 用 `Ctrl+A` 后按 `X`
- **宿主串口对接**：可设置 `QEMU_SERIAL=/dev/ttyCH343USB` 等替代 `stdio`，便于与 EVT 对拍

### 8.1b CH32V203 整机

```bash
export PATH="$PWD/dist/qemu-10.2.2-riscv32/bin:$PATH"
make -C qemu-test-firmware/ch32v203-minimal
qemu-system-riscv32 \
  -M ch32v203 \
  -cpu wch-qingke-v4b \
  -display none \
  -serial stdio \
  -kernel qemu-test-firmware/ch32v203-minimal/ch32v203-minimal.elf
# 期望输出：CH32V203 QEMU minimal firmware OK
```

**CH32V203 与 CH32V317 的主要差异**：

- CPU 为 QingKe V4B（`rv32imafc`，无 XW 扩展），使用 `-march=rv32imafc -mabi=ilp32f`
- Flash 256 KiB / SRAM 64 KiB（默认）
- 无以太网（`-nic` 选项无效）
- UART4~8 为与 USART2/3 同级的轻量 MMIO（手册表 18-2 ~ 18-9 基址；DMA2 RX 映射见 `ch32-usart.c`）；**`-serial` 仍仅绑定 USART1**
- CHIP_ID `0x20310500`（C8T6），地址 `0x1FFFF704`

### 8.1c CH32V203RBT6 整机（含 ETH10M）

CH32V203RBT6 是 CH32V20x_D8 封装，内置 ENC28J60 兼容的 10M 以太网控制器（ETH10M，`0x40028000`，IRQ 61）。

```bash
export PATH="$PWD/dist/qemu-10.2.2-riscv32/bin:$PATH"
# 构建测试固件
make -C qemu-test-firmware/ch32v203rb-eth

# 运行（user-mode 网络，无需 root）
qemu-system-riscv32 \
  -M ch32v203rb \
  -display none \
  -serial stdio \
  -nic user,model=ch32-eth10m \
  -kernel qemu-test-firmware/ch32v203rb-eth/ch32v203rb-eth.elf
# 期望输出：CH32V203RB ETH10M OK

# 使用 tap 后端（需 root 配置，参见 §9）
qemu-system-riscv32 \
  -M ch32v203rb \
  -display none \
  -serial stdio \
  -nic tap,ifname=tap0,model=ch32-eth10m \
  -kernel qemu-test-firmware/ch32v203rb-eth/ch32v203rb-eth.elf
```

**ETH10M 仿真特性**：

| 特性 | 说明 |
|------|------|
| 控制器型号 | ENC28J60 兼容 MAC，基址 `0x40028000`，寄存器 `0x30` 字节 |
| IRQ | 61（PFIC word=1, bit=29） |
| TX | 写 ETXST/ETXLN → 置 ECON1.TXRTS 触发发送 |
| RX | ERXST/ERXLN 指向 SRAM 物理地址；仿真自动投递收包 |
| PHY | 仿真 BMSR/ANLPAR，支持 `ETH_LinkUpCfg()` 流程 |
| LINKIF | 复位后约 200 ms 延迟触发（使固件先以 link-down 初始化） |
| 中断 | EIE.INTIE \| (EIE & EIR) → IRQ 61 路由至 PFIC |

### 8.1d CH32V307 整机（含内置 10M PHY）

CH32V307 内置 10M 以太网 MAC+PHY（`phy-model="phy10m"`），使用与 CH32V317 相同的 `ch32-eth-dwmac.c` 仿真，但 PHY 参数不同（ANLPAR=0、无 CH182）。若需仿真 RGMII 接 RTL8211F 千兆 PHY，可显式切换到 `phy-model="rtl8211f"`（见 §9.4）。

**标准启动（flash-image 或 -kernel，默认 boot-mode=flash）**：

```bash
# 使用 QEMU ELF（入口 0x08000000，推荐）
qemu-system-riscv32 \
  -M ch32v307 \
  -display none \
  -serial stdio \
  -nic "tap,ifname=tap0,script=no,downscript=no" \
  -kernel OrayOS-Tiny-Bin/WebServer-qemu.elf

# 使用裸 Flash 镜像
qemu-system-riscv32 \
  -M ch32v307,flash-image=WebServer.bin \
  -display none \
  -serial file:/tmp/webserver.log \
  -nic "tap,ifname=tap0,script=no,downscript=no"

# 使用 Intel HEX（自动识别 .hex 后缀，与 .bin 等效）
qemu-system-riscv32 \
  -M ch32v307,flash-image=WebServer.hex \
  -display none \
  -serial file:/tmp/webserver.log \
  -nic "tap,ifname=tap0,script=no,downscript=no"
```

预期串口输出（EVT WebServer 固件）：

```
WCHNET_LibInit Success
PHY Link Success
TCP Connect Success
```

> **CH32V307 以太网特性**：默认使用内置 10M PHY（`phy-model="phy10m"`）；驱动（`eth_driver_10M.c`）通过 DMASR bit31（PHYLINK 中断）而非轮询 BSR 来触发 `ETH_PHYLink()`；ANLPAR 返回 0（无远端协商），Autoneg Complete 后强制半双工。如需模拟 RTL8211F 千兆 RGMII PHY，可显式使用 `phy-model="rtl8211f"`（见 §9.4.1）。

### 8.2 virt 机器（通用 RISC-V 测试）

固件须按 `virt` / DRAM 布局链接（入口在 `0x80000000` 段）：

```bash
./scripts/qemu-riscv32-virt-run.sh /path/to/app.elf

# 需要用户态网络 + virtio-net 时（依赖构建时检测到 libslirp）：
NET=1 ./scripts/qemu-riscv32-virt-run.sh /path/to/app.elf
```

上游 `virt` 文档：<https://www.qemu.org/docs/master/system/riscv/virt.html>

---

## 9. 网络仿真（以太网 / tap 后端）

**`-M ch32v317`**（以及 `ch32v307`、`ch32v305`）内置仿真 CH32 片内 EMAC+DMA 以太网控制器（`ch32-eth-dwmac.c`），支持通过宿主机 **tap** 接口与外部网络通信，使含 LwIP 等协议栈的 RTOS 固件可在 QEMU 上直接做 ping、TCP 等网络验证。

> **CH32V307 与 CH32V317 以太网差异**：CH32V307 默认使用内置 10M PHY（`phy-model="phy10m"`），对应 EVT `eth_driver_10M.c` 驱动；CH32V317 默认使用外置 CH182 PHY（100Mbps，`phy-model="ch182"`），对应 `eth_driver_CH32V317.c`。两者共用同一 `ch32-eth-dwmac.c` 仿真核心，通过 `phy-model` 字符串属性区分行为。另外可选的 **`phy-model="rtl8211f"`**（千兆 RGMII）供需要模拟 RTL8211F 时显式启用（见 §9.4）。

### 9.1 前置准备（一次性，需要 root）

```bash
# 创建 tap0 接口并分配给当前用户
sudo ip tuntap add tap0 mode tap user $(whoami)
sudo ip link set tap0 up

# 宿主机侧 tap0 分配与 Guest 同网段的 IP（宿主机=.1，Guest=.10）
sudo ip addr add 192.168.1.1/24 dev tap0

# 可选：开启 IP 转发 + NAT，让 Guest 访问外网
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 192.168.1.0/24 -j MASQUERADE
```

> **提示**：tap0 为 netlink 持久接口，系统重启后仍然存在，无需每次重建。若想开机自动配置，可写入 `/etc/rc.local` 或 systemd-networkd 配置。

### 9.2 启动带网络的 QEMU

使用专用脚本（推荐）：

```bash
# 串口输出到文件（后台运行时推荐）
QEMU_SERIAL=file:/tmp/orayos.log \
    ./scripts/run-qemu-orayos-tiny-tap.sh

# 串口输出到当前终端（前台调试时）
QEMU_SERIAL=stdio \
    ./scripts/run-qemu-orayos-tiny-tap.sh

# 指定自定义 ELF（默认使用 OrayOS-Tiny-Bin/OrayOS-Tiny-qemu.elf）
./scripts/run-qemu-orayos-tiny-tap.sh /path/to/your-app.elf
```

等价手动命令：

```bash
qemu-system-riscv32 \
  -M ch32v317 \
  -cpu wch-qingke-v4f \
  -m 192k \
  -monitor none \
  -display none \
  -serial file:/tmp/orayos.log \
  -nic "tap,ifname=tap0,script=no,downscript=no" \
  -kernel OrayOS-Tiny-Bin/OrayOS-Tiny-qemu.elf
```

```bash
qemu-system-riscv32 \
  -M ch32v317,flash-image=flash.bin \
  -cpu wch-qingke-v4f \
  -m 192k \
  -monitor none \
  -display none \
  -serial file:/tmp/orayos.log \
  -nic "tap,ifname=tap0,script=no,downscript=no"
```


**关键参数说明**：

| 参数 | 说明 |
|------|------|
| **`-nic tap,ifname=tap0,...`** | 将 QEMU 以太网后端连接到 `tap0`；`script=no,downscript=no` 禁用 QEMU 自带的 if-up/down 脚本 |
| **`-m 192k`** | OrayOS-Tiny 需要 192KiB SRAM（`ch32v317` 默认值，可省略） |
| **`-serial file:...`** | 将 USART1 输出重定向到文件，便于后台运行时查看日志（`tail -f /tmp/orayos.log`） |

### 9.3 验证网络连通性

**等待固件启动**（通常 3～8 秒），确认串口输出包含：

```
[ETH] Link: UP 100Mbps Full
[LwIP] main interface en0 Initialized - IP: 192.168.1.10
```

**ping 测试**（必须用 `-I tap0` 指定接口，避免走其他路由）：

```bash
# 清除可能过期的 ARP 缓存
ip neigh flush dev tap0

# ping 10 次，超时 2 秒
ping -I tap0 -c 10 -W 2 192.168.1.10
```

预期输出（正常）：

```
PING 192.168.1.10 (192.168.1.10) from 192.168.1.1 tap0: 56(84) bytes of data.
64 bytes from 192.168.1.10: icmp_seq=1 ttl=64 time=1.20 ms
...
10 packets transmitted, 10 received, 0% packet loss
rtt min/avg/max/mdev = 0.170/0.522/1.199/0.248 ms
```

**tcpdump 抓包验证**（可与 ping 同时运行）：

```bash
tcpdump -i tap0 -n -vv &
ping -I tap0 -c 5 192.168.1.10
```

正常流量应包含：
1. `ARP Request`（宿主机询问 192.168.1.10 的 MAC）
2. `ARP Reply`（Guest 用 `77:88:11:22:33:44` 回应）
3. `ICMP echo request` + `ICMP echo reply`（双向）

> **`ping` / `tcpdump` 权限**：大多数发行版已设 **setuid-root** 或 **cap_net_raw**，直接执行即可。若提示 `Operation not permitted`：
> ```bash
> sudo setcap cap_net_raw+ep /usr/bin/ping
> sudo setcap cap_net_raw+ep /usr/bin/tcpdump
> ```

### 9.4 以太网模型特性（`ch32-eth-dwmac.c`）

| 功能 | 实现状态 | 说明 |
|------|----------|------|
| **DMA 描述符链**（TX/RX ring） | 完整仿真 | 支持 `TDes0/1/2/3`、`RDes0/1/2/3`，与 EVT 驱动布局（`ETH_DMATxDesc`）一致 |
| **PHY（CH182 RMII）** | 仿真 | `BCR/BSR/PHYID1/PHYID2`、链路上报 100Mbps Full-Duplex，MII MDIO 读写；`phy-model="ch182"`（CH32V317 默认） |
| **PHY（10M 内置，ch32v307 默认）** | 仿真 | `BCR/BSR`（ANLPAR=0，无远端协商）；到期后自动注入 DMASR bit31（PHYLINK 中断）触发 `ETH_PHYLink()`；半双工 10M；`phy-model="phy10m"` |
| **PHY（RTL8211F RGMII千兆）** | 仿真（可选） | Clause22 基础页 + Page 0xd04 LCR/EEELCR + MMD 间接访问；PHYID=0x001CC916，BCR 复位 0x1140，PHYSR link-up 后 0x002C（1000M FD）；MACCR 需驱动按协商结果写 PS/FES/DM；通过 `phy-model="rtl8211f"` 显式启用 |
| **MACCR 速率/双工位** | 仿真 | PS (bit15)、FES (bit14)、DM (bit11) 可读写；提供 `ch32_eth_mac_link_speed_mbps()` / `ch32_eth_mac_full_duplex()` helper 供内部逻辑查询（当前仿真核心仍以 tap 透传为主，速率位主要用于 PHY 表示和未来扩展） |
| **TX 硬件校验和卸载**（COE） | 完整仿真 | `TDes0[23:22]` CIC 字段：`00`=不处理，`01`=IPv4头，`10`=IPv4+TCP/UDP，`11`=IPv4+TCP/UDP/ICMP |
| **RX 硬件校验和验证**（IPC） | 仿真 | MACCR.IPC=1 时 `RDes0` 不置 `IPHCE`/`PCE` 错误位 |
| **ETH 中断路由**（IRQn=77） | 完整仿真 | 通过 PFIC MEIP 路由，与 `ETH_IRQHandler` VTF 机制兼容 |
| **WDT 复位后恢复** | 已修复 | `ch32_eth_rx_resume` 恢复 tap `read_poll`，防止接收永久暂停 |
| **ARP/IP/TCP/UDP/ICMP** | 透传 | 帧由 LwIP 等协议栈处理，QEMU 不做过滤 |
| **多播/广播** | 透传 | 按 `MACCR` 寄存器标志处理，默认通过 |
| **USBHS 以太网（CDC-ECM）** | 不仿真 | 仅支持片内 RMII MAC，不仿真 USB-ETH 路径 |

#### 9.4.1 `phy-model` 属性

`ch32-eth-dwmac` 设备开放 `phy-model` 字符串属性：

| 取值 | 含义 | 默认适用板子 |
|------|------|------|
| `"ch182"` | 外置 100M CH182 PHY（PHYID1=0x7371，PHYID2=0x9011，BCR=0x3100） | `ch32v317`（默认）、`ch32v305`、`ch32v303` |
| `"phy10m"` | CH32V307 内置 10M PHY（PHYID=0，ANLPAR=0，BCR=0x1000） | `ch32v307`（默认） |
| `"rtl8211f"` | Realtek RTL8211F 千兆 RGMII PHY（PHYID=0x001CC916，BCR=0x1140，PHYSR 1000M FD） | 无板子默认，按需显式启用 |
| `NULL`（未设置） | 等价 `ch182`（默认行为） | 老板子未设 `phy-model` 时的兜底 |

> 旧 `phy-10m` 布尔属性已在 2026.05.03 一并移除；板级 `default_nic="ch32-eth-dwmac-10m"` 会自动向设备写入 `phy-model="phy10m"`，CH32V307 默认机器无需固件侧改动。

显式启用 RTL8211F 的三种方法：

```bash
# 方法 1：QOM 属性直设（需 QEMU monitor qom-set，适用限制较多，一般不用）

# 方法 2：machine 属性通路（如果板级暴露）

# 方法 3（推荐）：临时切换板级 default_nic，自定义本地调试机器
# 编辑 qemu-overlay/10.2.2/hw/riscv/ch32-boards.c，将需要的板子定义改为
#   .default_nic = "ch32-eth-dwmac-rtl8211f",
# 重新构建后，machine_init 会自动设置 phy-model="rtl8211f"。
```

> RTL8211F 模型当前主要面向 **MDIO 寄存器级验证**（确认驱动能否识别 PHYID=0x001CC916、读到 1000M FD 状态、正确备份 Page 0xd04 LED/EEE 寄存器等）；tap 数据平面仍是 Gigabit-less 仿真，MACCR.PS/FES/DM 不影响 tap 透传速率。

### 9.5 网络故障排查

| 现象 | 原因 | 处理方式 |
|------|------|---------|
| `could not configure /dev/net/tun: Device or resource busy` | tap0 已被其他 QEMU 进程持有 | `pgrep -a qemu-system-riscv32` 找到并 `kill` 旧进程后重启 |
| `could not open /dev/net/tun: No such file or directory` | tap0 未创建或内核无 TUN 支持 | 执行 §9.1 前置准备；确认 `ls /dev/net/tun` 存在 |
| ARP FAILED / Destination Host Unreachable | tap0 未配置宿主机 IP；或 QEMU 未正常启动；或 ARP 缓存旧条目 | 检查 `ip addr show tap0`；检查 QEMU 串口日志；`ip neigh flush dev tap0` |
| ping RTT >50ms 或间歇丢包 | QEMU 负载高、宿主机调度延迟 | 正常现象；嵌入式仿真不保证实时性 |
| `0 packets received`（无 ARP 应答） | 旧 QEMU 进程（无 `ch32_eth_rx_resume` 修复）tap 处于暂停状态 | 使用本仓库 `build/qemu-10.2.2-obj/qemu-system-riscv32`（已含修复） |

---

## 10. USB 仿真

CH32V317（及 CH32V307、CH32V305）的 QEMU 整机模型包含 **USBHS 高速主机** 与 **USBFS 全速 OTG** 两套 USB 外设的 MMIO 寄存器层；其中 **USBHS 主机模式**已与 QEMU 通用 USB 栈对接，可真实枚举并通信下游设备。

### 10.1 MMIO 地址与覆盖范围

| 外设 | 基址 | 大小 | 类型 | 说明 |
|------|------|------|------|------|
| **USBHS**（高速，含主机/设备寄存器） | `0x40023400` | 1KiB | I/O | `ch32-usb.c`（MMIO）+ `ch32-usb-hs-host.c`（Host 桥接层，IRQ 85） |
| **USBFS**（全速 OTG，含 FIFO RAM） | `0x50000000` | 32KiB | RAM + I/O | `ch32-usb.c`（MMIO）+ `ch32-usb-fs-host.c`（Host 桥接层，IRQ 83） |

### 10.2 USBHS 主机模式（真实枚举）

USBHS 寄存器块（`0x40023400`）的**主机控制字段**已与 QEMU 通用 USB 栈（`USBBus` + 根端口）对接，支持：

- **设备连接/断开检测**：根端口 attach/detach 回调自动置 `UIF_DETECT` 并上报中断
- **总线复位**：写 `HOST_CTRL.UH_TX_BUS_RESET` 触发 `usb_device_reset()`
- **事务提交**：写 `HOST_EP_PID`（偏移 224）触发 SETUP / IN / OUT 事务，通过 `usb_handle_packet()` 路由到下游设备
- **DMA 传输**：`HOST_RX_DMA` / `HOST_TX_DMA` 指向 Guest SRAM 的 DMA 缓冲区（须在 `0x20000000` 起的 SRAM 范围内，最大 4KiB/包）；IN 数据写回 Guest 内存，OUT/SETUP 数据从 Guest 内存读取
- **完成中断**：事务完成后置 `INT_FG.UIF_TRANSFER`，经 PFIC MEIP 路由（IRQn=85）送入 `USBHS_IRQHandler`，兼容 VTF 路径
- **异步完成**：若 `usb_handle_packet()` 返回 `USB_RET_ASYNC`，由根端口 `complete` 回调收尾，结果与同步路径一致

**非主机字段**（设备模式、OTG 切换等）仍为 MMIO 寄存器桩（读写往返，不触发传输）。

### 10.3 USBFS（全速 Host + OTG 桩）

USBFS 寄存器块（`0x50000000`，32KiB）包含**主机模式桥接**与**设备/OTG 寄存器桩**：

- **主机模式**（`ch32-usb-fs-host.c`，IRQ 83）：与 USBHS 结构完全对称，支持 Low/Full 速设备枚举与事务提交；`UC_HOST_MODE`（bit7）置 1 时激活主机桥接路径
- **寄存器桩**：`UC_CLR_ALL` / `UC_RESET_SIE` 清零 `INT_FG` / `INT_ST`；`INT_FG` 写 1 清位（W1C）
- **`MIS_SIE_FREE` / `SIE_FREE` 状态位**：读操作动态 OR 返回（始终为 1），使固件初始化时的 SIE 空闲检查可以通过而不阻塞
- **设备模式 / OTG 传输**：不仿真；固件若依赖 USBFS 设备枚举、CDC-ACM 等，在 QEMU 上会停在等待连接状态

### 10.4 向 QEMU 挂接 USB 设备

USBHS 根端口默认**空闲**（不再硬编码默认设备），用户通过 `-device` 按需挂载。设备直连根端口，CherryUSB 主机栈可直接枚举（无需 Hub Class 驱动）：

```bash
# 挂接 QEMU 仿真的 USB 键盘
qemu-system-riscv32 -M ch32v317 -nographic \
  -kernel OrayOS-Tiny-Bin/OrayOS-Tiny-qemu.elf \
  -device usb-kbd

# 透传宿主机 USB 设备（如 CH341 USB 转串口）
# 注意：需要宿主机 /dev/bus/usb 读写权限（sudo chmod 0666 /dev/bus/usb/XXX/YYY）
qemu-system-riscv32 -M ch32v317 -nographic \
  -kernel OrayOS-Tiny-Bin/OrayOS-Tiny-qemu.elf \
  -device usb-host,vendorid=0x1a86,productid=0x7523

# 挂接 QEMU 仿真的 USB 大容量存储（需宿主机上的镜像文件）
qemu-system-riscv32 -M ch32v317 -nographic \
  -kernel your-usb-msc-app.elf \
  -drive id=usbdisk,file=disk.img,if=none,format=raw \
  -device usb-storage,drive=usbdisk
```

**已验证的枚举结果**（OrayOS-Tiny CherryUSB 主机栈）：

| 设备 | VID:PID | 枚举结果 | 备注 |
|------|---------|----------|------|
| `usb-kbd` | 0627:0001 | Enumeration success | CherryUSB 无 HID 类驱动，报 "Do not support Class:0x03" |
| `usb-host` (CH341) | 1a86:7523 | Enumeration success | CherryUSB 无 Vendor Specific 类驱动，报 "Do not support Class:0xff" |

> **注意**：固件须配置 USBHS 为主机模式（`UC_HOST_MODE=1`）且在 PFIC 中使能 IRQn=85，才能通过上述路径枚举下游设备。`-device` 不带 `bus=` 参数时默认挂到 USBHS 总线；若需指定 USBFS 总线可用 `bus=usb-bus.0`。USBFS 设备模式（如 CDC-ACM、HID 从机）当前不仿真。

### 10.5 USB 仿真特性与限制

| 功能 | 实现状态 | 说明 |
|------|----------|------|
| **USBHS 主机：设备检测** | 完整仿真 | attach/detach 事件 → `INT_FG.UIF_DETECT`，IRQn=85 |
| **USBHS 主机：SETUP/IN/OUT 事务** | 完整仿真 | `HOST_EP_PID` 写触发；DMA 缓冲区直接访问 Guest SRAM |
| **USBHS 主机：异步完成** | 完整仿真 | `USB_RET_ASYNC` 路径由根端口 `complete` 回调收尾 |
| **USBHS 主机：总线复位** | 仿真 | `HOST_CTRL.UH_TX_BUS_RESET` → `usb_device_reset()` |
| **USBHS 主机：usb-host 透传** | 已验证 | 宿主机 CH341（1a86:7523）透传到 Guest，CherryUSB 枚举成功 |
| **USBHS 中断（IRQn=85）** | 完整仿真 | 经 PFIC MEIP 路由，与 `USBHS_IRQHandler` VTF 机制兼容 |
| **USBHS 设备模式（USBHSD）** | 寄存器桩 | 读写往返；不触发 QEMU USB 设备侧协议 |
| **USBFS 主机模式（IRQn=83）** | 完整仿真 | 与 USBHS 结构对称，支持 Low/Full 速设备枚举 |
| **USBFS 设备模式 / OTG** | 寄存器桩 | `SIE_FREE` 恒 1，W1C 中断标志可用；不仿真实际传输 |
| **USBHS 以太网（CDC-ECM over USB）** | 不仿真 | 见 §9；以太网仿真走片内 RMII MAC，不走 USB 路径 |

---

## 11. 常用命令行参数

| 参数 | 说明 |
|------|------|
| **`-M <name>`** | 选择 §4 中的机器 |
| **`-cpu <model>`** | 选择 QingKe 或通用 RV32 CPU；缺省为该机型的默认 CPU |
| **`-m <size>`** | SRAM 大小（如 `64k`、`192k`）；须与固件链接的 RAM 长度匹配 |
| **`-kernel <elf>`** | 将 ELF 装入 Flash；与 `flash-image` 二选一（同时存在时优先本项） |
| **`-machine ...,boot-mode=<mode>`** | 启动模式（见 §3.2）：`flash`（默认，BOOT0=0，0x00000000→Flash）、`sysmem`（BOOT0=1/BOOT1=0，0x00000000→系统存储器 0x1FFF8000）、`sram`（BOOT0=1/BOOT1=1，CPU 从 0x20000000 启动）；未指定时默认 `flash` |
| **`-machine ...,flash-image=<bin\|hex>`** | 裸镜像整段装入 Flash（长度不得超过该机型默认 Flash 大小或 `flash-size` 设置值）；`.hex`/`.HEX` 后缀自动解析 Intel HEX 格式 |
| **`-machine ...,flash-size=<size>`** | 覆盖片内 Flash RAM 容量（`qemu_strtosz` 语法，如 `480k`、`0x78000`；范围 4KiB～16MiB） |
| **`-nic tap,...`** | tap 网络后端，见 §9 |
| **`-monitor none -display none`** | 关闭 QEMU 监视器与图形窗口（推荐自动化或后台运行） |
| **`-serial stdio`** | USART1 接到标准输入输出（推荐自动化验证） |
| **`-serial mon:stdio`** | 串口与 monitor 复用 stdin/stdout（手工调试，退出用 `Ctrl+A X`） |
| **`-serial file:<path>`** | 串口输出重定向到文件（后台运行推荐） |
| **`-s`** | 开启 GDB stub，监听 `tcp::1234` |
| **`-S`** | 启动后立即暂停，等待 GDB 连接后再运行 |
| **`-d`** / **`-D`** | 开启 QEMU 调试日志（进阶，见上游文档） |

---

## 12. GDB 调试

QEMU 内置 GDB stub（`-s`），可与本仓库 **`riscv32-wch-elf-gdb`** 配合，在不修改固件的前提下进行指令级调试：打断点、单步、读写寄存器与内存、查看符号等。

### 12.1 启动 QEMU GDB 模式

**典型启动命令**（第一个终端）：

```bash
# 方式一：-S 暂停，等待 GDB 连接后再运行
qemu-system-riscv32 \
  -M ch32v317 -cpu wch-qingke-v4f -m 192k \
  -monitor none -display none \
  -serial file:/tmp/qemu-serial.log \
  -kernel your-app.elf \
  -s -S

# 方式二：不暂停，附加调试正在运行的固件
qemu-system-riscv32 \
  -M ch32v317 -cpu wch-qingke-v4f -m 192k \
  -monitor none -display none \
  -serial stdio \
  -kernel your-app.elf \
  -s

# 方式三：带 tap 网络 + GDB stub（调试 LwIP / 网络协议栈固件）
qemu-system-riscv32 \
  -M ch32v317 -cpu wch-qingke-v4f -m 192k \
  -monitor none -display none \
  -serial file:/tmp/qemu-serial.log \
  -nic "tap,ifname=tap0,script=no,downscript=no" \
  -kernel your-app.elf \
  -s -S
```

> 使用 `flash-image=` 裸二进制启动时无法加载符号，建议调试时优先使用 `-kernel <elf>`；如需 `flash-image`，在 GDB 侧另行 `add-symbol-file` 加载（见 §12.3 场景四）。

### 12.2 连接 GDB

在第二个终端中使用本仓库工具链的 GDB（`riscv32-wch-elf-gdb`）：

```bash
export PATH="$PWD/dist/toolchain-wch-qingke/bin:$PATH"

# 交互调试
riscv32-wch-elf-gdb your-app.elf
(gdb) set remotetimeout 60
(gdb) target remote :1234
(gdb) info registers         # 查看所有寄存器
(gdb) backtrace              # 查看调用栈
(gdb) break main             # 下断点（需 ELF 有调试符号）
(gdb) continue
(gdb) step                   # 单步（源码级）
(gdb) stepi                  # 单步（指令级）
(gdb) finish                 # 运行到函数返回
(gdb) info symbol 0x08001234 # 查询地址对应符号
(gdb) x/10xw 0x20000000      # 以十六进制查看内存
(gdb) p some_variable        # 打印变量值
(gdb) quit

# 批处理脚本（CI / 自动化验证）
riscv32-wch-elf-gdb your-app.elf -batch -nx \
  -ex 'set remotetimeout 120' \
  -ex 'target remote :1234' \
  -ex 'break main' \
  -ex 'continue' \
  -ex 'printf "PC=0x%lx\n", $pc' \
  -ex 'quit'
```

> **GDB stub 端口冲突**：同时运行多个 QEMU 实例时，用 `-gdb tcp::3333` 等指定不同端口，GDB 侧对应 `target remote :3333`。

### 12.3 常用调试场景

#### 场景一：调试固件启动崩溃

```bash
# 终端1：启动 QEMU，暂停
qemu-system-riscv32 -M ch32v317 -cpu wch-qingke-v4f -m 192k \
  -monitor none -display none -serial stdio \
  -kernel your-app.elf -s -S

# 终端2：连接并单步
riscv32-wch-elf-gdb your-app.elf
(gdb) target remote :1234
(gdb) info registers         # 查看复位后 PC、SP
(gdb) stepi 10               # 执行前 10 条指令，观察跳转流程
(gdb) continue
```

崩溃后 PC 落在未知地址时：

```bash
(gdb) info symbol $pc        # 尝试还原符号
(gdb) x/4i $pc-8             # 反汇编崩溃点附近指令
(gdb) backtrace              # 查看调用链
```

#### 场景二：调试中断处理流程

```bash
(gdb) target remote :1234
(gdb) break TIM2_IRQHandler  # 对函数名下断点（需有调试符号）
(gdb) continue
# 中断触发时 GDB 自动暂停
(gdb) info registers         # 查看 mcause、mepc 等 CSR
(gdb) p/x *(volatile unsigned int*)0x40000000  # 查看外设寄存器（按实际地址替换）
(gdb) p/x $mepc              # 异常/中断返回地址
(gdb) p/x $mcause            # 异常原因（PFIC 注入后有特殊含义）
```

#### 场景三：触发软件复位

CH32 PFIC 支持写 `CFGR` 寄存器触发软件复位（`KEY=0xBEEF`，`SYSRESET=1`）：

```bash
(gdb) set *((unsigned int*)0xE000E048) = 0xBEEF0080
# QEMU 将模拟复位流程（ch32-stk-pfic.c 处理该写入）
# QEMU GDB stub 在复位后会断开连接，需重新 target remote :1234
```

#### 场景四：与 `flash-image` 配合（无 ELF 符号）

```bash
# QEMU 用 flash-image 启动
qemu-system-riscv32 -M ch32v317 \
  -machine ch32v317,flash-image=your-flash.bin \
  -monitor none -display none -serial stdio \
  -s -S

# GDB 先连接再手动加载符号
riscv32-wch-elf-gdb --nx
(gdb) target remote :1234
(gdb) add-symbol-file your-app.elf 0x08000000
(gdb) break main
(gdb) continue
```

> `add-symbol-file` 第二个参数为代码段基地址，通常为 `0x08000000`。

### 12.4 VS Code 配置

```jsonc
// .vscode/launch.json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "QEMU ch32v317 GDB",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build/your-app.elf",
      "miDebuggerPath": "/path/to/dist/toolchain-wch-qingke/bin/riscv32-wch-elf-gdb",
      "miDebuggerServerAddress": "localhost:1234",
      "stopAtEntry": false,
      "cwd": "${workspaceFolder}",
      "MIMode": "gdb",
      "setupCommands": [
        { "text": "set remotetimeout 60" },
        { "text": "set architecture riscv:rv32" }
      ]
    }
  ]
}
```

> `"request": "launch"` + `miDebuggerServerAddress`：QEMU 已在外部启动，VS Code 仅作 GDB 前端执行 `target remote`。

### 12.5 注意事项与限制

| 事项 | 说明 |
|------|------|
| **单核限制** | GDB stub 默认暂停/恢复整个 vCPU（对单核 CH32 无影响） |
| **硬件断点** | QEMU GDB stub 支持软件断点（无限制）；不模拟 CH32 片内硬件断点数量限制 |
| **`-S` 与外设初始化** | 用 `-S` 暂停时，QEMU 时钟不推进，TIM2 等 QEMUTimer 不会触发；`continue` 后时钟才正常走动 |
| **WDT 超时** | 调试暂停时虚拟时钟不走，**QEMU 不会因单步而触发 WDT 复位**（与真机不同，调试时间不受限） |
| **PFIC/VTF** | GDB 观察到的 `mcause` 反映 PFIC 注入的 IRQn，与标准 RISC-V `mcause` 有差异 |
| **步进精度** | `step` 为源码级单步（需 `-g`），`stepi` 为指令级；裸机调试推荐 `stepi` |

---

## 13. 当前模型能力汇总

**已具备（便于裸机 bring-up）**

- 片内 Flash / SRAM 线性映射、系统存储器（0x1FFF8000）占位，以及 `-kernel` ELF 或 `flash-image` 裸镜像 / Intel HEX 装载
- **启动模式仿真**（`boot-mode`）：根据手册表 1-1，`flash`（默认）/ `sysmem` / `sram` 三种模式，含 0x00000000 映射与 CPU resetvec 设置
- USART1 极简输出（轮询写 DATAR 即可在终端看到字符）
- QingKe XW / HPE 等在 TCG 中的简化实现（与手册、工具链对齐的细节见 §6）
- **特权模式**（M + U）：
  - V3/V4/V5 全系列（V3A〜V3V / V4A〜V4J / V5F）`misa_ext` 含 `RVU`，支持 M + U 两种特权模式（手册 §1.3）
  - V2 系列（V2A/V2C）仅支持 M 模式（手册 §1.3： MPP 恒为 0b11）
  - `mret` 时根据 `mstatus.MPP` 选择返回 M 或 U 模式；U 模式访问 M-CSR 触发 `mcause=2`（非法指令异常）
  - 已经由 `ch32v317-priv-verify.elf` 验证（5 项全部 PASS）
- **CPU 继承结构**：所有 WCH QingKe 型号继承 `TYPE_RISCV_CPU_WCH_QINGKE_BASE`（`"wch-qingke-base"`）；`TYPE_RISCV_CPU_BASE32`（`"rv32"`）已恢复继承 `DYNAMIC_CPU`（有 MMU/SV32），与 `spike`/`virt` 等标准机器兼容
- **以太网 MAC**（CH32V317/V307/V305 片内 EMAC+DMA，见 §9）：
  - CH32V317/V305：CH182 PHY 100Mbps 全双工（默认 `phy-model="ch182"`），支持 tap 后端双向通信、TX/RX COE 仿真
  - CH32V307：内置 10M PHY（`phy-model="phy10m"`），PHYLINK 中断自动注入，支持 EVT `eth_driver_10M.c` 驱动不修改运行
  - RTL8211F 千兆 RGMII PHY（可选，`phy-model="rtl8211f"`）：Clause22 + Page 0xd04 + MMD 寄存器完整模型，PHYID=0x001CC916，BCR=0x1140，PHYSR link-up 后 0x002C（1000M FD）
  - MACCR 扩展位：PS (bit15) / FES (bit14) / DM (bit11) 可读写，供驱动按协商结果写入链路速率/双工（tap 透传路径不受影响）
- **PFIC / SysTick / VTF 中断系统**（已全量建模）：
  - IENR/IRER/IPSR/IPRR/IACTR/IPRIOR/VTFIDR/VTFADDR/GISR/SCTLR 完整 MMIO，行为与手册一致
  - `NVIC_SetPendingIRQ(irq_n)` 通过 MEIP/`wch_evt_mcause_override` 路由，支持 VTF 快速中断（4 路通道）
  - mret 回调机制（`wch_pfic_mret_cb`）：ISR 返回时自动清 IACTR/override/MEIP，并重扫软件挂起中断，支持链式嵌套触发
  - 已验证：EVT INT 示例 `Interrupt_Nest`（8 级嵌套）、`Interrupt_VTF`、`VectorInRAM` 全部通过
- **PMP 物理内存保护**（4 组 PMPADDR + PMPCFG0，手册 §9.5.4）：
  - `csrw pmpaddr0-3` / `csrw pmpcfg0` CSR 可写（4 组 PMP 规则）
  - NAPOT/NA4/TOR 三种地址模式均支持
  - WCH QingKe PMP 行为差异建模：规则匹配时限制权限，不匹配时全部允许（与标准 RISC-V 相反）
  - 已验证：EVT PMP 示例 `PMP`（NAPOT 模式 Store access fault 正确触发）

**尚未建模或仅为占位**

- 片内 Flash 控制器（擦写、等待周期）等部分外设
- PFIC 中断优先级仲裁（`pfic_iprior` 寄存器已建模，但优先级比较逻辑尚未实现，所有使能中断按 IRQ 编号顺序服务）
- WFE 睡眠模式（`PFIC SCTLR` SLEEPONEXIT/SLEEPDEEP 寄存器已建模，但 CPU 不真正进入低功耗状态）
- **CH32V407 / V467 专项**：
  - 内核与存储容量已修正：默认 `-cpu wch-qingke-v3v`，Flash **992 KiB**（`0x08000000–0x080F7FFF`）、SRAM **200 KiB**（`0x20000000–0x20031FFF`），启动别名 `0x00000000–0x000F7FFF`
  - 仍未建模：**V 向量子集 `Zve64x+Zvbb`**（RVV 指令会 fault）、**200 MHz SYSCLK** 与等待周期差异、**64 KB SRAM 可重划为零等待 Flash** 的动态切换
  - 外设未实现：FSMC（Bank1/Bank2）、LTDC、ARGB、I3C、DVP、双 USBHS、SDIO、RNG、第 2/3 ADC/DAC、OPA、CH32V467 的片内 **PSRAM 4/8 MB** 窗口（`0x8000_0000 – 0x8080_0000`）

**后续扩展路线（可选）**

1. **PFIC 优先级仲裁**：按 `pfic_iprior` 字节优先级值做抢占式嵌套
2. **WFE / 低功耗**：实现 SCTLR 睡眠控制与 PFIC SEVONPEND 等唤醒机制
3. **Flash 控制器、DMA** 等按手册分阶段建模
4. **更多 `-mcpu` 对齐**：如 V3A/V4B/V5F（B 扩展、Zve 等）的对应 DEFINE_RISCV_CPU 与验证固件

> **真机对照**：`OrayOS-Tiny-Bin/OrayOS-Tiny.hex` 仍以 CH32V317 真机为主；全量 OrayOS 在 QEMU 上是否可跑取决于上述外设与中断模型完成度，勿与 `ch32v317-minimal` 自检混为一谈。

---

## 14. 故障排查

| 现象 | 可能原因 | 处理方向 |
|------|----------|----------|
| 提示需要 `-kernel` 或 `flash-image` | 未指定固件 | 加 `-kernel your.elf` 或 `-M ch32v317,flash-image=boot.bin` |
| **`Invalid CPU model`** | `-cpu` 与 `-M` 白名单不符 | 换合法组合（§5.1）或 `qemu-system-riscv32 -cpu help` |
| **`could not load ELF`** | 路径错、非 RV32 ELF、损坏 | 用 `file` / `readelf -h` 检查 |
| 串口有输出但无法输入命令（RT-Thread） | STK_CTLR bit31（SWTRG）触发软件中断机制缺失，`rt_schedule` 的任务切换从未生效 | 已在 `ch32-stk-pfic.c` 修复 |
| 串口无输出 | 未接 `-serial`、固件未写 USART1、写错基址 | 对照 §7 与 EVT 外设初始化 |
| 网络不通 / ARP 失败 | tap0 未创建 / 未配置 IP / QEMU 未启动 | 见 §9.5 |
| 一运行就异常 / 死循环 | 访问未实现外设、中断未建模 | 对照 §13，减少外设依赖或真机调试 |
| GDB 连接失败 | 端口未开启或被占用 | 确认 QEMU 启动时加了 `-s`；检查端口 `ss -tlnp | grep 1234` |


---

*文档版本：与仓库 QEMU overlay / 验证脚本行为一致；若升级 QEMU 版本或修改 `ch32-*.c`，请以源码与 `-M help` / `-cpu help` 为准。*
