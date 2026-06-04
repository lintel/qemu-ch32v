# CH32FV2x / V3x 寄存器详述（位域级）

本目录对《**CH32F/V20x_V30x_V31x 系列应用手册**》（V2.4，[`../CH32FV2x_V3xRM.PDF`](../CH32FV2x_V3xRM.PDF)）中常用外设做 **R32/R16/R8 命名、偏移、复位值、按位说明** 的整理，便于与 `ch32v20xhw.h` / `ch32v30xhw.h` 对照开发。

> **注意**：原文 PDF 为法律与技术上的最终依据。个别型号/批号限定（如 "仅 D8C"、"V20x_D6 批号"）在手册中极其详细，若下表与您的芯片型号不符，**以数据手册 + PDF 对应章为准**。

## 文档一览（按手册章号排序）

| 文档 | 手册章 | 内容提要 |
|------|--------|----------|
| [02-PWR-寄存器位域详述.md](./02-PWR-寄存器位域详述.md) | 第 2 章 | PWR 表 2-2：`CTLR`（RAM/VBAT 保持、`PVDE/PLS`、`DBP`、`PDDS/LPDS`）、`CSR`（`EWUP`、`PVDO/SBF/WUF`），低功耗模式一览表 2-1 |
| [03-RCC-寄存器位域详述.md](./03-RCC-寄存器位域详述.md) | 第 3 章 | `RCC_*`、`OSC`（HSE 校准/LSI32K）、表 3-1/3-2；`CFGR0/CFGR2` 全位与多型号 `PLLMUL` 编码 |
| [04-BKP-寄存器位域详述.md](./04-BKP-寄存器位域详述.md) | 第 4 章 | 表 4-1：`DATAR1..42`、`OCTLR` RTC 校准/TAMPER 脉冲、`TPCTLR/TPCSR` 侵入检测 |
| [05-CRC-寄存器位域详述.md](./05-CRC-寄存器位域详述.md) | 第 5 章 | 表 5-1：`DATAR/IDATAR/CTLR`，CRC32 多项式 `0x4C11DB7` |
| [06-RTC-寄存器位域详述.md](./06-RTC-寄存器位域详述.md) | 第 6 章 | 表 6-1：`CTLRH/L`、`PSCR`（重装 `PRL`）、`DIV`（分频余值）、`CNT`、`ALR`；配置模式 / 同步标志 |
| [07-08-IWDG-WWDG-寄存器位域详述.md](./07-08-IWDG-WWDG-寄存器位域详述.md) | 第 7/8 章 | IWDG 键值（`0x5555/0xAAAA/0xCCCC`） + WWDG `T/W/WDGTB/EWIF` 完整位域 |
| [09-EXTI-PFIC-STK-寄存器位域详述.md](./09-EXTI-PFIC-STK-寄存器位域详述.md) | 第 9 章 | `EXTI`（22 线）、`PFIC`（4×32 中断）、`STK`（青稞系统计数） |
| [10-GPIO-AFIO-寄存器位域详述.md](./10-GPIO-AFIO-寄存器位域详述.md) | 第 10 章 | GPIOA~E 寄存器；AFIO 表 10-48、`EXTICR1..4`、`PCFR1` **完整 18 位重映射表** + `PCFR2` |
| [11-DMA-寄存器位域详述.md](./11-DMA-寄存器位域详述.md) | 第 11 章 | 表 11-7/11-8、`INTFR/INTFCR/CFGR/CNTR` 位域；DMA2 扩展、请求映像表号见章内 |
| [12-ADC-寄存器位域详述.md](./12-ADC-寄存器位域详述.md) | 第 12 章 | 表 12-5/12-6 全表、`STATR/CTLR1/2`、序列表、`RDATAR`/`AUX` 等位域 |
| [13-TKEY-寄存器位域详述.md](./13-TKEY-寄存器位域详述.md) | 第 13 章 | 表 13-1/13-2：`TKEY1/2` 各自映射到 ADC1/ADC2 空间；`CHARGEx/CHGOFFSET/ACT_DCG/DR` |
| [14-ADTM-高级定时器-寄存器位域详述.md](./14-ADTM-高级定时器-寄存器位域详述.md) | 第 14 章 | 表 14-3..6：**TIM1/8/9/10** 完整地图 + 与 GPTM 差异（`RPTCR/BDTR/MOE/死区/刹车`） |
| [15-16-TIM-通用与基本定时器-寄存器概览.md](./15-16-TIM-通用与基本定时器-寄存器概览.md) | 第 15/16 章 | **TIM2..5** 表 15-3～6、TIM6/7 表 16-1/2 |
| [17-DAC-寄存器位域详述.md](./17-DAC-寄存器位域详述.md) | 第 17 章 | 表 17-2 全表、`CTLR` 双通道对称域、触发表 17-1 |
| [18-USART-寄存器位域详述.md](./18-USART-寄存器位域详述.md) | 第 18 章 | **USART1/2/3（全功能）+ UART4/5/6/7/8（仅异步）**；CH32V/F203C8 特例 UART4=USART4；8 个实例基址+中断号+RCC 位号；`STATR/DATAR/BRR/CTLR1/2/3/GPR/CTRL4` 完整位域；同步/LIN/IrDA/智能卡/半双工 互斥组合表 |
| [19-I2C-寄存器位域详述.md](./19-I2C-寄存器位域详述.md) | 第 19 章 | 表 19-1/19-2、`CTLR1/2` 与 `STAR/CKCFGR` |
| [20-SPI-I2S-寄存器位域详述.md](./20-SPI-I2S-寄存器位域详述.md) | 第 20 章 | 表 20-1～20-3、`CTLR1/2/STATR`、`I2S_CFGR/I2SPR/HSCR` |
| [21-22-23-USB-寄存器全表与位域说明.md](./21-22-23-USB-寄存器全表与位域说明.md) | 第 21～23 章 | USBD 表 21-5/6/7、USBHS 表 22-1/2/3、USBFS 表 23-1/3/5 |
| [24-CAN-寄存器位域详述.md](./24-CAN-寄存器位域详述.md) | 第 24 章 | 表 24-1..9 全部：主控/状态/位时序、邮箱（CAN1/2 独立）、**28 组过滤器 CAN1/2 共用** |
| [27-ETH-寄存器位域详述.md](./27-ETH-寄存器位域详述.md) | 第 27 章 | **章节导航**：27.1 = V307/V317/F207；27.2 = V203/V208/F208 |
| [CH32V307-ETH-寄存器详述.md](./CH32V307-ETH-寄存器详述.md) | 27.1 | **V307 / V317 / F207** 全功能 MAC+MMC+PTP+DMA + 描述符 |
| [CH32V203-ETH-寄存器详述.md](./CH32V203-ETH-寄存器详述.md) | 27.2 | **V203 / V208 / F208** 内置 10M MAC+PHY+简化 DMA |
| [29-RNG-寄存器位域详述.md](./29-RNG-寄存器位域详述.md) | 第 29 章 | 表 29-1：`CR/SR/DR`，PLL48CLK 驱动 LFSR |
| [30-OPA-寄存器位域详述.md](./30-OPA-寄存器位域详述.md) | 第 30 章 | 表 30-1：`OPA_CTLR` 4 组 OPA 对称 `EN/MODE/NSEL/PSEL` |
| [31-ESIG-寄存器位域详述.md](./31-ESIG-寄存器位域详述.md) | 第 31 章 | 表 31-1：`FLACAP` + 96 位 UID（`UNIID1..3`） |
| [32-FLASH-寄存器位域详述.md](./32-FLASH-寄存器位域详述.md) | 第 32 章 | 表 32-1..4：主存储器/选择字结构、标准/快速编程、读/写保护 |
| [33-EXTEN-寄存器位域详述.md](./33-EXTEN-寄存器位域详述.md) | 第 33 章 | 表 33-1：`CTR`（LDO、Lockup、USBD、ETH10M/RGMIION、HSIPRE）、`CTR2`（OPA 高速）、`FEATURE_SIGN`（VLEVEL） |
| [34-DBG-寄存器位域详述.md](./34-DBG-寄存器位域详述.md) | 第 34 章 | RISC-V CSR `0x7C0` + ARM `0xE0042004` `DBGMCU_CR`；`*_STOP`、`SLEEP/STOP/STANDBY`、`TRACE_*` |

---

## 尚未单独成文（细节较少的高带宽外设）

| 章 | 模块 | 说明 |
|----|------|------|
| 第 25 章 | **DVP** | 数字图像接口（V30x 等），位域以 PDF 原文为准 |
| 第 26 章 | **FSMC** | 可变静态存储控制器，寄存器区 **`0xA0000000` 段**；各 Bank 分寄存器表 |
| 第 28 章 | **SDIO** | SDIO 接口，见 PDF `28.6.x` 全表 |

上述章节可通过 `scripts/ch32-rm-pdftotext.sh` 按页导出原文速查。

---

## 以太网（两族分治）

- 导航页：**[27-ETH-寄存器位域详述](./27-ETH-寄存器位域详述.md)**
- **V307 / V317 / F207**：**[CH32V307-ETH-寄存器详述](./CH32V307-ETH-寄存器详述.md)**（27.1）
- **V203 / V208 / F208**：**[CH32V203-ETH-寄存器详述](./CH32V203-ETH-寄存器详述.md)**（27.2）
- **应用/描述符例程（V307）**：[../CH32V30X/CH32V307-ETH.md](../CH32V30X/CH32V307-ETH.md)

## 速查与配套资源

- 速查表 + 表号：[`../CH32FV2x_V3xRM-外设寄存器速查.md`](../CH32FV2x_V3xRM-外设寄存器速查.md)
- USB 专题：[21-22-23-USB-寄存器全表与位域说明.md](./21-22-23-USB-寄存器全表与位域说明.md) + [CH32V307-USB-Regs.md](../CH32V30X/CH32V307-USB-Regs.md)
- 按页 `pdftotext` 提取脚本：[`../../scripts/ch32-rm-pdftotext.sh`](../../scripts/ch32-rm-pdftotext.sh)
- 已按页导出的文本：[`./_layout_extract/`](./_layout_extract/)

## 与头文件 TypeDef 的对应

手册列 **绝对地址**（如 `0x40021000`）与 **R32_ 名**；SPL 式头文件通常为 "外设基址 + 成员"：例如 `R32_RCC_CTLR` ↔ `RCC->CTLR`，偏移 `0x00`。详见各章文首 "与头文件" 注。
