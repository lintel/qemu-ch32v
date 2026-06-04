# USART / UART（通用同步异步收发器 / 通用异步收发器）寄存器位域详述

**来源**：《CH32F/V20x_V30x_V31x 系列应用手册》V2.4（[`../CH32FV2x_V3xRM.PDF`](../CH32FV2x_V3xRM.PDF)），**第 18 章「通用同步异步收发器（USART）」**（PDF **p256–271**）。  
**适用**：CH32F20x / V20x / V30x / V31x **全系列**。  
**按页导出**：[`_layout_extract/ch18-usart_p259-274.txt`](./_layout_extract/ch18-usart_p259-274.txt)。  
**头文件**：`USART_TypeDef` 按 16 位半字 + 填充至偏移 `0x1B`；`CTRL4`（+`0x1C`）在老版头文件中可能缺失。

---

## 1. USART / UART 功能分级（章首原文）

> 该模块包含 **3 个通用同步异步收发器（USART1/2/3）** 和 **5 个通用异步收发器（UART4/5/6/7/8）**。  
> **例外**：CH32V203C8、CH32F203C8 **串口 4 升级为同步异步收发器（USART4）**。

**硬件上两类模块共用一套寄存器定义**（手册的 `R32_USARTx_*` 命名覆盖 x=1..8 所有实例，差异体现在各功能"是否实际起作用"上）：

| 能力 | **USART1/2/3**（及 CH32V/F203C8 的 USART4） | **UART4/5/6/7/8**（非 C8） |
|------|---------------------------------------------|-----------------------------|
| **异步收发** | ✅ | ✅ |
| **同步主机 `CK` 输出** | ✅（`CTLR2.CLKEN/CPOL/CPHA/LBCL`） | ❌（CK 引脚未引出；`CLKEN` 无效） |
| **LIN（断开符检测/发送）** | ✅（`CTLR2.LINEN/LBDIE/LBDL`） | ✅ |
| **智能卡 ISO7816-3** | ✅（`CTLR3.SCEN/NACK` + `GPR.GT/PSC` + 需 `CLKEN`） | ❌（无 `CK`，`SCEN` 无效） |
| **IrDA 红外（NRZ + SIR）** | ✅（`CTLR3.IREN/IRLP` + `GPR.PSC`） | ✅ |
| **半双工（`TX` 单线）** | ✅（`CTLR3.HDSEL`） | ✅ |
| **硬件流控 `nCTS/nRTS`** | ✅（`CTLR3.CTSE/RTSE/CTSIE`、`STATR.CTS`） | ✅ |
| **DMA** | ✅（`CTLR3.DMAT/DMAR`） | ✅ |
| **多机地址唤醒** | ✅（`CTLR2.ADD` + `CTLR1.WAKE/RWU`） | ✅ |
| **`CTRL4` MARK/SPACE 校验** | ✅（**批号限定**：D8/D8C 等批号倒数第 6 位 ≠ 0） | ✅（同上批号） |
| **9 位数据位 `M=1`** | ✅ | ✅ |
| **5/6/7 位数据位 `M_EXT`** | ✅（**批号限定**） | ✅（同上） |

> **术语约定**：**从手册向量表（表 9-2）来看，所有 8 个串口中断都叫 `USART1..USART8`，不叫 `UART`**。官方头文件（`ch32v30xhw.h` 等）里亦统一为 `USARTx_TypeDef`，把 UART4-8 当作**精简 USART** 处理。软件层面访问方式一致，**只是 UART 下写 `CLKEN/SCEN` 等位不会实际生效**。

---

## 2. 时钟与波特率

### 2.1 挂载总线

- **USART1**：**APB2（PCLK2）** → `RCC_APB2PCENR.USART1EN(bit14)`；复位位 `APB2PRSTR.USART1RST(14)`；
- **USART2/3 + UART4/5（= USART4/5）**：**APB1（PCLK1）** → `RCC_APB1PCENR.USART2EN(17)/USART3EN(18)/USART4EN(19)/USART5EN(20)`；复位 `APB1PRSTR.*RST`；
- **UART6/7/8（= USART6/7/8）**：**APB1（PCLK1）** → `RCC_APB1PCENR.USART6EN(6)/USART7EN(7)/USART8EN(8)`（位号与 2-5 不连续）；复位位同寄存器。

### 2.2 波特率公式

$$
\text{Baud} = \frac{F_\text{PCLK}}{16 \times \text{USARTDIV}},\quad \text{USARTDIV} = \text{DIV\_Mantissa} + \frac{\text{DIV\_Fraction}}{16}
$$

- **`BRR[15:4] = DIV_Mantissa[11:0]`**、**`BRR[3:0] = DIV_Fraction[3:0]`**；
- 如 PCLK=12 MHz、目标 9600 bps → USARTDIV=78.125 → `BRR = (78<<4) | 2`（0.125×16=2）；
- 目标 115200 bps → USARTDIV=6.51（实际写 6.5，实得 ~115384 bps，误差 0.16%）；
- 模块容差**不低于 3%**（9 位或分数波特率会略降）。

---

## 3. 表 18-2 ~ 18-9 基址与寄存器窗口

**每个实例共 8 个 32 位寄存器**（物理宽 16 位，高 16 位保留）：

| 偏移 | 寄存器 | 典型复位值 |
|------|--------|-----------|
| `+0x00` | `R32_USARTx_STATR` | `0x000000C0`（`TXE=1, TC=1`） |
| `+0x04` | `R32_USARTx_DATAR` | `0x00000000` |
| `+0x08` | `R32_USARTx_BRR`   | `0x00000000` |
| `+0x0C` | `R32_USARTx_CTLR1` | `0x00000000` |
| `+0x10` | `R32_USARTx_CTLR2` | `0x00000000` |
| `+0x14` | `R32_USARTx_CTLR3` | `0x00000000` |
| `+0x18` | `R32_USARTx_GPR`   | `0x00000000` |
| `+0x1C` | `R32_USARTx_CTRL4` | `0x00000000`（**批号限定**） |

**基址（表 18-2 ~ 18-9）**：

| 实例 | 基址 | 总线 | 中断向量（RISC-V 表 9-2） | 功能分级 |
|------|------|------|--------------------------|----------|
| **USART1** | `0x40013800` | PCLK2 | `USART1` = **#53（编号 39）** | 全功能（含同步/智能卡/IrDA） |
| **USART2** | `0x40004400` | PCLK1 | `USART2` = **#54（40）** | 全功能 |
| **USART3** | `0x40004800` | PCLK1 | `USART3` = **#55（41）** | 全功能 |
| **UART4**（C8 上为 `USART4`） | `0x40004C00` | PCLK1 | `USART4` = **#68（54）** | 仅异步（C8 全功能） |
| **UART5** | `0x40005000` | PCLK1 | `USART5` = **#69（55）** | 仅异步 |
| **UART6** | `0x40001800` | PCLK1 | `USART6` = **#87（73）** | 仅异步 |
| **UART7** | `0x40001C00` | PCLK1 | `USART7` = **#88（74）** | 仅异步 |
| **UART8** | `0x40002000` | PCLK1 | `USART8` = **#89（75）** | 仅异步 |

> **手册打印"UASRT"** 是 WCH PDF 的排版笔误，实为 **USART**；以寄存器 `R32_USARTx_*` 和头文件中 `USARTx` 为准。

---

## 4. 中断与标志（表 18-1）

| 事件 | 状态位（`STATR`） | 使能位（`CTLR1/2/3/4`） | 清除方式 |
|------|-------------------|--------------------------|----------|
| 发送缓冲空 | `TXE`(7) | `TXEIE`(`CTLR1:7`) | **写 `DATAR`** |
| 发送完成 | `TC`(6)  | `TCIE`(`CTLR1:6`) | 读 `STATR` 再写 `DATAR`；或直接软件写 0 |
| 接收非空 | `RXNE`(5) | `RXNEIE`(`CTLR1:5`) | 读 `DATAR`；或软件写 0 |
| 总线空闲 | `IDLE`(4) | `IDLEIE`(`CTLR1:4`) | 读 `STATR` 再读 `DATAR`；**`RXNE` 未置前不会再次置位** |
| 奇偶错 | `PE`(0) | `PEIE`(`CTLR1:8`) | 读 `STATR` 再读 `DATAR`（需等 `RXNE=1`） |
| 帧/噪声/过载 | `FE`(1) / `NE`(2) / `ORE`(3) | `EIE`(`CTLR3:0`) + `DMAR`=1（多缓冲模式下才触发中断） | 读 `STATR` 再读 `DATAR` |
| LIN Break | `LBD`(8) | `LBDIE`(`CTLR2:6`) | **软件写 0** |
| CTS 变化 | `CTS`(9) | `CTSIE`(`CTLR3:10`) + `CTSE=1` | 软件写 0 |
| MARK/SPACE 校验错 | `MS_ERR`(11) | `MS_ERRIE`(`CTRL4:1`) | 读 `STATR` 再读 `DATAR`（**批号限定**） |

> **`ORE` 特性**：数据寄存器**值不丢失**，但**移位寄存器会被覆盖**；开 `EIE` 后在多缓冲（DMA）模式下产生中断。

---

## 5. 位域详述

### 5.1 `R32_USARTx_STATR`（`0x00`）

| 位 | 名称 | 访问 | 描述 | 复位 |
|----|------|------|------|------|
| `[31:12]` | Reserved | RO | — | 0 |
| 11 | **`MS_ERR`** | RO | MARK/SPACE 校验错（**批号限定**） | 0 |
| 10 | **`RX_BUSY`** | RO | 接收进行中（**批号限定**） | 0 |
| 9  | **`CTS`** | RW0 | `nCTS` 变化；需 `CTSE=1` | 0 |
| 8  | **`LBD`** | RW0 | LIN Break 检测 | 0 |
| 7  | **`TXE`** | RO | TDR → 移位寄存器空；**写 `DATAR` 清** | 1 |
| 6  | **`TC`** | RW0 | 发送完成（末帧 + `TXE=1`） | 1 |
| 5  | **`RXNE`** | RW0 | RDR 非空；**读 `DATAR` 清** | 0 |
| 4  | **`IDLE`** | RO | 总线空闲；读 `STATR` 再读 `DATAR` 清 | 0 |
| 3  | **`ORE`** | RO | 过载错误 | 0 |
| 2  | **`NE`** | RO | 噪声检测 | 0 |
| 1  | **`FE`** | RO | 帧错误 | 0 |
| 0  | **`PE`** | RO | 奇偶校验错（清前需等 `RXNE=1`） | 0 |

### 5.2 `R32_USARTx_DATAR`（`0x04`）

| 位 | 名称 | 访问 | 描述 |
|----|------|------|------|
| `[8:0]` | **`DR[8:0]`** | RW | **读** = RDR（接收字节）；**写** = TDR（发送字节）；**9 位模式** `CTLR1.M=1` 时用满 9 位 |

### 5.3 `R32_USARTx_BRR`（`0x08`）

| 位 | 名称 | 访问 | 描述 |
|----|------|------|------|
| `[15:4]` | **`DIV_Mantissa[11:0]`** | RW | USARTDIV 整数部分 |
| `[3:0]`  | **`DIV_Fraction[3:0]`** | RW | 小数部分（/16） |

### 5.4 `R32_USARTx_CTLR1`（`0x0C`）

| 位 | 名称 | 访问 | 描述 |
|----|------|------|------|
| `[15:14]` | **`M_EXT[1:0]`** | RW | **批号限定**：`00` 由 `M` 决定；`01`=7 位；`10`=6 位；`11`=5 位（仅 F20x_D8/D8C、V30x_D8/D8C、V31x_D8C 批号倒数第 6 位 ≠ 0） |
| 13 | **`UE`** | RW | USART 使能；清 0 后**当前字节发完才停** |
| 12 | **`M`** | RW | 字长：`0`=8 数据位；`1`=9 数据位 |
| 11 | **`WAKE`** | RW | 唤醒方式：`0`=总线空闲；`1`=地址标记 |
| 10 | **`PCE`** | RW | 校验使能 |
| 9  | **`PS`** | RW | `0`=偶；`1`=奇 |
| 8  | **`PEIE`** | RW | 奇偶错中断 |
| 7  | **`TXEIE`** | RW | 发送缓冲空中断 |
| 6  | **`TCIE`** | RW | 发送完成中断 |
| 5  | **`RXNEIE`** | RW | 接收非空 / ORE 中断 |
| 4  | **`IDLEIE`** | RW | 空闲中断 |
| 3  | **`TE`** | RW | 发送使能（置位后发一个**空闲帧**） |
| 2  | **`RE`** | RW | 接收使能（检测 RX 起始位） |
| 1  | **`RWU`** | RW | 接收唤醒：`1`=静默模式；**置 `RWU` 前必须已收过 1 字节** |
| 0  | **`SBK`** | RW | 发送帧断开符（10/11 位低电平）；断开帧停止位时硬件清 |

### 5.5 `R32_USARTx_CTLR2`（`0x10`）

| 位 | 名称 | 访问 | 描述 | UART4-8 |
|----|------|------|------|---------|
| 14 | **`LINEN`** | RW | LIN 模式使能（与 `SBK` 配合发 LIN 同步断开符） | ✅ |
| `[13:12]` | **`STOP[1:0]`** | RW | 停止位：`00`=1；`01`=0.5；`10`=2；`11`=1.5 | ✅ |
| 11 | **`CLKEN`** | RW | **同步模式**：使能 CK 引脚 | **❌**（无 CK，保留） |
| 10 | **`CPOL`** | RW | 同步 CK 空闲电平（`0`=低，`1`=高）；**`TE/RE` 未使能时才可改** | ❌ |
| 9  | **`CPHA`** | RW | 同步 CK 采样相位（`0`=第一沿，`1`=第二沿） | ❌ |
| 8  | **`LBCL`** | RW | `1`=最后位时钟不出 CK | ❌ |
| 6  | **`LBDIE`** | RW | LIN Break 检测中断 | ✅ |
| 5  | **`LBDL`** | RW | LIN 断开符长度：`0`=10 位；`1`=11 位 | ✅ |
| `[3:0]` | **`ADD[3:0]`** | RW | 多处理器本机地址（静默模式唤醒用） | ✅ |

### 5.6 `R32_USARTx_CTLR3`（`0x14`）

| 位 | 名称 | 访问 | 描述 | UART4-8 |
|----|------|------|------|---------|
| 10 | **`CTSIE`** | RW | CTS 变化中断 | ✅ |
| 9  | **`CTSE`** | RW | nCTS 硬件流控使能 | ✅ |
| 8  | **`RTSE`** | RW | nRTS 硬件流控使能 | ✅ |
| 7  | **`DMAT`** | RW | 发送 DMA 使能（`TXE` 触发） | ✅ |
| 6  | **`DMAR`** | RW | 接收 DMA 使能（`RXNE` 触发） | ✅ |
| 5  | **`SCEN`** | RW | **智能卡模式**使能 | **❌**（需 CK） |
| 4  | **`NACK`** | RW | 智能卡校验错时发 NACK | ❌ |
| 3  | **`HDSEL`** | RW | **单线半双工**（TX 单线，内部连 RX） | ✅ |
| 2  | **`IRLP`** | RW | IrDA 低功耗模式 | ✅ |
| 1  | **`IREN`** | RW | **IrDA 红外模式**使能 | ✅ |
| 0  | **`EIE`** | RW | 错误中断总使能（配合 `DMAR=1` 下对 `FE/NE/ORE` 产生中断） | ✅ |

> **互斥组合**（手册正文原话）：
> - **同步（`CLKEN=1`）** 需 **`SCEN=0` + `HDSEL=0` + `IREN=0`**；
> - **半双工（`HDSEL=1`）** 需 **`SCEN=0` + `CLKEN=0` + `IREN=0`**；
> - **智能卡（`SCEN=1`）** 需 **`LINEN=0` + `HDSEL=0` + `IREN=0`**（`CLKEN` **可开**输出 SC 时钟）；
> - **IrDA（`IREN=1`）** 需 **`LINEN=0` + `STOP=00` + `CLKEN=0` + `SCEN=0` + `HDSEL=0`**。

### 5.7 `R32_USARTx_GPR`（`0x18`）

| 位 | 名称 | 访问 | 描述 |
|----|------|------|------|
| `[15:8]` | **`GT[7:0]`** | RW | **智能卡保护时间**（单位=波特率时钟周期）；GT 过后才置 `TC` |
| `[7:0]` | **`PSC[7:0]`** | RW | **IrDA 低功耗**：PCLK ÷ `PSC` 作 SIR 时钟，**不能为 0**；**IrDA 普通模式**：**只能设 `1`**；**智能卡模式**：PCLK ÷ (`PSC[4:0]` × 2) 作 CK 时钟，不能为 0 |

### 5.8 `R32_USARTx_CTRL4`（`0x1C`，**批号限定**）

| 位 | 名称 | 访问 | 描述 |
|----|------|------|------|
| `[3:2]` | **`CHECK_SEL[1:0]`** | RW | `0x`=关；`10`=**MARK 校验**；`11`=**SPACE 校验** |
| 1 | **`MS_ERRIE`** | RW | MARK/SPACE 校验错中断使能 |
| 0 | Reserved | RO | — |

> **仅** CH32F20x_D8、F20x_D8C、V30x_D8、V30x_D8C、V31x_D8C **批号倒数第 6 位 ≠ 0** 的产品；其他批号上此寄存器保留，`STATR.MS_ERR` 与 `RX_BUSY` 同样不实现。

---

## 6. 与 RCC / AFIO / DMA 配套

### 6.1 RCC 位号（与第 3 章 `RCC` 对照）

- **APB2**：`USART1EN(14) / USART1RST(14)`；
- **APB1 主组**：`USART2EN(17) / USART3EN(18) / USART4EN(19) / USART5EN(20)`；
- **APB1 扩展**（D8C 等）：`USART6EN(6) / USART7EN(7) / USART8EN(8)`；

### 6.2 AFIO 重映射

- `AFIO_PCFR1.USART1_RM(bit2)` + `AFIO_PCFR2.USART1_RM1(bit26)` 组成 **USART1 2 位映射**；
- `USART2_RM(3)`、`USART3_RM[1:0](5:4)`；
- **UART4..UART8**：各自 1 位重映射位在 `PCFR2`（见 [10-GPIO-AFIO](./10-GPIO-AFIO-寄存器位域详述.md)）。

### 6.3 DMA 请求通道

- 见第 11 章表 11-2..11-6；例如 **USART1_TX = DMA1_CH4、USART1_RX = DMA1_CH5**；  
- UART4 等在 **DMA2** 上（具体映射随型号）。

---

## 7. 典型用法

### 7.1 普通异步（USART1 或任意 UART4-8）

```c
RCC->APB2PCENR |= RCC_USART1EN;
// GPIO: TX 推挽复用，RX 浮空输入
USART1->BRR = ((F_PCLK2 / 9600) & ~0xF) | ((F_PCLK2 / 9600) & 0xF);
USART1->CTLR1 = USART_CTLR1_TE | USART_CTLR1_RE | USART_CTLR1_UE;
// 发送：while (!(USART1->STATR & USART_TXE)); USART1->DATAR = c;
// 接收：while (!(USART1->STATR & USART_RXNE)); c = USART1->DATAR;
```

### 7.2 9600 8N1 带硬件流控（USART1/2/3 推荐）

```c
USART1->CTLR3 = USART_CTLR3_CTSE | USART_CTLR3_RTSE;  // 硬件流控
USART1->CTLR1 = USART_CTLR1_UE | TE | RE;
```

### 7.3 同步主机（仅 USART1/2/3 或 C8 USART4）

```c
// 要求: SCEN=0 HDSEL=0 IREN=0
USART1->CTLR2 = USART_CTLR2_CLKEN | USART_CTLR2_CPOL | USART_CTLR2_CPHA;
// 之后 TE=1 开始发送 + CK 输出
```

### 7.4 IrDA 红外（全系列可用，需要红外收发器）

```c
USART1->CTLR3 = USART_CTLR3_IREN;  // 禁止 LINEN/STOP/CLKEN/SCEN/HDSEL
USART1->GPR  = 1;                   // 普通 IrDA：PSC 必须=1
```

### 7.5 LIN 主从（全系列可用）

```c
USART1->CTLR2 = USART_CTLR2_LINEN | (0<<12); // STOP=1
USART1->CTLR1 |= USART_CTLR1_UE | TE | RE;
// 发送断开符：USART1->CTLR1 |= USART_CTLR1_SBK; (硬件自清)
```

---

## 8. `USART_TypeDef` 与头文件

`ch32v30xhw.h` 默认成员只到 `GPR`：

```c
typedef struct {
    __IO uint16_t STATR;    uint16_t _rsvd0;
    __IO uint16_t DATAR;    uint16_t _rsvd1;
    __IO uint16_t BRR;      uint16_t _rsvd2;
    __IO uint16_t CTLR1;    uint16_t _rsvd3;
    __IO uint16_t CTLR2;    uint16_t _rsvd4;
    __IO uint16_t CTLR3;    uint16_t _rsvd5;
    __IO uint16_t GPR;      uint16_t _rsvd6;
    // 老版本无 CTRL4；可自行扩展：
    // __IO uint16_t CTRL4; uint16_t _rsvd7;
} USART_TypeDef;
```

若需 `CTRL4`：

```c
#define USART_CTRL4(h)  (*(volatile uint16_t*)((uint32_t)(h) + 0x1CU))
```
