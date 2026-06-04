English | [中文](README.md)

# QEMU CH32V — WCH QingKe RISC-V MCU Full-System Emulation

Full-system emulation environment for **WCH CH32 / QingKe** MCUs based on **QEMU 10.2.2**, covering the entire CH32V003 – CH32V407 / CH32H417 product line. Perform instruction-level firmware verification, USART serial debugging, Ethernet stack integration testing, USB device enumeration, and GDB debugging — all on your PC, no hardware required.

## ✨ Features

- **10 machine models**: CH32V003 / V103 / V203 / V203RB / V303 / V305 / V307 / V317 / V407 / H417
- **Full QingKe CPU family**: V2A through V5F (with XW compressed load/store, HPE hardware push/pop, mcpy instruction)
- **PFIC / SysTick / VTF interrupt system**: Complete MMIO modeling, supports 8-level nesting and VTF fast interrupts
- **Ethernet emulation**: On-chip EMAC+DMA with three PHY models (CH182 / 10M / RTL8211F), bidirectional tap backend
- **USBHS / USBFS host mode**: Real enumeration of downstream USB devices (keyboard, CH341 passthrough, etc.)
- **PMP physical memory protection**: 4 rule sets, NAPOT/NA4/TOR modes, WCH-specific behavior modeling
- **Multiple boot modes**: Flash / System Memory / SRAM, aligned with datasheet Table 1-1
- **Firmware loading**: ELF (`-kernel`), raw `.bin`, Intel HEX (`flash-image=`)
- **GDB debugging**: Built-in GDB stub with breakpoints, single-stepping, register/memory inspection

## 📦 Repository Structure

```
qemu-ch32v/
├── build-wch-qemu.sh          # One-click build script (Chinese messages)
├── build-wch-qemu-en.sh       # One-click build script (English messages)
├── qemu-overlay/10.2.2/       # CH32 machine model & QingKe CPU sources
│   ├── hw/riscv/ch32-*.c/.h   #   Board-level entry + peripheral modules (25+ files)
│   └── target/riscv/          #   QingKe CPU definitions, XW/HPE/mcpy decoding
├── patches/qemu/              # Upstream QEMU incremental patches (Kconfig / meson)
├── docs/
│   ├── QingKe-MCU-QEMU使用手册.md     # Full user manual (Chinese)
│   ├── CH32FV2x_V3xRM-寄存器详述/     # Peripheral register field docs (29 files)
│   └── Datasheet/                      # Official datasheet PDFs
└── LICENSE                    # CC0 1.0
```

## 🚀 Quick Start

### 1. Build QEMU

**Requirements**: Linux x86_64. On Debian/Ubuntu, the script automatically installs dependencies.

```bash
git clone <this-repo>
cd qemu-ch32v

# Show help (all options and default paths)
./build-wch-qemu-en.sh --help

# Build (installs to dist/qemu-10.2.2-riscv32/ by default)
./build-wch-qemu-en.sh -j$(nproc)

# Add to PATH
export PATH="$PWD/dist/qemu-10.2.2-riscv32/bin:$PATH"
qemu-system-riscv32 --version
```

Build workflow: Download official QEMU 10.2.2 tarball → overlay CH32 model sources → apply patches → configure (`riscv32-softmmu` only) → compile & install.

### 2. Run Firmware

```bash
# CH32V317 machine
qemu-system-riscv32 \
  -M ch32v317 \
  -display none \
  -serial stdio \
  -kernel your-app.elf

# CH32V203 machine
qemu-system-riscv32 \
  -M ch32v203 \
  -display none \
  -serial stdio \
  -kernel your-app.elf

# With Ethernet (requires tap setup, see user manual §9)
qemu-system-riscv32 \
  -M ch32v317 \
  -display none \
  -serial stdio \
  -nic "tap,ifname=tap0,script=no,downscript=no" \
  -kernel your-app.elf

# Intel HEX or raw binary
qemu-system-riscv32 \
  -M ch32v317,flash-image=firmware.hex \
  -display none \
  -serial stdio

# GDB debugging
qemu-system-riscv32 \
  -M ch32v317 \
  -display none \
  -serial stdio \
  -kernel your-app.elf \
  -s -S
# In another terminal:
# riscv32-wch-elf-gdb your-app.elf -ex "target remote :1234"
```

## 🖥️ Supported Machines & CPUs

### Machines (`-M`)

| Machine | Flash | SRAM | Default CPU | Ethernet |
|---------|-------|------|-------------|----------|
| `ch32v317` | 480 KiB | 192 KiB | `wch-qingke-v4f` | ✅ CH182 100M |
| `ch32v307` | 256 KiB | 64 KiB | `wch-qingke-v4f` | ✅ Built-in 10M |
| `ch32v305` | 288 KiB | 64 KiB | `wch-qingke-v4f` | ✅ CH182 100M |
| `ch32v303` | 256 KiB | 64 KiB | `wch-qingke-v4f` | ✅ CH182 100M |
| `ch32v203` | 256 KiB | 64 KiB | `wch-qingke-v4b` | — |
| `ch32v203rb` | 128 KiB | 64 KiB | `wch-qingke-v4b` | ✅ ETH10M |
| `ch32v103` | 64 KiB | 20 KiB | `wch-qingke-v3a` | — |
| `ch32v003` | 16 KiB | 2 KiB | `wch-qingke-v2c` ⚠ | — |
| `ch32v407` | 992 KiB | 200 KiB | `wch-qingke-v3v` | ✅ |
| `ch32h417` | 896 KiB ⚠ | 192 KiB ⚠ | `wch-qingke-v5f` ⚠ | — |

> ⚠ `ch32v003` uses V2C as placeholder (real chip is V2A); `ch32h417` is a placeholder implementation.

### CPUs (`-cpu`)

| CPU | Architecture | Privilege Modes | XW | HPE | mcpy |
|-----|-------------|-----------------|----|----|------|
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

### MCU Model Support Matrix

The following table maps WCH CH32 / QingKe MCU families to QEMU machines:

| MCU Family | Representative Models | QingKe Core | QEMU Machine (`-M`) | Support Status |
|-----------|----------------------|-------------|---------------------|----------------|
| **CH32V003** | CH32V003F4P6, CH32V003A4M6, CH32V003J4M6 | V2A (RV32EC) | `ch32v003` | ⚠ CPU uses V2C placeholder |
| **CH32V002/004/005/006/007** | CH32V006K8U6 etc. | V2C (RV32EC+Zmmul) | `ch32v003` | ⚠ Shares V003 machine |
| **CH32V103** | CH32V103C8T6, CH32V103R8T6 | V3A (RV32IMAC) | `ch32v103` | ✅ |
| **CH32L103** | CH32L103C8T6 | V3A (RV32IMAC) | `ch32v103` | ✅ Shares V103 machine |
| **CH32V203** | CH32V203C8T6, CH32V203C6T6, CH32V203K8T6 | V4B (RV32IMAC+XW) | `ch32v203` | ✅ |
| **CH32V203RBT6** | CH32V203RBT6 (D8 package, w/ ETH10M) | V4B (RV32IMAC+XW) | `ch32v203rb` | ✅ With 10M Ethernet |
| **CH32V208** | CH32V208WBU6, CH32V208GBU6 | V4C (RV32IMAC+XW) | `ch32v203` | ⚠ Shares V203, no BLE |
| **CH32V303** | CH32V303VCT6, CH32V303RCT6, CH32V303CBT6 | V4F (RV32IMAFC+XW) | `ch32v303` | ✅ |
| **CH32V305** | CH32V305FBP6, CH32V305RBT6 | V4F (RV32IMAFC+XW) | `ch32v305` | ✅ With Ethernet |
| **CH32V307** | CH32V307VCT6, CH32V307RCT6, CH32V307WCU6 | V4F (RV32IMAFC+XW) | `ch32v307` | ✅ With 10M Ethernet |
| **CH32V317** | CH32V317WCU6, CH32V317VCT6 | V4F (RV32IMAFC+XW) | `ch32v317` | ✅ With 100M Ethernet |
| **CH32V407** | CH32V407VET6 etc. | V3V (RV32IMACB+XW) | `ch32v407` | ⚠ Vector subset not implemented |
| **CH32V467** | CH32V467VET6 etc. | V3V (RV32IMACB+XW) | `ch32v407` | ⚠ Shares V407, no PSRAM |
| **CH32H417** | CH32H417VET6 etc. | V5F (RV32IMAFCB+XW) | `ch32h417` | ⚠ Single-core placeholder |
| **CH571/CH573** | CH573F, CH571D etc. | V3A (RV32IMAC) | `ch32v103` | ⚠ No BLE emulation |
| **CH581/CH582/CH583** | CH582M, CH583F etc. | V4A (RV32IMAC) | `ch32v203` | ⚠ No BLE emulation |
| **CH584/CH585** | CH584M etc. | V4C (RV32IMAC+XW) | `ch32v203` | ⚠ No BLE emulation |
| **CH32X033/X035** | CH32X035G8U6 etc. | V3B (RV32I[M]C+XW) | `ch32v103` | ⚠ Significant peripheral differences |
| **CH591/CH592** | CH592F etc. | V3B (RV32I[M]C+XW) | `ch32v103` | ⚠ No BLE emulation |
| **CH643/CH645** | CH643Q, CH645Q etc. | V4C (RV32IMAC+XW) | `ch32v203` | ⚠ No RGB LED emulation |

**Legend**: ✅ = Primary verification target with aligned peripherals & memory layout | ⚠ = Runnable with limitations (shared machine, no BLE/wireless, missing peripherals)

> **Tip**: All machines support `-cpu` switching (see whitelist above). Choose the `-cpu wch-qingke-*` matching your chip's core. For models not listed, select the machine with the closest core and peripheral set.

## 📖 Documentation

- **[QingKe MCU QEMU User Manual](docs/QingKe-MCU-QEMU使用手册.md)** (Chinese) — Complete reference covering build, machine/CPU selection, firmware requirements, networking, USB emulation, GDB debugging, capability boundaries, and troubleshooting
- **[Peripheral Register Reference](docs/CH32FV2x_V3xRM-寄存器详述/)** — Bit-field level register documentation for CH32 peripherals (29 files, based on Reference Manual V2.4)
- **[Patch Notes](patches/qemu/README.md)** — Upstream QEMU patches and overlay file mapping
- **[Official Datasheets](docs/Datasheet/)** — CH32V203 / V208 / V307 datasheet PDFs

## ⚙️ Build Options

```bash
./build-wch-qemu-en.sh [options]

Options:
  --prefix PATH          Install directory (default: dist/qemu-10.2.2-riscv32)
  --downloads-dir PATH   Source tarball cache directory
  --build-dir PATH       Build root directory
  -j N                   Parallel build jobs (default: nproc)
  --no-install-deps      Skip apt dependency installation
  --skip-archive-sha256  Skip source tarball SHA256 verification
  --force-rebuild        Force re-extraction of sources
  --clean                Remove build directory and rebuild

Environment variables:
  WCH_QEMU_VERSION       QEMU version (default: 10.2.2)
  WCH_QEMU_URL           Download URL
  WCH_QEMU_SHA256        Expected SHA256 (lowercase hex)
  JOBS                   Parallel build jobs
```

## 🔧 Peripheral Modeling Status

| Peripheral | Status | Notes |
|-----------|--------|-------|
| Flash / SRAM | ✅ Complete | Linear mapping with boot aliasing |
| USART1 | ✅ Complete | STATR/DATAR, connected to `-serial` |
| PFIC / SysTick / VTF | ✅ Complete | Full MMIO: IENR/IRER/IPSR/IPRR/IACTR/IPRIOR etc. |
| Ethernet MAC+DMA | ✅ Complete | TX/RX COE, three PHY models |
| USBHS Host | ✅ Complete | Device enumeration, DMA transfer, interrupt routing |
| USBFS Host | ✅ Complete | Low/Full-speed device enumeration |
| RCC | ✅ Modeled | HSI/HSE/PLL ready bits, SWS tracking, clock enable |
| IWDG | ✅ Modeled | Key sequence, watchdog feed, timeout reset |
| RTC | ✅ Modeled | CNT/ALR, write protection, SECF/ALRF |
| CRC | ✅ Modeled | Hardware CRC32 computation |
| RNG | ✅ Modeled | DRDY auto-set |
| PMP | ✅ Modeled | 4 rule sets, WCH-specific behavior |
| GPIO / AFIO / EXTI | 🔲 Stub | Read/write round-trip |
| DMA / TIM / SPI / I2C | 🔲 Stub | Read/write round-trip |
| ADC / DAC / WWDG / BKP / PWR / SDIO | 🔲 Stub | Read/write round-trip |
| USBHS Device / USBFS OTG | 🔲 Stub | No actual transfers |
| Flash Controller (erase/write) | ❌ Not implemented | — |
| PFIC Priority Arbitration | ❌ Not implemented | IRQs served by number order |
| WFE Low Power | ❌ Not implemented | — |

## 📄 License

This repository is released under **CC0 1.0 Universal**. QEMU itself follows its upstream license (GPLv2).
