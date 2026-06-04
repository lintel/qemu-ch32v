# ESIG（电子签名）寄存器位域详述

**来源**：《CH32F/V20x_V30x_V31x 系列应用手册》V2.4（[`../CH32FV2x_V3xRM.PDF`](../CH32FV2x_V3xRM.PDF)），**第 31 章「电子签名（ESIG）」**（PDF **p520–521**）。  
**适用**：CH32F20x / V20x / V30x / V31x **全系列**。

---

## 1. 概述

**只读**出厂烧入的电子签名：
- **闪存容量**（`F_SIZE`）—— 用户可用 CODE 大小；
- **96 位唯一 UID**（`UNIID1..3`）—— 对每片芯片全球唯一，可作序列号/密码/密钥种子。

可经 **SWD / SDI**（调试接口）或**运行中应用代码**访问；支持 **8/16/32 位**任意读。

---

## 2. 表 31-1 寄存器（**地址位于系统存储区**）

| 名称 | 地址 | 描述 | 复位值 |
|------|------|------|--------|
| `R16_ESIG_FLACAP` | `0x1FFFF7E0` | 闪存容量（KB 单位） | `0xXXXX` |
| `R32_ESIG_UNIID1` | `0x1FFFF7E8` | UID[31:0] | `0xXXXXXXXX` |
| `R32_ESIG_UNIID2` | `0x1FFFF7EC` | UID[63:32] | `0xXXXXXXXX` |
| `R32_ESIG_UNIID3` | `0x1FFFF7F0` | UID[95:64] | `0xXXXXXXXX` |

> `0x1FFFF7E4` 未命名（可能为对齐保留），不应依赖。

---

## 3. 位域

### 3.1 `R16_ESIG_FLACAP`（`0x1FFFF7E0`）

| 位 | 名称 | 访问 | 描述 |
|----|------|------|------|
| `[15:0]` | **`F_SIZE[15:0]`** | RO | 闪存容量（**KB**）。例：`0x0080` = 128 KB |

### 3.2 `R32_ESIG_UNIID1/2/3`

| 名称 | 地址 | 含义 |
|------|------|------|
| `UNIID1` | `0x1FFFF7E8` | UID **bits[31:0]** |
| `UNIID2` | `0x1FFFF7EC` | UID **bits[63:32]** |
| `UNIID3` | `0x1FFFF7F0` | UID **bits[95:64]** |

---

## 4. 用法

```c
uint16_t flash_kb = *(volatile uint16_t*)0x1FFFF7E0;
uint32_t uid0 = *(volatile uint32_t*)0x1FFFF7E8;
uint32_t uid1 = *(volatile uint32_t*)0x1FFFF7EC;
uint32_t uid2 = *(volatile uint32_t*)0x1FFFF7F0;
```

> 建议使用 **`volatile`** 指针读取；**只读**，写入会触发总线错误或被忽略。
