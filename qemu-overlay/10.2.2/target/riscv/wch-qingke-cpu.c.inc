/*
 * WCH QingKe RISC-V CPU 型号定义（作为 cpu.c 的 #include 片段）
 *
 * 本文件为 target/riscv/cpu.c 中 `riscv_cpu_type_infos[]` 数组的一部分，
 * 通过 `#include "wch-qingke-cpu.c.inc"` 在 TARGET_RISCV32 块内展开。
 * 因此：
 *   1. 不要单独加入 meson.build（不是独立编译单元）。
 *   2. 宏（WCH_QINGKE_BASE_CFG 等）与 DEFINE_RISCV_CPU(...) 的位置、缩进
 *      必须与原 cpu.c 中的上下文一致，否则生成的数组初始化器会错位。
 *   3. 首尾不要加额外的数组元素分隔符；所有 `,` 均随 DEFINE_RISCV_CPU(...)
 *      展开产生。
 *
 * 拆出本文件的目的（见仓库架构评审建议 #4）：
 *   - 将 ~210 行 WCH 私有 CPU 定义与 QEMU 上游 cpu.c 物理解耦，
 *     显著降低后续 QEMU 版本升级（如 10.2.2 -> 11.x）的合并冲突面，
 *     cpu.c 的 overlay diff 从「215 行 WCH 内嵌块」压缩到单行 #include。
 *   - 新增/调整 QingKe 型号时，只改本文件一处，review 范围更聚焦。
 *
 * 覆盖范围：
 *   - 抽象基类：TYPE_RISCV_CPU_WCH_QINGKE_BASE
 *   - 公共宏  ：WCH_QINGKE_BASE_CFG / WCH_QINGKE_BEXT_CFG / WCH_QINGKE_FLOAT_CFG
 *   - 具体型号：V4F / V4A / V4B / V4C / V4J / V3A / V3B / V3C / V3F / V3V /
 *               V2A / V2C / V5F
 */

    /*
     * WCH QingKe 系列专用抽象基类（"wch-qingke-base"）。
     * 继承 BARE_CPU：无 MMU（mmu=false, max_satp_mode=MBARE）、有 PMP（pmp=true, regions=4）。
     * riscv_cpu_cfg_merge 对 BOOL_FIELD(mmu) 使用 |=，子类无法把父类 mmu=true 改回 false，
     * 因此必须在此类（而非 BASE32/DYNAMIC_CPU）设置 mmu=false，
     * 避免 QingKe 首条写 SRAM 走 Sv32 报 store access fault。
     * 所有 WCH QingKe V2/V3/V4/V5 型号均以此类为父类。
     */
    DEFINE_ABSTRACT_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_BASE, TYPE_RISCV_BARE_CPU,
        .priv_spec = PRIV_VERSION_LATEST,
        .cfg.mmu = false,
        .cfg.pmp = true,
        .cfg.pmp_regions = 4,
        .cfg.max_satp_mode = VM_1_10_MBARE,
        .misa_mxl_max = MXL_RV32,
    ),

/*
 * WCH QingKe CPU 定义辅助宏
 *
 * 消除 V3/V4 系列内重复的 ext_zicsr/ext_zifencei/ext_zca/ext_zcb 字段。
 * 新增就少一个型号时，只需拦截变属列表并展开 DEFINE_WCH_Vxx 宏即可。
 */

/** WCH_QINGKE_BASE_CFG - 所有 V2/V3/V4 共用的最小 ISA 基线扩展 */
#define WCH_QINGKE_BASE_CFG \
    .cfg.ext_zicsr    = true, \
    .cfg.ext_zifencei = true, \
    .cfg.ext_zca      = true, \
    .cfg.ext_zcb      = true, \
    .cfg.pmp_regions  = 4

/** WCH_QINGKE_BEXT_CFG - B bit 扩展组（Zba/Zbb/Zbc/Zbs） */
#define WCH_QINGKE_BEXT_CFG \
    .cfg.ext_zba = true, \
    .cfg.ext_zbb = true, \
    .cfg.ext_zbc = true, \
    .cfg.ext_zbs = true

/**
 * WCH_QINGKE_FLOAT_CFG - RV32F 配置（必须同时启用 Zcf）
 * RV32F + Zca => C.FSW / C.FLW 指令合法。
 */
#define WCH_QINGKE_FLOAT_CFG \
    .cfg.ext_zcf = true

/**
 * WCH_QINGKE_MARCHID / WCH_QINGKE_MIMPID - marchid / mimpid CSR 编码
 *
 * 依据《青稞 V4 微处理器手册》§8.2：
 *   marchid 全系列位域一致，差别仅在 Serial 与 Version 字段：
 *     bit 31        Reserved   = 1
 *     bit 30-26     Vender0 'W'= 0x17
 *     bit 25-21     Vender1 'C'= 0x03
 *     bit 20-16     Vender2 'H'= 0x08
 *     bit 15        Reserved   = 1
 *     bit 14-10     Arch    'V'= 0x16
 *     bit  9- 5     Serial     = 系列数字 2/3/4/5
 *     bit  4- 0     Version    = 版本字母 'A'=1,'B'=2,...,'V'=22 等
 *
 *   mimpid 全系列固定为 "WCH"：
 *     bit 31     = 1
 *     bit 30-16  = WCH
 *     bit 15     = 1
 *     bit 14- 1  = 0
 *     bit  0     = 1
 *   读出值 0xDC688001。
 *
 * 手册示例：V4F 读取 0xDC68D886，解码 "WCH-V4F"；
 * wlink 实测 V4B 读取 0xDC68D882，均与本定义吻合。
 */
#define WCH_QINGKE_MARCHID(_series_char, _version_char)                 \
    (0xDC68D800ULL |                                                    \
     ((uint64_t)((_series_char)  - '0') << 5) |                         \
     ((uint64_t)((_version_char) - 'A' + 1) & 0x1FULL))

#define WCH_QINGKE_MIMPID 0xDC688001ULL

/**
 * WCH_QINGKE_MVENDORID - mvendorid CSR 值
 *
 * 截止当前，WCH 未公开其 JEDEC 厂商编码 bank/offset，
 * 官方 SDK（core_riscv.c::__get_MVENDORID）仅读取该值但不做
 * 校验，wlink 探测也未使用 mvendorid。RISC-V Priv 规范 §3.1.1
 * 允许未实现的 mvendorid 返回 0。
 *
 * 在拿到权威值之前，此处显式置 0，表明“有意为空”：
 *   - 正确模拟 WCH 真机的默认行为（读出 0）；
 *   - 一旦获得 WCH 官方 JEDEC 分配或 mvendorid 编码规则，
 *     在本宏单点修改即可覆盖全系列 QingKe CPU。
 *
 * 当前与 QEMU 默认值等价，但显式赋值能够：
 *   - 传达“已核查”的编码意图（避免后人误读为“忘填”）；
 *   - 为未来注入真实厂商码预留单点钩子。
 */
#define WCH_QINGKE_MVENDORID 0x00000000ULL

/** 同时设置 mvendorid + marchid + mimpid 的快捷宏 */
#define WCH_QINGKE_IDS(_series_char, _version_char)                     \
    .cfg.mvendorid = WCH_QINGKE_MVENDORID,                              \
    .cfg.marchid = WCH_QINGKE_MARCHID(_series_char, _version_char),     \
    .cfg.mimpid  = WCH_QINGKE_MIMPID

/*
 * 芯片→内核对应参考（来源：reference-project/wlink/src/lib.rs::RiscvChip）
 *
 *   CH32V003 / CH641          → RISC-V2A
 *   CH32V103 / CH56X / CH57X  → RISC-V3A
 *   CH585 / CH584             → RISC-V3C  （wlink 0.1.0 新增）
 *   CH32V20X                  → RISC-V4B / V4C
 *   CH32V30X                  → RISC-V4C / V4F
 *   CH32V317                  → RISC-V4（实用 V4F）
 *   CH32V407 / CH32V467       → RISC-V3V  （含 Zve64x/Zvbb + B 扩展）
 *   CH582 / CH583 / CH581     → RISC-V4A
 *   CH59X                     → RISC-V4C
 *   CH643 / CH32X035 /
 *   CH32L103 / CH645 / CH653  → RISC-V4C
 *   CH564                     → RISC-V4J
 *   CH32H417                  → RISC-V5F
 *
 * 注：本仓库中 V3B 注释覆盖 CH32X033/X035/CH591/CH592（含 mcpy 指令），
 * 与 wlink 将 CH32X035 标注为 V4C 的语义存在差异；QEMU 侧以本仓库
 * patches/toolchain/0001-gcc-wch-qingke-all.patch 的 mcpy 分类为准。
 */

    /*
     * QingKe V4F：RV32IMACF + XW（单精度浮点），硬件堆栈 3 级，中断嵌套 8 级，
     * 内存保护区域 4（手册表 1-1）。CH32V307/CH32V317 等。
     * 特权模式：M + U（手册 §1.3）。
     */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V4F, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVA | RVF | RVC | RVU,
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_FLOAT_CFG,
        WCH_QINGKE_IDS('4', 'F'),    /* marchid = 0xDC68D886, mimpid = 0xDC688001 */
        .cfg.ext_xw = true,
        .cfg.ext_hpe = true,
    ),

    /*
     * QingKe V4A：RV32IMAC，无 XW，内存保护区域 4（手册表 1-1）。
     * CH32V458/CH581/CH582/CH583 等。特权模式：M + U（手册 §1.3）。
     */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V4A, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVA | RVC | RVU,
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_IDS('4', 'A'),    /* marchid = 0xDC68D881 */
        .cfg.ext_xw = false,
        .cfg.ext_hpe = true,
    ),

    /*
     * QingKe V4B：RV32IMAC + XW，内存保护区域 0（手册表 1-1：内存保护为 0）。
     * CH32V203 等。
     * 注：手册明确 V4B 无内存保护区域（MPU 保护区个数=0），pmp_regions=0 覆盖
     * WCH_QINGKE_BASE_CFG 中的 4。特权模式：M + U（手册 §1.3）。
     */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V4B, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVA | RVC | RVU,
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_IDS('4', 'B'),    /* marchid = 0xDC68D882，wlink 实测值 */
        .cfg.pmp_regions = 0,   /* V4B 无内存保护（手册表 1-1）*/
        .cfg.ext_xw  = true,
        .cfg.ext_hpe = true,
    ),

    /*
     * QingKe V4C：RV32IMAC + XW，内存保护区域 4（手册表 1-1）。
     * CH32V208/CH32V305/CH584/CH585/CH643/CH645 等。特权模式：M + U（手册 §1.3）。
     */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V4C, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVA | RVC | RVU,
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_IDS('4', 'C'),    /* marchid = 0xDC68D883 */
        .cfg.ext_xw  = true,
        .cfg.ext_hpe = true,
    ),

    /*
     * QingKe V4J：RV32IMAC + XW，内存保护区域 4，含 I-Cache（手册表 1-1）。
     * I-Cache 为微架构特性，QEMU 不模拟缓存，此处仅注释说明。
     * CH569/CH565 等。特权模式：M + U（手册 §1.3）。
     */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V4J, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVA | RVC | RVU,
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_IDS('4', 'J'),    /* marchid = 0xDC68D88A */
        .cfg.ext_xw  = true,
        .cfg.ext_hpe = true,
    ),

    /*
     * QingKe V3A：RV32IMAC，无 XW。
     * 手册表 1-1「内存保护区域个数」列为 ×，V3A 无 PMP。
     * pmp_regions=0 覆盖 WCH_QINGKE_BASE_CFG 中的 4（注：pmp BOOL_FIELD
     * 因 |= 合并无法由子类清除，pmp_regions=0 使 PMP 实际不可用）。
     * 特权模式：M + U（手册 §1.3）。
     */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V3A, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVA | RVC | RVU,
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_IDS('3', 'A'),    /* marchid = 0xDC68D861 */
        .cfg.pmp_regions = 0,   /* V3A 无 PMP（手册表 1-1）*/
        .cfg.ext_xw = false,
        .cfg.ext_hpe = true,
    ),

    /*
     * QingKe V3B：RV32I[M]C[B] + XW + mcpy（CH32X033/X035/CH591/CH592）。
     * 手册表 1-1：ISA 为 RV32I[M]C[B]（**无 A**），M 与 B 可选，部分料无 M 或无 B；
     * 通用配置默认打开 M + B（图片/手册原文：「其中部分芯片不支持 M、B 指令」）。
     * 「内存保护区域个数」列为 ×，V3B 无 PMP。特权模式：M + U（手册 §1.3）。
     */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V3B, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVC | RVB | RVU,   /* 手册：无 A；M/B 默认开，可选 */
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_BEXT_CFG,
        WCH_QINGKE_IDS('3', 'B'),    /* marchid = 0xDC68D862 */
        .cfg.pmp_regions = 0,          /* V3B 无 PMP（手册表 1-1）*/
        .cfg.ext_xw = true,
        .cfg.ext_xwchmcpy = true,      /* mcpy 硬件内存拷贝指令 */
        .cfg.ext_hpe = true,
    ),

    /*
     * QingKe V3C：RV32IMCB + XW + mcpy（手册表 1-1：**无 A**，有 B）。
     * 特权模式：M + U（手册 §1.3）。
     */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V3C, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVC | RVB | RVU,   /* 手册 RV32IMCB：无 A */
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_BEXT_CFG,
        WCH_QINGKE_IDS('3', 'C'),    /* marchid = 0xDC68D863 */
        .cfg.ext_xw = true,
        .cfg.ext_xwchmcpy = true,  /* mcpy 硬件内存拷贝指令 */
        .cfg.ext_hpe = true,
    ),

    /* QingKe V3F：RV32IMAFCB + XW + mcpy。特权模式：M + U（手册 §1.3）。 */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V3F, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVA | RVF | RVC | RVB | RVU,
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_FLOAT_CFG,
        WCH_QINGKE_BEXT_CFG,
        WCH_QINGKE_IDS('3', 'F'),    /* marchid = 0xDC68D866 */
        .cfg.ext_xw = true,
        .cfg.ext_xwchmcpy = true,  /* mcpy 硬件内存拷贝指令 */
        .cfg.ext_hpe = true,
    ),

    /*
     * QingKe V3V：RV32IMACB_Zve64x_zvbb + XW + mcpy（手册原文写法）。
     * Zve64x + Zvbb 为向量子集（非完整 V 扩展），内存保护区域 4。
     * 注：QEMU 尚未实现 Zve64x/Zvbb，此处暂不包含对应 misa 位/cfg。
     * 特权模式：M + U（手册 §1.3）。
     */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V3V, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVA | RVC | RVB | RVU,
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_BEXT_CFG,
        WCH_QINGKE_IDS('3', 'V'),    /* marchid = 0xDC68D876 */
        .cfg.ext_xw = true,
        .cfg.ext_xwchmcpy = true,  /* mcpy 硬件内存拷贝指令 */
        .cfg.ext_hpe = true,
    ),

    /* QingKe V2A：RV32EC + XW。特权模式：仅 M 模式（V2 手册 §1.3：无 PMP，MPP 恒为 0b11）。 */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V2A, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVE | RVC,  /* 无 RVU：V2 不支持用户模式 */
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_IDS('2', 'A'),    /* marchid = 0xDC68D841 */
        .cfg.ext_xw = true,
        .cfg.ext_hpe = true,
    ),

    /* QingKe V2C：RV32EC + Zmmul + XW。特权模式：仅 M 模式（V2 手册 §1.3：无 PMP，MPP 恒为 0b11）。 */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V2C, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVE | RVC,  /* 无 RVU：V2 不支持用户模式 */
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_IDS('2', 'C'),    /* marchid = 0xDC68D843 */
        .cfg.ext_zmmul = true,
        .cfg.ext_xw = true,
        .cfg.ext_hpe = true,
    ),

    /*
     * QingKe V5F：RV32IMAFC + B + Zba/Zbb/Zbc/Zbs + XW（见文档 §1.2）。
     * 特权模式：M + U（手册 §1.3）。
     */
    DEFINE_RISCV_CPU(TYPE_RISCV_CPU_WCH_QINGKE_V5F, TYPE_RISCV_CPU_WCH_QINGKE_BASE,
        .misa_mxl_max = MXL_RV32,
        .misa_ext = RVI | RVM | RVA | RVF | RVC | RVB | RVU,
        WCH_QINGKE_BASE_CFG,
        WCH_QINGKE_FLOAT_CFG,
        WCH_QINGKE_BEXT_CFG,
        WCH_QINGKE_IDS('5', 'F'),    /* marchid = 0xDC68D8A6 */
        .cfg.ext_xw = true,
        .cfg.ext_hpe = true,
    ),
