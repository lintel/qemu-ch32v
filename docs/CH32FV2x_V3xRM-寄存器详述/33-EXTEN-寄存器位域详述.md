# EXTEN（扩展配置）寄存器位域详述

**来源**：《CH32F/V20x_V30x_V31x 系列应用手册》V2.4（[`../CH32FV2x_V3xRM.PDF`](../CH32FV2x_V3xRM.PDF)），**第 33 章「扩展配置（EXTEN）」**（PDF **p535–537**）。  
**适用**：CH32F20x / V20x / V30x / V31x **全系列**（逐位适用型号以 **芯片批号** 为准）。

---

## 1. 概述

`EXTEN` 是 WCH 在 HB 段上的扩展配置单元，含：
- **LDO / ULLDO 内核电压调整**；
- **HSI 分频到 PLL 的旁路**（配合 RCC 使用）；
- **Lock-up 监控**；
- **USBD 内置 1.5 kΩ 上拉/速度选择**；
- **ETH 10M 及 RGMII 模块启用开关**；
- **低功耗下 HSE 是否保持振荡**；
- **OPA1..4 高速模式**（`CTR2`）。

**只在系统复位执行复位**，读 `FEATURE_SIGN` 可获取 VDD 支持电压等级。

---

## 2. 表 33-1 寄存器

| 名称 | 地址 | 描述 | 复位 |
|------|------|------|------|
| `R32_EXTEN_CTR` | `0x40023800` | 扩展控制 | `0x00000A40` |
| `R32_EXTEN_CTR2` | `0x40023808` | 扩展控制 2（OPA 高速） | `0x00000000` |
| `R32_FEATURE_SIGN` | `0x1FFFF7D0` | 特征信息（**系统存储区**） | `0xE339XXXX` |

> **`0x40023804`** 上不是 `EXTEN`，而是 **`OPA_CTLR`**（第 30 章），与 EXTEN 同段相邻。

---

## 3. `R32_EXTEN_CTR`（`0x00`）— 扩展控制

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| `[31:13]` | Reserved | RO | — | 0 |
| 12 | **`HSEKPLP`** | RW | 低功耗模式下 HSE 振荡：`0`=不振 / `1`=保持振荡；**适用 V20x_D8/D8W、F20x_D8W** | 0 |
| `[11:10]` | **`LDOTRIM[1:0]`** | RW | LDO 电压：`00`=1.3V、`01`=1.2V、`10`=1.1V、`11`=1.0V | `10b` |
| `[9:8]` | **`ULLDOTRIM[1:0]`** | RW | 低功耗模式下 ULLDO 电压调整 | `10b` |
| 7 | **`LKUPRST`** | RW1 | Lockup 复位标志：写 1 清 | 0 |
| 6 | **`LKUPEN`** | RW | Lockup 监测使能（发生时系统复位并置 `LKUPRST`） | 1 |
| 5 | Reserved | RO | — | 0 |
| 4 | **`HSIPRE`** | RW | `0`=HSI÷2 进 PLL；`1`=HSI 直进 PLL（**PLL 关闭时才能写**） | 0 |
| 3 | **`RGMIION`** | RW | 千兆 ETH RGMII 接口 + 时钟使能（**F207 / V307**） | 0 |
| 2 | **`ETH10M`**（`ETH_10M_EN`）| RW | 10M ETH + 时钟使能（**F207/V307、V203、V208、F208**） | 0 |
| 1 | **`USBDPU`** | RW | USBD 内置 1.5 kΩ 上拉：`1`=启用（免外部电阻） | 0 |
| 0 | **`USBDLS`** | RW | USBD 速度：`0`=全速；`1`=低速 | 0 |

> **注**：历史文档中常见 `EXTEN_ETH_10M_EN`、`EXTEN_ETH_RGMII_SEL` 等宏名，对应**本表**的 `bit2 ETH10M` 与 `bit3 RGMIION`；仓库旧文 `EXTEN_ETH_RGMII_SEL` 应以 `RGMIION` 为准。

---

## 4. `R32_EXTEN_CTR2`（`0x08`）— OPA 高速

**适用**：**F20x_D8/D8C、V30x_D8/D8C、V31x_D8C 批号倒数第六位不为 0** 的产品。

| 位 | 名称 | 描述 |
|----|------|------|
| 3 | `OPA4_HSMD` | OPA4 高速 |
| 2 | `OPA3_HSMD` | OPA3 高速 |
| 1 | `OPA2_HSMD` | OPA2 高速 |
| 0 | `OPA1_HSMD` | OPA1 高速 |

与 [30-OPA](./30-OPA-寄存器位域详述.md) 的 `OPA_CTLR` 配合使用。

---

## 5. `R32_FEATURE_SIGN`（`0x1FFFF7D0`）— 特征信息

| 位 | 名称 | 描述 |
|----|------|------|
| `[31:16]` | Reserved | 复位值 `0xE339`（ID） |
| `[15:8]` | Reserved | `[7:0]` 的**按位取反** |
| `[7:1]` | Reserved | `0x7F` |
| 0 | **`VLEVEL`** | VDD 支持最低电压：`0`=1.8V / `1`=2.4V |

> **识别方法**：若 `bit[7:0]` 与 `bit[15:8]` **互为反码**，则 `VLEVEL` 有效；否则芯片默认支持 2.4V。`VLEVEL` 影响 PVD 阈值表（见 [02-PWR](./02-PWR-寄存器位域详述.md)）。

---

## 6. 典型用法

### 6.1 启用 USBD 全速 + 内置上拉

```c
EXTEND->EXTEN_CTR |= EXTEN_USBD_PU_EN;   // bit1
EXTEND->EXTEN_CTR &= ~EXTEN_USBD_LS;     // bit0 = 0 全速
```

### 6.2 启用 10M 内置 PHY

```c
EXTEND->EXTEN_CTR |= EXTEN_ETH_10M_EN;   // bit2
// + RCC 使能 ETHMACEN / ETHTXEN / ETHRXEN + 配置 PLL3 60MHz（V307）
```

### 6.3 HSI 不分频进 PLL

```c
// 先关 PLL
RCC->CTLR &= ~RCC_PLLON;
EXTEND->EXTEN_CTR |= EXTEN_HSIPRE;       // bit4 = 1
// 再开 PLL，SYSCLK 选 PLL
```
