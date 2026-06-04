# DBG（调试支持）寄存器位域详述

**来源**：《CH32F/V20x_V30x_V31x 系列应用手册》V2.4（[`../CH32FV2x_V3xRM.PDF`](../CH32FV2x_V3xRM.PDF)），**第 34 章「调试支持（DBG）」**（PDF **p538–543**）。  
**适用**：**CH32V20x / V30x / V31x（RISC-V）** 使用 **`DBGMCU_CR` 作为 CSR `0x7C0`**；**CH32F20x（ARM）** 使用**内存映射 `0xE0042004`**。两者位定义大体一致，但 ARM 版多出 **`TRACE_MODE/TRACE_IOEN`**（SWO 跟踪）；RISC-V 版独有 **`CAN2_STOP (bit21)`** 的位号不同（见下表）。

---

## 1. RISC-V 调试 MCU 配置寄存器（34.2.1）

**CSR 地址**：`0x7C0`（`DBGMCU_CR`）。需**调试器**经 **SDI** 协议访问；应用代码无法直接读写。

| 位 | 名称 | 描述 |
|----|------|------|
| `[31:24]` | Reserved | — |
| 23 | **`TIM10_STOP`** | 调试状态下冻结 TIM10 计数器 |
| 22 | **`TIM9_STOP`**  | 冻结 TIM9 |
| 21 | **`CAN2_STOP`**  | 冻结 CAN2 接收 |
| 20 | **`CAN1_STOP`**  | 冻结 CAN1 接收 |
| 19 | **`TIM8_STOP`**  | 冻结 TIM8 |
| 18 | **`TIM7_STOP`**  | 冻结 TIM7 |
| 17 | **`TIM6_STOP`**  | 冻结 TIM6 |
| 16 | **`TIM5_STOP`**  | 冻结 TIM5 |
| 15 | **`TIM4_STOP`**  | 冻结 TIM4 |
| 14 | **`TIM3_STOP`**  | 冻结 TIM3 |
| 13 | **`TIM2_STOP`**  | 冻结 TIM2 |
| 12 | **`TIM1_STOP`**  | 冻结 TIM1 |
| 11 | **`I2C2_SMBUS_TIMEOUT`** | 冻结 I2C2 SMBus 超时 |
| 10 | **`I2C1_SMBUS_TIMEOUT`** | 冻结 I2C1 SMBus 超时 |
| 9  | **`WWDG_STOP`** | 冻结 WWDG 计数器 |
| 8  | **`IWDG_STOP`** | 冻结 IWDG 计数器 |
| `[7:3]` | Reserved | — |
| 2  | **`STANDBY`** | `1`=调试 Standby：FCLK/HCLK 开（由内部 RC 提供），退出方式为系统复位 |
| 1  | **`STOP`** | `1`=调试 Stop：FCLK/HCLK 开（由内部 RC），退出仍需重配时钟 |
| 0  | **`SLEEP`** | `1`=调试 Sleep：FCLK/HCLK 开（由原系统时钟） |

> 典型用法：调试 `IWDG / WWDG / TIMx` 时设置对应 `*_STOP=1`，防止**断点停机时看门狗超时**或**定时器异常**。

---

## 2. ARM 调试 MCU 配置寄存器（34.2.2，仅 CH32F20x）

**地址**：`0xE0042004`（MMIO）。

位与 RISC-V 版相近，但排布略有不同：

| 位 | 名称 | 说明 |
|----|------|------|
| 23 | `TIM10_STOP` | — |
| 22 | `TIM9_STOP` | — |
| 21 | `CAN2_STOP` | — |
| 20 | `TIM8_STOP` | — |
| 19 | `TIM7_STOP` | — |
| 18 | `TIM6_STOP` | — |
| 17 | `TIM5_STOP` | — |
| 16 | `I2C2_SMBUS_TIMEOUT` | — |
| 15 | `I2C1_SMBUS_TIMEOUT` | — |
| 14 | **`CAN1_STOP`** | 注意 **位号比 RISC-V 版更低** |
| 13 | `TIM4_STOP` | — |
| 12 | `TIM3_STOP` | — |
| 11 | `TIM2_STOP` | — |
| 10 | `TIM1_STOP` | — |
| 9  | `WWDG_STOP` | — |
| 8  | `IWDG_STOP` | — |
| `[7:6]` | **`TRACE_MODE[1:0]`** | 跟踪引脚模式（与 `TRACE_IOEN` 配合）：`00` 异步 / `01/10/11` 同步 + 数据长度 1/2/4 |
| 5  | **`TRACE_IOEN`** | `1`=分配跟踪引脚（ARM 的 TRACESWO/TRACEDATA） |
| `[4:3]` | Reserved | — |
| 2  | `STANDBY` | 同 RISC-V |
| 1  | `STOP` | 同 RISC-V |
| 0  | `SLEEP` | 同 RISC-V |

---

## 3. 提示

- 调试器端通常自动设置常用 `*_STOP` 位，**用户应用代码不必手动访问**；需要时通过调试器脚本或 OpenOCD / WCH-Link 配置。
- `DBG_CR` **不会被系统复位**（它受 **调试域**影响，独立于 MCU 复位），可在多次下载/复位间保留配置。
- **RISC-V CSR 0x7C0** 属于 **非标准调试 CSR**，普通 `csrr/csrw` 指令默认在用户模式下不可访问；需在**机器模式**或通过 **SDI 调试接口**操作。

---

## 4. 与其它章节

- 低功耗模式配合：[02-PWR](./02-PWR-寄存器位域详述.md)
- 看门狗冻结：[07-08-IWDG-WWDG](./07-08-IWDG-WWDG-寄存器位域详述.md)
- 定时器冻结：[14-ADTM](./14-ADTM-高级定时器-寄存器位域详述.md) / [15-16-TIM](./15-16-TIM-通用与基本定时器-寄存器概览.md)
