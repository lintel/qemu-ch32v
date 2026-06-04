# CH32V203 / CH32V208 / CH32F208 — 以太网寄存器位域详述（内置 10M MAC+PHY）

**来源**：《CH32F/V20x_V30x_V31x 系列应用手册》V2.4（[`../CH32FV2x_V3xRM.PDF`](../CH32FV2x_V3xRM.PDF)），**第 27 章「以太网收发器（ETH）」**。  
**适用**：**本节 27.2** 明确覆盖 **CH32F208、CH32V203、CH32V208**（**集成 10 Mbit/s MAC + 内置 PHY + DMA** 的专用以太网控制器）。  
**区分**：**同一本手册同为第 27 章**，但 **27.1** 对应 V307/V317/F207 的千兆 MAC（`R32_ETH_MACCR` 等）、**完全不同**，见 [CH32V307-ETH-寄存器详述.md](./CH32V307-ETH-寄存器详述.md)。

> **关键事实**：V203 **绝不**使用 `R32_ETH_MACCR / MACFFR / DMABMR / DMATDLAR` 那一套——这属于 V307；V203 是一套 **字节/半字混排** 的 `R8_ETH_EIE / R8_ETH_ECON1 / R16_ETH_ETXST / R32_ETH_MAADRL …` 控制器，**基址起自 `0x4002_8003`**（注意不是 `0x4002_8000`）。

---

## 1. 控制器简介（27.2.1）

集成以太网控制器 **MAC + 10 Mbit/s 内置 PHY + DMA**，兼容 **IEEE 802.3**；DMA 在系统 RAM 中发送/接收数据；PHY 提供少量可写寄存器以调整收发性能。

**主要特性**：
- 支持 **全/半双工**；
- **短包填充**（多种模式）、**CRC 设置与填充**；
- 支持**巨型帧接收**；
- 多种**过滤模式组合**（UC/MC/BC/HASH/CRC/魔法包）；
- 支持 **Pause 帧发送与设置**；
- 支持**自动协商**；
- **DMA** 负责收发数据搬运；
- **PHY 收发器兼容 10BASE-T**，发送模块支持**节能模式**；
- 内置 **50 Ω 传输阻抗匹配电阻**（可开/关或外接）；
- 提供 **IEEE 全球唯一 MAC 地址**（**`R32_ETH_MAADRL / R16_ETH_MAADRH` 上电复位值 = 出厂烧入**）。

**访问模型**：
- **控制寄存器** 以 **8/16/32 位混合** 的字节偏移放在 **`0x4002_8000`** 外设段；
- **同一物理地址** 因宽度不同有**多个别名**（例：`0x40028008` 既是 `R32_ETH_TX` 32 位视图，也是 `R16_ETH_ETXST` 16 位视图）。
- 配合 `ch32v20xhw.h` 的 `#if defined(CH32V20x_D8) || defined(CH32V20x_D8W)` 段中 **`ETH10M_TypeDef`** 与 **`ETH10M_BASE = AHBPERIPH_BASE + 0x8000`** 使用。

---

## 2. 寄存器地图（表 27-23）

> **基址 `0x4002_8000`**；**偏移从 `0x03` 开始**（前三字节为隐含保留/对齐）。

| 名称 | 宽 | 绝对地址 | 描述 | 复位值 |
|------|----|----------|------|--------|
| `R8_ETH_EIE`     | 8  | `0x40028003` | 中断使能 | `0x00` |
| `R8_ETH_EIR`     | 8  | `0x40028004` | 中断标志 | `0x00` |
| `R8_ETH_ESTAT`   | 8  | `0x40028005` | 状态 | `0x00` |
| `R8_ETH_ECON2`   | 8  | `0x40028006` | PHY 模拟参数（复位 **`0x0A`**） | `0x0A` |
| `R8_ETH_ECON1`   | 8  | `0x40028007` | 收发控制 | `0x00` |
| `R32_ETH_TX`     | 32 | `0x40028008` | 发送 DMA 控制（`ETXST + ETXLN` 合并视图） | `0xXXXXXXXX` |
| `R16_ETH_ETXST`  | 16 | `0x40028008` | 发送 DMA 缓冲区起始地址 | `0xXXXX` |
| `R16_ETH_ETXLN`  | 16 | `0x4002800A` | 发送长度 | `0xXXXX` |
| `R32_ETH_RX`     | 32 | `0x4002800C` | 接收 DMA 控制（`ERXST + ERXLN` 合并视图） | `0x00000000` |
| `R16_ETH_ERXST`  | 16 | `0x4002800C` | 接收 DMA 缓冲区起始地址 | `0x0000` |
| `R16_ETH_ERXLN`  | 16 | `0x4002800E` | 接收长度 | `0x0000` |
| `R32_ETH_HTL`    | 32 | `0x40028010` | Hash 表低字节（出厂烧入，复位 **`0x484EA033`**） | `0x484EA033` |
| `R32_ETH_HTH`    | 32 | `0x40028014` | Hash 表高字节（复位 **`0x5000EF97`**） | `0x5000EF97` |
| `R32_ETH_MACON`  | 32 | `0x40028018` | 接收过滤设置（复位 **`0x10000000`**，`ERXFCON/MACON1/MACON2/MABBIPG` 合并视图） | `0x10000000` |
| `R8_ETH_ERXFCON` | 8  | `0x40028018` | 接收包过滤控制 | `0x00` |
| `R8_ETH_MACON1`  | 8  | `0x40028019` | MAC 层流控制 | `0x00` |
| `R8_ETH_MACON2`  | 8  | `0x4002801A` | MAC 层封包控制 | `0x00` |
| `R8_ETH_MABBIPG` | 8  | `0x4002801B` | 最小包间间隔（复位 `0x10` = **0b001_0000**） | `0x10` |
| `R32_ETH_TIM`    | 32 | `0x4002801C` | 流控暂停帧时间（`EPAUS + MAMXFL` 合并视图） | `0xXXXXXXXX` |
| `R16_ETH_EPAUS`  | 16 | `0x4002801C` | 流控暂停帧时间 | `0xXXXX` |
| `R16_ETH_MAMXFL` | 16 | `0x4002801E` | 最大接收包长度 | `0x0000` |
| `R16_ETH_MIRD`   | 16 | `0x40028020` | MII 读数据（复位 `0x1100`） | `0x1100` |
| `R32_ETH_MIWR`   | 32 | `0x40028024` | MII 写（`MIREGADR + MISTAT + MIWR16` 合并视图） | `0x00000000` |
| `R8_ETH_MIREGADR`| 8  | `0x40028024` | MII 地址 | `0x00` |
| `R8_ETH_MISTAT`  | 8  | `0x40028025` | MII 状态 | `0x00` |
| `R16_ETH_MIWR`   | 16 | `0x40028026` | MII 写数据 | `0x0000` |
| `R32_ETH_MAADRL` | 32 | `0x40028028` | MAC 地址低（字节 1–4，**出厂** IEEE 地址） | `0xXXXXXXXX` |
| `R16_ETH_MAADRH` | 16 | `0x4002802C` | MAC 地址高（字节 5–6） | `0xXXXX` |

> `R32_ETH_TX` / `R32_ETH_RX` / `R32_ETH_MACON` / `R32_ETH_TIM` / `R32_ETH_MIWR` 为**合并视图**，允许一次 32 位写同时设置两/四个 8/16 位寄存器；**字节视图访问** 亦有效。

**内部 10M PHY 寄存器**：见 [§6 本文末](#6-内部-10m-phy-寄存器sm-访问)（**经 SMI 通过 `R16_ETH_MIRD/MIWR + R8_ETH_MIREGADR` 间接访问**；定义与 CH32V307 相同，均在手册 **27.1.8.5**）。

---

## 3. MAC / DMA 控制位域（27.2.2.1 ~ 27.2.2.22）

### 3.1 `R8_ETH_EIE` — 中断使能（地址 `0x40028003`）

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| 7 | `RB_ETH_EIE_INTIE` | RW | 以太网中断总使能 | 0 |
| 6 | `RB_ETH_EIE_RXIE`  | RW | 接收完成中断使能 | 0 |
| 5 | Reserved | RO | — | 0 |
| 4 | `RB_ETH_EIE_LINKIE`| RW | Link 变化中断使能 | 0 |
| 3 | `RB_ETH_EIE_TXIE`  | RW | 发送完成中断使能 | 0 |
| 2 | `RB_ETH_EIE_R_EN50`| RW | 内置 **50 Ω** 阻抗电阻：0=断开；1=连接 | 0 |
| 1 | `RB_ETH_EIE_TXERIE`| RW | 发送错误中断使能 | 0 |
| 0 | `RB_ETH_EIE_RXERIE`| RW | 接收错误中断使能 | 0 |

### 3.2 `R8_ETH_EIR` — 中断标志（地址 `0x40028004`，**RW1 清**）

| 位 | 名称 | 描述 |
|----|------|------|
| 7 | Reserved | — |
| 6 | `RB_ETH_EIR_RXIF`   | 接收完成 |
| 5 | Reserved | — |
| 4 | `RB_ETH_EIR_LINKIF` | Link 变化 |
| 3 | `RB_ETH_EIR_TXIF`   | 发送完成 |
| 2 | Reserved | — |
| 1 | `RB_ETH_EIR_TXERIF` | 发送错误 |
| 0 | `RB_ETH_EIR_RXERIF` | 接收错误 |

### 3.3 `R8_ETH_ESTAT` — 状态（地址 `0x40028005`）

| 位 | 名称 | 访问 | 描述 |
|----|------|------|------|
| 7 | `RB_ETH_ESTAT_INT`     | RW1 | 中断 |
| 6 | `RB_ETH_ESTAT_BUFER`   | RW1 | Buffer 错误 |
| 5 | `RB_ETH_ESTAT_RXCRCER` | RO  | 接收 CRC 错 |
| 4 | `RB_ETH_ESTAT_RXNIBBLE`| RO  | 接收 nibble 错 |
| 3 | `RB_ETH_ESTAT_RXMORE`  | RO  | 接收超过设定最大包长 |
| 2 | `RB_ETH_ESTAT_RXBUSY`  | RO  | 接收进行中 |
| 1 | `RB_ETH_ESTAT_TXABRT`  | RO  | 发送被 MCU 打断 |
| 0 | Reserved               | RO  | — |

### 3.4 `R8_ETH_ECON2` — PHY 模拟参数（地址 `0x40028006`，复位 `0x0A`）

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| `[7:4]` | Reserved | RO | — | 0 |
| `[3:1]` | `RB_ETH_ECON2_RX_MUST` | RW | **保留，必须写入 `110b`** | `110b` |
| 0 | `RB_ETH_ECON2_TX` | RW | 发送端节能驱动：0=额定；1=节能 | 0 |

### 3.5 `R8_ETH_ECON1` — 收发控制（地址 `0x40028007`）

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| 7 | `RB_ETH_ECON1_TXRST` | RW | 发送模块复位 | 0 |
| 6 | `RB_ETH_ECON1_RXRST` | RW | 接收模块复位 | 0 |
| `[5:4]` | Reserved | RO | — | 0 |
| 3 | `RB_ETH_ECON1_TXRTS` | RW | **启动发送**；发送完成自动清零 | 0 |
| 2 | `RB_ETH_ECON1_RXEN`  | RW | 使能接收 | 0 |
| `[1:0]` | Reserved | RO | — | 0 |

### 3.6 `R16_ETH_ETXST` / `ETXLN`（地址 `0x40028008`/`0x4002800A`）

- `ETXST[15:0]`：**发送 DMA 缓冲区起始地址**；**低 15 位有效**；**必须 4 字节对齐**。
- `ETXLN[15:0]`：**发送长度**。

### 3.7 `R16_ETH_ERXST` / `ERXLN`（地址 `0x4002800C`/`0x4002800E`）

- `ERXST[15:0]`：**接收 DMA 缓冲区起始地址**，低 15 位，**4 字节对齐**。
- `ERXLN[15:0]`：**接收长度**（RO，硬件回写）。

### 3.8 `R32_ETH_HTL` / `HTH` — 哈希表（地址 `0x40028010`/`0x40028014`）

- `HTL`：字节 0~3（`R8_ETH_EHT0..EHT3`），**复位 `0x484EA033`**。
- `HTH`：字节 4~7（`R8_ETH_EHT4..EHT7`），**复位 `0x5000EF97`**。

### 3.9 `R8_ETH_ERXFCON` — 接收过滤（地址 `0x40028018`）

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| 7 | `RB_ETH_ERXFCON_UCEN`  | RW | 单播匹配：0=全丢；1=收目标地址匹配 | 0 |
| 6 | Reserved | RO | — | 0 |
| 5 | `RB_ETH_ERXFCON_CRCEN` | RW | CRC 错包：0=丢；1=收 | 0 |
| 4 | `RB_ETH_ERXFCON_EN`    | RW | 接收过滤使能 | 0 |
| 3 | `RB_ETH_ERXFCON_MPEN`  | RW | 魔法包：0=丢；1=收 | 0 |
| 2 | `RB_ETH_ERXFCON_HTEN`  | RW | Hash 表匹配：0=丢；1=收 | 0 |
| 1 | `RB_ETH_ERXFCON_MCEN`  | RW | 组播全收/全丢 | 0 |
| 0 | `RB_ETH_ERXFCON_BCEN`  | RW | 广播全收/全丢 | 0 |

### 3.10 `R8_ETH_MACON1` — MAC 层流控（地址 `0x40028019`）

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| `[7:6]` | Reserved | RO | — | 0 |
| `[5:4]` | `RB_ETH_MACON1_FCEN` | RW | Pause（全双工下）：00=停止发送；01=单次后停止；10=周期；11=发 `0 timer` 后停止 | 0 |
| 3 | `RB_ETH_MACON1_TXPAUS` | RW | 发送 Pause 使能 | 0 |
| 2 | `RB_ETH_MACON1_RXPAUS` | RW | 接收 Pause 使能 | 0 |
| 1 | `RB_ETH_MACON1_PASSALL`| RW | 控制帧：0=过滤；1=未过滤者写入缓存 | 0 |
| 0 | `RB_ETH_MACON1_MARXEN` | RW | MAC 接收使能（等同**打开接收主开关**） | 0 |

### 3.11 `R8_ETH_MACON2` — MAC 层封包（地址 `0x4002801A`）

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| `[7:5]` | `RB_ETH_MACON2_PADCFG[2:0]` | RW | 短包填充：`000/010/100/110` 不填；`001` 填到 60 B + 4 B CRC；`011/111` 填到 64 B + 4 B CRC；`101` 若为 VLAN（`0x8100`）→ 64 B，否则 60 B，均附 CRC | 0 |
| 4 | `RB_ETH_MACON2_TXCRCEN` | RW | 硬件填 CRC | 0 |
| 3 | `RB_ETH_MACON2_PHDREN`  | RW | 特殊 4 字节不参与 CRC | 0 |
| 2 | `RB_ETH_MACON2_HFRMEN`  | RW | 允许接收巨型帧 | 0 |
| 1 | Reserved | RO | — | 0 |
| 0 | `RB_ETH_MACON2_FULDPX`  | RW | 通讯模式：0=半双工；1=全双工 | 0 |

### 3.12 `R8_ETH_MABBIPG` — 最小包间间隔（地址 `0x4002801B`，复位 `0b001_0000`）

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| 7 | Reserved | RO | — | 0 |
| `[6:0]` | `R8_ETH_MABBIPG[6:0]` | RW | 最小包间间隔（字节） | `0b001_0000` |

### 3.13 `R16_ETH_EPAUS` — 暂停帧时间（地址 `0x4002801C`）

`EPAUS[15:0]`：Pause time。

### 3.14 `R16_ETH_MAMXFL` — 最大接收包长度（地址 `0x4002801E`）

`MAMXFL[15:0]`：**硬件超过此长度即置 `ESTAT.RXMORE`**。

### 3.15 `R16_ETH_MIRD` — MII 读数据（地址 `0x40028020`，复位 `0x1100`）

读取经 SMI 自 PHY 读回的 16 位数据；MIRD 的复位值反映上电 BMSR/BMCR 状态。

### 3.16 `R8_ETH_MIREGADR` — MII 地址（地址 `0x40028024`）

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| `[7:5]` | Reserved | RO | — | 0 |
| `[4:0]` | `RB_ETH_MIREGADR_MIRDL` | RW | PHY 寄存器地址（**见 §6** 内部 10M PHY 表） | 0 |

### 3.17 `R8_ETH_MISTAT` — MII 状态（地址 `0x40028025`）

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| `[7:1]` | Reserved | RO | — | 0 |
| 0 | `R8_ETH_MII_STA` | RO | 0=读 MII；1=写 MII | 0 |

### 3.18 `R16_ETH_MIWR` — MII 写数据（地址 `0x40028026`，**WO**）

向 PHY 寄存器写 16 位数据。

### 3.19 `R32_ETH_MAADRL` / `R16_ETH_MAADRH` — MAC 地址（地址 `0x40028028`/`0x4002802C`）

- `MAADRL[31:0]`：字节 1–4；
- `MAADRH[15:0]`：字节 5–6；
- **上电值即 IEEE 为本芯片分配的唯一 MAC 地址**；用户通常直接使用，不需重写。

---

## 4. 操作流程（27.2.3）

### 4.1 初始化
1. **配置安全寄存器进入安全模式，打开以太网时钟与电源**（RCC/EXTEN）；
2. 开启所需 `R8_ETH_EIE.*IE`；可选 `R_EN50` 启动**内置 50 Ω** 阻抗；
3. 配置 `ERXFCON` 过滤、`MACON2.TXCRCEN` 等 CRC 功能；写入 `MAADRL/H` 或使用出厂值；
4. 设置 `ETXST`/`ERXST`（4 B 对齐）与 `MAMXFL`；
5. `RB_ETH_MACON1_MARXEN=1` + `ECON1.RXEN=1` → 启动接收并打开中断。

### 4.2 发送数据
1. 写 `R16_ETH_ETXLN` 数据长度；
2. 写 `R16_ETH_ETXST` 数据地址；
3. 置 `RB_ETH_ECON1_TXRTS=1` 启动发送（完成后硬件自动清零，并置 `EIR.TXIF`）。

### 4.3 接收数据
1. 预先设置 `ERXST` 接收地址、`MACON1.MARXEN=1` + `ECON1.RXEN=1`；
2. 通过中断（`EIR.RXIF`）或轮询 `ESTAT.RXBUSY` 判断接收完成；
3. 读 `R16_ETH_ERXLN` 获取长度；
4. 更新下一次 `ERXST`。

### 4.4 SMI 对 PHY 的读写（通用）
```
读：MIREGADR ← 目标 PHY 寄存器地址
    通过硬件时序从 MIRD 读取 16 位数据（MISTAT 位 0 = 0 表读）

写：MIREGADR ← 目标 PHY 寄存器地址
    MIWR ← 16 位写入值（MISTAT 位 0 = 1 表写）
```

> 具体应用请**基于以太网协议栈库**使用，参考 [`../CH32V20X/CH32V20xEVT/EVT/EXAM/ETH/`](../CH32V20X/CH32V20xEVT/EVT/EXAM/ETH/) 的官方示例；**没有提供完整的 DMA 描述符机制**（与 V307 不同，**本控制器使用单发送缓冲 + 单接收缓冲**模型，由 `ETXST/ETXLN` 与 `ERXST/ERXLN` 指示）。

---

## 5. 中断汇总

- NVIC/PFIC 入口：**`ETH_IRQn = 61`**（V20x_D8/D8W，见 `ch32v20xhw.h`）。
- 使能主开关 `EIE.INTIE`；总标志 `ESTAT.INT`；各事件标志在 `EIR`（RW1 清）。
- 错误/Buffer：`ESTAT` 的 `BUFER`（RW1 清）、`RXCRCER`/`RXNIBBLE`/`RXMORE`/`RXBUSY`/`TXABRT`（均 RO）。

---

## 6. 内部 10M PHY 寄存器（SMI 访问）

> 手册正文在 **27.1.8.5**，并在 27.2 节尾注明**「内部 10M 物理层相关寄存器内容详见 27.1.8.5」**，即 **V203 使用的 PHY 寄存器与 V307 的 10M PHY 完全相同**。

**通过 `MIREGADR + MIRD/MIWR` 间接访问**（PHY 内部偏移非 MMIO）：

| 寄存器 | PHY 偏移 | 复位值 | 作用 |
|--------|----------|--------|------|
| `BMCR` | `0x00` | `0x2100` | 基本控制：`RST`(15)、`Loopback`(14)、`Auto-Neg`(12)、`Restart AutoNeg`(9)、`Duplex Mode`(8)、`Collision Test`(7) |
| `BMSR` | `0x01` | `0x1809` | 基本状态：`Auto-Neg Complete`(5)、`Link`(2) |
| `R16_ETH_ANLPAR` | `0x05` | `0x0001` | 自动协商对端能力：`NP/ACK/RF/ASYPAUSE/PAUSE/100Base-T4/100Base-TX-FD/100Base-TX/10Base-T-FD/10Base-T/SELECT[4:0]` |
| `PHY_SR` | `0x10` | `0x0000` | 物理层状态：`Loopback_10M`(3)、`Full_10M`(2) |
| `PHY_MDIX` | `0x1E` | `0x0000` | 翻转：`[3:2] P/N`（00 正常 / 01 颠倒）、`[1:0] T/R`（00 自动 / 01 MDIX / 1x MDI） |

**位域详见** [CH32V307-ETH-寄存器详述.md 第 10 节](./CH32V307-ETH-寄存器详述.md)（两款芯片共用此段定义）。

---

## 7. 与头文件对应

`ch32v20xhw.h`（**必须启用** `CH32V20x_D8` 或 `CH32V20x_D8W`）：

```c
typedef struct {
    __IO uint8_t  reserved1;  // 0x00
    __IO uint8_t  reserved2;  // 0x01
    __IO uint8_t  reserved3;  // 0x02
    __IO uint8_t  EIE;        // 0x03  R8_ETH_EIE
    __IO uint8_t  EIR;        // 0x04
    __IO uint8_t  ESTAT;      // 0x05
    __IO uint8_t  ECON2;      // 0x06
    __IO uint8_t  ECON1;      // 0x07
    __IO uint16_t ETXST;      // 0x08
    __IO uint16_t ETXLN;      // 0x0A
    __IO uint16_t ERXST;      // 0x0C
    __IO uint16_t ERXLN;      // 0x0E
    __IO uint32_t HTL;        // 0x10
    __IO uint32_t HTH;        // 0x14
    __IO uint8_t  ERXFCON;    // 0x18
    __IO uint8_t  MACON1;     // 0x19
    __IO uint8_t  MACON2;     // 0x1A
    __IO uint8_t  MABBIPG;    // 0x1B
    __IO uint16_t EPAUS;      // 0x1C
    __IO uint16_t MAMXFL;     // 0x1E
    __IO uint16_t MIRD;       // 0x20
    __IO uint16_t reserved4;  // 0x22
    __IO uint8_t  MIERGADR;   // 0x24（即 MIREGADR）
    __IO uint8_t  MISTAT;     // 0x25
    __IO uint16_t MIWR;       // 0x26
    __IO uint32_t MAADRL;     // 0x28
    __IO uint16_t MAADRH;     // 0x2C
    __IO uint16_t reserved5;  // 0x2E
} ETH10M_TypeDef;

#define ETH10M_BASE   (AHBPERIPH_BASE + 0x8000)   // 0x40028000
#define ETH10M        ((ETH10M_TypeDef *) ETH10M_BASE)
```

**结构体首 3 字节保留** 正对应手册 `R8_ETH_EIE` 从 **`0x40028003`** 开始（**而非** `0x40028000`）。

### 额外配合
- `EXTEN` 寄存器中 **`EXTEN_ETH_10M_EN`** 需置 1 才能启用内置 10M PHY；CH32V208（带 BLE）等封装另有 **`EXTEN_ETH_RGMII_SEL`**（**不适用** CH32V203，详见官方 DS）。
- **RCC**：使能 **ETHEN / ETHTXEN / ETHRXEN**（若芯片定义），并确保 AHB 时钟不低于要求；**PLL3 60 MHz** 的要求主要针对 V307 的内置 10M PHY，**V203 的 10M PHY 由芯片内部时钟链给出**，以 DS 为准。

---

## 8. 操作示例（伪代码，出自 27.2.3 + 头文件）

```c
// 1. 时钟与 EXTEN
RCC->AHBPCENR |= RCC_ETHMACEN | RCC_ETHMACTXEN | RCC_ETHMACRXEN;
EXTEN->EXTEN_CTR |= EXTEN_ETH_10M_EN;

// 2. MAC 复位与 PHY 复位（略，通过 SMI 写 BMCR.RST）
ETH10M->ECON1 = RB_ETH_ECON1_TXRST | RB_ETH_ECON1_RXRST;
ETH10M->ECON1 = 0;

// 3. 过滤 / CRC / 全双工
ETH10M->ERXFCON = RB_ETH_ERXFCON_UCEN | RB_ETH_ERXFCON_EN |
                  RB_ETH_ERXFCON_BCEN | RB_ETH_ERXFCON_HTEN;
ETH10M->MACON1 = RB_ETH_MACON1_MARXEN;
ETH10M->MACON2 = (0b001 << 5) | RB_ETH_MACON2_TXCRCEN | RB_ETH_MACON2_FULDPX;
ETH10M->MABBIPG = 0x12;
ETH10M->MAMXFL = 1518;
ETH10M->ECON2  = (0b110 << 1);   // 必要保留位

// 4. 缓冲
ETH10M->ERXST = (uint16_t)(uint32_t)rx_buf;   // 4 字节对齐
ETH10M->ETXST = (uint16_t)(uint32_t)tx_buf;

// 5. 中断 + 启动
ETH10M->EIE = RB_ETH_EIE_INTIE | RB_ETH_EIE_RXIE | RB_ETH_EIE_TXIE |
              RB_ETH_EIE_LINKIE | RB_ETH_EIE_TXERIE | RB_ETH_EIE_RXERIE;
ETH10M->ECON1 |= RB_ETH_ECON1_RXEN;
NVIC_EnableIRQ(ETH_IRQn);

// 6. 发送：填 ETXLN + ETXST + TXRTS
ETH10M->ETXLN = tx_len;
ETH10M->ETXST = (uint16_t)(uint32_t)tx_buf;
ETH10M->ECON1 |= RB_ETH_ECON1_TXRTS;
```

> 请**始终参考官方例程** `CH32V20xEVT.ZIP → EVT/EXAM/ETH/*`（本仓 [`../CH32V20X/CH32V20xEVT/EVT/EXAM/ETH/`](../CH32V20X/CH32V20xEVT/EVT/EXAM/ETH/)），本文仅为**寄存器级位域参考**。

---

## 9. 交叉引用

- 手册原文（按页导出，包含 27.2 全节）：[`_layout_extract/ch27-eth_p429-492.txt`](./_layout_extract/ch27-eth_p429-492.txt) 第 **3338** 行起。
- CH32V307（**千兆 MAC+DMA+PTP**，完全不同）：[CH32V307-ETH-寄存器详述.md](./CH32V307-ETH-寄存器详述.md)。
- 第 27 章导航：[27-ETH-寄存器位域详述.md](./27-ETH-寄存器位域详述.md)。
- QEMU 侧 ETH10M 仿真：[../QingKe-MCU-QEMU使用手册.md](../QingKe-MCU-QEMU使用手册.md) 与 [`../../qemu-overlay/10.2.2/hw/riscv/ch32-eth-dwmac.c`](../../qemu-overlay/10.2.2/hw/riscv/ch32-eth-dwmac.c)。
