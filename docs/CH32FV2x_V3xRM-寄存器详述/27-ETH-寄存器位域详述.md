# 第 27 章 以太网收发器（ETH）— 章节导航与型号分流

**来源**：《CH32F/V20x_V30x_V31x 系列应用手册》V2.4（[`../CH32FV2x_V3xRM.PDF`](../CH32FV2x_V3xRM.PDF)），**第 27 章**（PDF 第 **426–489** 页）。

---

## 0. 为什么「只有一份 ETH 文档」本身就是错的？

**手册第 27 章 首句即声明**（见 [按页导出文本](_layout_extract/ch27-eth_p429-492.txt) 第 6 行）：

> **本章模块描述适用于 CH32V30x、CH32V31x 和 CH32F20x 微控制器系列部分产品。**

而**第 27 章 的正文**进一步**用小节号把两种以太网分成两组完全不同的寄存器地图**：

| 小节 | 适用型号 | 外设类型 | 寄存器命名 | 基址与偏移 | 对应详述文档 |
|------|----------|----------|-----------|-----------|-------------|
| **27.1** | **CH32F207、CH32V307、CH32V317** | **全功能 MAC + MMC + PTP + DMA** 千兆以太网收发器；可选 10/100M 内置 PHY 或外置 1G PHY；支持 IEEE 1588 PTP、VLAN、哈希+完美过滤、魔法/唤醒帧、描述符链式/环式 DMA | **`R32_ETH_*` 全 32 位**；子块为 `MACCR/MACFFR/MACMIIAR/MACFCR/MACVLAN/MACPMTCSR/MACSR/MACIMR/MACAxHR|LR/MACCFG0` + `MMCCR/MMCRIR/…` + `PTPTSCR/PTPSSIR/PTPTSHR/…` + `DMABMR/DMATPDR/DMARPDR/DMARDLAR/DMATDLAR/DMASR/DMAOMR/DMAIER/DMAMFBOCR/DMACHTDR/DMACHRDR/DMACHTBAR/DMACHRBAR` | **MAC/MMC/PTP 基址 `0x4002_8000`**；**DMA 基址 `0x4002_9000`**；MAC 0x00 起、MMC 0x0100 起、PTP 0x0700 起、DMA 0x1000 起 | ➜ [**CH32V307-ETH-寄存器详述.md**](./CH32V307-ETH-寄存器详述.md) |
| **27.2** | **CH32F208、CH32V203、CH32V208** | **内置 10 Mbit/s MAC + 内置 10M PHY + 简化 DMA**；单缓冲收 / 单缓冲发；支持帧过滤、巨型帧、短包填充、Pause、50Ω 片内匹配电阻 | **`R8_/R16_/R32_ETH_*` 混合**；以 `EIE/EIR/ESTAT/ECON1/ECON2/ETXST/ETXLN/ERXST/ERXLN/HTL/HTH/ERXFCON/MACON1/MACON2/MABBIPG/EPAUS/MAMXFL/MIRD/MIREGADR/MISTAT/MIWR/MAADRL/MAADRH` 为主 | **基址 `0x4002_8000`**；**偏移从 `0x03` 起**；寄存器**宽度混排**，同址可能是 8/16/32 不同视图 | ➜ [**CH32V203-ETH-寄存器详述.md**](./CH32V203-ETH-寄存器详述.md) |
| **27.1.8.5** | **27.1 与 27.2 共用** | **内部 10M PHY 寄存器**（经 SMI 间接访问，非 MMIO 空间） | `BMCR/BMSR/ANLPAR/PHY_SR/PHY_MDIX` | PHY 内部偏移 `0x00/0x01/0x05/0x10/0x1E` | 见上述任一详述的「内部 10M PHY」节 |

**结论**：CH32V203 与 CH32V307 的以太网控制器在**芯片实现**、**寄存器映射**、**编程模型** 上**均不相同**。它们**同在第 27 章** 是因为**合并手册的编目方式**（文档结构上一章，内部按芯片族分两小节）——**不是** 一个外设。

---

## 1. 两款以太网核对比（速览）

| 维度 | **CH32V307 / V317 / F207（27.1）** | **CH32V203 / V208 / F208（27.2）** |
|------|-------------------------------------|-------------------------------------|
| 速率 | 10 / 100 / **1000** Mbps | **10** Mbit/s |
| MAC 核 | 大而全（ST/DW 系 `MACCR` 模型） | 精简（ENC28J60 风格的 `ECON1/ECON2`） |
| 接口 | MII / RMII / **RGMII** + 可选内置 10M PHY 或内置 10/100M（V317）或外置 1G PHY | **仅**内置 10M PHY |
| DMA | **32 位宽专用 DMA**，收发描述符链式/环式，`DMATDLAR/DMARDLAR` | **单发送缓冲 + 单接收缓冲**，地址写 `ETXST/ERXST`，长度写 `ETXLN/ERXLN` |
| MMC | **有**（计数 + 中断 + 冻结） | 无 |
| PTP / IEEE 1588 | **有**（秒 + 亚秒、精/粗调、PPS、目标中断） | 无 |
| 魔法帧/唤醒帧 | **有**（`PMT` + `MACRWUFFR` 8 长字） | 仅**魔法包过滤**位（`RB_ETH_ERXFCON_MPEN`） |
| VLAN 识别 | 有（`MACVLAN`，按 `VLANT/VLANTI`） | 短包填充表里**间接识别** `0x8100`（VLAN）自动 64 B 填充 |
| 硬件 CRC 卸载 / IP/UDP/TCP 校验和 | **有**（`MACCR.IPCO` + `TDes0.CIC`） | 仅**填充 CRC**（`MACON2.TXCRCEN`），无 IP/UDP/TCP 校验和卸载 |
| 50 Ω 片内匹配电阻 | **有**（`MACCR.PR`） | **有**（`EIE.R_EN50`） |
| NVIC 向量 | `ETH_IRQn`（见 `ch32v30xhw.h`，V307 典型 `77`） | `ETH_IRQn = 61`（V20x_D8/D8W，`ch32v20xhw.h`） |
| 头文件结构 | `ETH_TypeDef` + 子块 `ETH_MAC/ETH_MMC/ETH_PTP/ETH_DMA`；`ETH_DMATxDesc_*`/`Rx` 宏 | **`ETH10M_TypeDef`**（`#if defined(CH32V20x_D8) \|\| CH32V20x_D8W`）；不依赖描述符宏 |
| EXTEN 配合 | `EXTEN_ETH_10M_EN`（若用内置 10M PHY）、`EXTEN_ETH_RGMII_SEL`（RGMII 封装） | `EXTEN_ETH_10M_EN`（内置 10M PHY）；V203 不支持 RGMII |
| RCC 位 | `RCC_ETHMACEN / ETHMACTXEN / ETHMACRXEN`（+ V30x `ETH1GSRC/ETH1G_125M_EN`） | `RCC_ETHMACEN / ETHMACTXEN / ETHMACRXEN`（V20x 可用） |

---

## 2. 本章子节对照（便于查 PDF 原页）

| PDF 子节 | 内容 | 起始行（[`_layout_extract/ch27-eth_p429-492.txt`](_layout_extract/ch27-eth_p429-492.txt)） |
|----------|------|-----------------------------------------------------------------------------|
| 27.1 | 关于 CH32F207/V307/V317 产品（MAC + MMC + PTP + DMA + 10M PHY 选配） | 14 |
| 27.1.1.1~6 | 特征：MAC / DMA / MMC / PTP / 10M PHY / V317 10-100M PHY | 25~90 |
| 27.1.3 | 引脚与 MII/RMII/RGMII 选择（表 27-1） | 166 |
| 27.1.4 | PHY 管理与 SMI/MII/RMII/RGMII 时序 | 208 |
| 27.1.4.4 | 内部 10M / V317 10-100M 使用注意 | 467 |
| 27.1.4.5 | 时钟产生与 MCO 输出（PLL3 60 MHz 等） | 511 |
| 27.1.5 | IEEE 802.3 + 1588 帧 / 过滤 / MMC / PMT / PTP | 565 |
| 27.1.6 | DMA 操作与描述符（**表 27-11～21**，TDes0/1/2/3、RDes0/1/2/3 位图） | 845 |
| 27.1.7 | DMA / ETH / PMT 中断 | 1483 |
| 27.1.8 | **V307 系寄存器位域详述**（**表 27-22** 全表） | 1517 |
| 27.1.8.1 | MAC 控制寄存器各位域 | 1609 |
| 27.1.8.2 | MMC 寄存器各位域 | 2291 |
| 27.1.8.3 | PTP 寄存器各位域 | 2517 |
| 27.1.8.4 | DMA 寄存器各位域 | 2715 |
| 27.1.8.5 | **内部 10M PHY 寄存器**（V307、V203 共用） | 3200 |
| **27.2** | **关于 CH32F208/V203/V208 产品** | **3338** |
| 27.2.1 | 控制器简介 | 3339 |
| 27.2.2 | **V203 系寄存器描述**（**表 27-23**） | 3360 |
| 27.2.2.1~22 | `EIE/EIR/ESTAT/ECON2/ECON1/ETXST/ETXLN/ERXST/ERXLN/HTL/HTH/ERXFCON/MACON1/MACON2/MABBIPG/EPAUS/MAMXFL/MIRD/MIREGADR/MISTAT/MIWR/MAADRL+MAADRH` | 3397~3679 |
| 27.2.3 | 操作指南（初始化、发送、接收） | 3681 |

---

## 3. 按页 `pdftotext` 导出原文

```bash
# 仓库根执行：按章按页提取原排版文本
./scripts/ch32-rm-pdftotext.sh 429 492 ch27-eth
```

已有：[`_layout_extract/ch27-eth_p429-492.txt`](_layout_extract/ch27-eth_p429-492.txt)（含 27.1/27.2 全部表 27-1～23）。

---

## 4. 其它入口

- **应用 / 板级 / 描述符例程**（V307）：[../CH32V30X/CH32V307-ETH.md](../CH32V30X/CH32V307-ETH.md)。
- **全仓 ETH 速查表**：[../CH32FV2x_V3xRM-外设寄存器速查.md](../CH32FV2x_V3xRM-外设寄存器速查.md) 的 `ETH`/`EXTEN` 行。
- **芯片内存布局**：[../CH32V-寄存器与内存布局.md](../CH32V-寄存器与内存布局.md) 中 `ETH` 与 `ETH10M` 两条。
- **QEMU 模型**：V307 的千兆 MAC + V203 的 ETH10M 已分别实现，见 [../QingKe-MCU-QEMU使用手册.md](../QingKe-MCU-QEMU使用手册.md)。

---

## 5. 常见误区

1. **不要** 把 `R32_ETH_MACCR @ 0x40028000` 往 V203 上写，**V203 在这个地址上存的是前三字节保留 + `R8_ETH_EIE`**；  
2. **不要** 在 V307 上按 `R8_ETH_ECON1` 启动发送，V307 使用的是 **`DMAOMR.ST`**、描述符的 `TDes0.OWN`；  
3. **内部 10M PHY** 两族共用（**27.1.8.5**），但**通过哪个 SMI 接口**访问不同：V307 走 `MACMIIAR/MACMIIDR`（32 位），V203 走 `MIREGADR/MIRD/MIWR`（8/16 位混排）。
