/*
 * Copyright (c) 2026 Oray Inc. All rights reserved.
 * Copyright (C) 2022-2026 OraySZ / OrayOS-Team.
 *
 * This work is licensed under CC BY-NC-SA 4.0.
 * To view a copy of this license, visit:
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *
 * File:     ch32-v.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-20
 *
 * Description:
 *     WCH CH32 系列 minimal SoC（QingKe + 片内 Flash/SRAM）。外设实现已拆至
 *     ch32-*.c（Flash、RCC、STK/PFIC、RTC/RNG/ADC、IWDT、GPIO/AFIO、USART、以太网）。
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/tcg.h"
#include "hw/intc/riscv_aclint.h"
#include "target/riscv/cpu_bits.h"
#include "hw/qdev-core.h"
#include "hw/core/cpu.h"
#include "ch32-machine-internal.h"

#include <errno.h>

/*
 * SRAM / 外设总线背景 IO：0x20000000 片内 SRAM 与 0x40000000 外设总线
 * 之间的地址间隙，以及外设区未建模的地址带。部分固件会沿 SRAM 连续
 * 探测直至出现总线 fault；若整段 unmap，易在 FreeRTOS 队列边界处误伤。
 * 此处用 MMIO 占位（读 0 / 写忽略），使探测可穿过该带而不触发异常。
 * 注意：该 ops 同时被用作 periph_bus_ram 的背景 IO（见 machine_init）。
 *
 */
static uint64_t ch32_sram_gap_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)size;
    return 0;
}

static void ch32_sram_gap_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)val;
    (void)size;
}

static const MemoryRegionOps ch32_sram_gap_ops = {
    .read = ch32_sram_gap_read,
    .write = ch32_sram_gap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void ch32_mtimer_quiesce_mtip(void)
{
    hwaddr addr = CH32_QEMU_CLINT_BASE + RISCV_ACLINT_SWI_SIZE
                + RISCV_ACLINT_DEFAULT_MTIMECMP;
    uint64_t v = UINT64_MAX;
    MemTxResult r;

    r = address_space_write(&address_space_memory, addr,
                            MEMTXATTRS_UNSPECIFIED, &v, sizeof(v));
    if (r != MEMTX_OK) {
        error_report("ch32: could not clear ACLINT MTIP (mtimecmp write): %d",
                     (int)r);
        exit(EXIT_FAILURE);
    }
}

static void ch32_machine_instance_init(Object *obj)
{
    Ch32MachineState *s = CH32_MACHINE(obj);
    s->boot_mode = CH32_BOOT_FLASH; /* 默认：程序闪存存储器启动 */
}

static char *ch32_prop_get_flash_image(Object *obj, Error **errp)
{
    Ch32MachineState *s = CH32_MACHINE(obj);

    (void)errp;
    return g_strdup(s->flash_image ? s->flash_image : "");
}

static void ch32_prop_set_flash_image(Object *obj, const char *value, Error **errp)
{
    Ch32MachineState *s = CH32_MACHINE(obj);

    (void)errp;
    g_free(s->flash_image);
    s->flash_image = g_strdup(value ? value : "");
}

static char *ch32_prop_get_flash_size(Object *obj, Error **errp)
{
    Ch32MachineState *s = CH32_MACHINE(obj);

    (void)errp;
    return g_strdup_printf("%" PRIu64, s->flash_size_cfg);
}

static void ch32_prop_set_flash_size(Object *obj, const char *value, Error **errp)
{
    Ch32MachineState *s = CH32_MACHINE(obj);
    uint64_t v;

    if (!value || !value[0]) {
        s->flash_size_cfg = 0;
        return;
    }
    if (qemu_strtosz(value, NULL, &v) < 0) {
        error_setg(errp, "ch32: invalid flash-size: %s", value);
        return;
    }
    if (v < 4096) {
        error_setg(errp, "ch32: flash-size below minimum 4096");
        return;
    }
    if (v > 16 * MiB) {
        error_setg(errp, "ch32: flash-size above maximum 16MiB");
        return;
    }
    s->flash_size_cfg = v;
}

static char *ch32_prop_get_boot_mode(Object *obj, Error **errp)
{
    Ch32MachineState *s = CH32_MACHINE(obj);

    (void)errp;
    switch (s->boot_mode) {
    case CH32_BOOT_FLASH:  return g_strdup("flash");
    case CH32_BOOT_SYSMEM: return g_strdup("sysmem");
    case CH32_BOOT_SRAM:   return g_strdup("sram");
    default:               return g_strdup("flash");
    }
}

static void ch32_prop_set_boot_mode(Object *obj, const char *value, Error **errp)
{
    Ch32MachineState *s = CH32_MACHINE(obj);

    if (!value || !strcmp(value, "") || !strcmp(value, "flash")) {
        s->boot_mode = CH32_BOOT_FLASH;
    } else if (!strcmp(value, "sysmem") || !strcmp(value, "system")) {
        s->boot_mode = CH32_BOOT_SYSMEM;
    } else if (!strcmp(value, "sram")) {
        s->boot_mode = CH32_BOOT_SRAM;
    } else {
        error_setg(errp, "ch32: invalid boot-mode '%s': expected flash|sysmem|sram",
                   value);
    }
}

static void ch32_machine_finalize(Object *obj)
{
    Ch32MachineState *s = CH32_MACHINE(obj);

    if (s->stk_timer) {
        timer_free(s->stk_timer);
        s->stk_timer = NULL;
    }
    if (s->iwdt_timer) {
        timer_free(s->iwdt_timer);
        s->iwdt_timer = NULL;
    }
    if (s->iwdt_flag_timer) {
        timer_free(s->iwdt_flag_timer);
        s->iwdt_flag_timer = NULL;
    }
    ch32_usbhost_machine_finalize(s);
    ch32_usbfs_host_machine_finalize(s);
    g_free(s->flash_image);
    s->flash_image = NULL;
}

static void ch32_ahb_misc_reset_defaults(Ch32MachineState *s)
{
    void *p;

    if (CH32_AHB_MISC_SIZE < CH32_EXTEN_CTR2_OFF + 4) {
        return;
    }
    p = memory_region_get_ram_ptr(&s->ahb_misc_ram);
    stl_le_p((uint8_t *)p + CH32_EXTEN_CTR_OFF, CH32_EXTEN_CTR_RESET);
    stl_le_p((uint8_t *)p + CH32_EXTEN_CTR2_OFF, CH32_EXTEN_CTR2_RESET);
    ch32_usb_reset_defaults(s);
}

/*
 * 整机软状态复位：集中重置各外设 shadow / 标志 / 计数器等不持有 MMIO
 * 资源的状态，供 ch32_machine_reset（guest reset 钩子）与 ch32_machine_init
 * （首次初始化）共用，避免两处维护重复初始化代码。
 *
 * 不在此处处理的内容：
 *   - PFIC/STK/TIM/IWDG/GPIO/DMA/USART/ETH 等子系统的复位（有专用 reset
 *     函数，由调用方按需调用）
 *   - stk_timer/iwdt_timer 等 QEMUTimer 对象的创建（仅 init 负责）
 *   - MMIO region 注册（仅 init 负责）
 */
static void ch32_soft_state_reset(Ch32MachineState *s)
{
    /* RCC shadow：HSION=1 为上电默认 */
    memset(&s->rcc_shadow, 0, sizeof(s->rcc_shadow));
    s->rcc_shadow.words[0] = 0x00000001u;

    /* Flash 寄存器影子：LOCK=1 */
    memset(s->flash_reg_shadow, 0, sizeof(s->flash_reg_shadow));
    s->flash_reg_shadow[4] = 0x00000080u;
    s->flash_unlocked = false;
    s->flash_key_step = 0;
    s->flash_ob_key_step = 0;
    s->flash_statr = 0;

    /* RNG / CRC */
    s->rng_seq = 0;
    s->rng_ctlr = 0;
    s->crc_dr  = 0xFFFFFFFFu;
    s->crc_idr = 0;

    /* WWDG / PWR / BKP */
    s->wwdg_ctlr  = 0x7fu;
    s->wwdg_cfgr  = 0x7fu;
    s->wwdg_statr = 0;
    s->pwr_ctlr = 0;
    s->pwr_csr  = 0;
    ch32_bkp_reset(s);

    /* SDIO */
    memset(s->sdio_reg, 0, sizeof(s->sdio_reg));
    s->sdio_sta = 0;

    /* RTC 实时计数状态（shadow + 虚拟时钟 epoch） */
    memset(&s->rtc_shadow, 0, sizeof(s->rtc_shadow));
    s->rtc_cnt_set = false;
    s->rtc_cnt0    = 0;
    s->rtc_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->rtc_secf_ns = 0;
    s->rtc_alr     = 0;
}

/*
 * ESIG/DBGMCU IDCODE 填充与 Flash 首指 JAL 修正已拆至 ch32-boot.c：
 *   ch32_esig_populate（导出）
 *   ch32_decode_jal_target（导出）
 *   ch32_flash_patch_vector_jal_if_needed（导出）
 */

static void ch32_machine_reset(MachineState *machine, ResetType type)
{
    Ch32MachineState *s = CH32_MACHINE(machine);

    /*
     * QEMU guest reset 路径：qemu_system_reset -> machine->reset()。
     * ch32_machine_init 只在居机启动时执行一次，此处对每次 guest reset
     * （WDT 超时等）恢复外设软状态，防止 pfic_ienr/STK timer 残留导致
     * mscratch 未初始化时 SysTick 即刻重新触发。
     */

    /* 重置 PFIC 中断使能、STK 定时器、MTIP/MSIP */
    ch32_stk_pfic_reset(s);

    /* 重置 IWDG */
    ch32_iwdt_reset(s);

    /*
     * 通用软状态复位：RCC/Flash/RNG/CRC/WWDG/PWR/BKP/SDIO/RTC shadow。
     * 与 ch32_machine_init 共享同一段初始化逻辑，避免复制粘贴漂移。
     */
    ch32_soft_state_reset(s);

    /* 重置 GPIO/AFIO */
    ch32_gpio_afio_reset(s);

    /* 重置 DMA 控制器 */
    ch32_dma_reset(s);

    /* 重置 USART2/3 + UART4~8 轻量状态（含 RX FIFO / rx_pending） */
    ch32_usart_lite_reset(s);
    ch32_usart_lite_meip_resync(s);

    /* 重置 USART1 */
    ch32_usart1_reset(s);

    /* 重置 AHB MISC（EXTEN CTR 等）和 USB 默认值 */
    ch32_ahb_misc_reset_defaults(s);

    /* 重置 SPI/I2C */
    ch32_spi_reset(s);
    ch32_i2c_reset(s);

    /*
     * WCH CH32 mc->reset 覆盖了 qemu_devices_reset，所以 CPU reset（riscv_cpu_reset_hold）
     * 不会被自动调用。必须手动触发 **每个 hart** 的 cold reset，才能确保：
     * 1. env->pc 被重置为 resetvec（flash 起始地址）
     * 2. env->mtvec 被清零（避免旧向量在 startup 之前被意外使用）
     * 3. env->wch_vtf_in_isr / wch_hpe_depth / wch_hpe_sw_handler_skip 被清零
     * 4. mstatus.MIE 被清零
     *
     * 注意：device_cold_reset(DEVICE(&s->cpus)) **不会**级联到 harts[]，
     * 因为 QEMU 10.2.2 的 RISCVHartArrayState 没有把 harts 注册为 bus child
     * 或 Resettable child。这里用通用的 CPU_FOREACH + cpu_reset 遍历，而不再
     * reach-into s->cpus.harts[] 私有数组，对上游 11.x 潜在的 hart-array
     * 自己 register children 改造具备向前兼容性（上游如果级联了 harts，
     * cpu_reset 仍然是幂等的）。
     *
     * CPU reset 必须在 ch32_stk_pfic_reset（已撤销 MTIP/MSIP/mie）之后调用，
     * 否则 riscv_cpu_reset_hold 不会清零 mie，导致复位后 SysTick 立即重新触发。
     * SRAM 内容按真机行为不清零。
     */
    {
        CPUState *cs;
        CPU_FOREACH(cs) {
            cpu_reset(cs);
        }
    }

    /*
     * ETH DMA 影子状态复位：
     * mc->reset 覆盖了 qemu_devices_reset，ch32_eth_reset（注册为 dc->reset）
     * 不会自动被调用，需手动触发。否则 WDT/软件复位后 ETH DMA 描述符环游标
     * 残留旧值，新固件重建的 DMARDLAR/DMATDLAR 与旧游标不对齐，导致
     * ETH 无法接收数据包（ARP 不响应）。
     */
    if (s->eth_state) {
        device_cold_reset(DEVICE(s->eth_state));
    }
    if (s->eth10m_state) {
        device_cold_reset(DEVICE(s->eth10m_state));
    }

    /* 重置 TIM2~TIM7 */
    ch32_tim_reset(s);
}

static void ch32_machine_init(MachineState *machine, const Ch32BoardDesc *bd)
{
    Ch32MachineState *s = CH32_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    uint64_t elf_entry = CH32_FLASH_BASE;
    uint64_t reset_pc = CH32_FLASH_BASE;
    uint64_t low = 0, high = 0;
    ssize_t loaded;
    uint64_t flash_sz = s->flash_size_cfg ? s->flash_size_cfg : bd->flash_size;
    g_autofree char *flash_name = g_strdup_printf("%s.flash", bd->tag);

    /*
     * 绑定 board 描述指针，外设（如 RCC）可通过 s->board 读取
     * per-板的 HSI/HSE 频率等静态参数。
     */
    s->board = bd;

    {
        bool have_k = machine->kernel_filename && machine->kernel_filename[0];
        bool have_f = s->flash_image && s->flash_image[0];

        if (!have_k && !have_f) {
            error_report("%s: specify -kernel <file.elf> OR "
                         "-machine %s,flash-image=<file.bin|file.hex> "
                         "(raw binary or Intel HEX from 0x08000000, size <= flash)", bd->tag,
                         bd->tag);
            exit(EXIT_FAILURE);
        }
        if (have_k && have_f) {
            warn_report("%s: both -kernel and flash-image set; using -kernel",
                        bd->tag);
        }
    }

    memory_region_init_ram_nomigrate(&s->flash, OBJECT(machine), flash_name,
                                     flash_sz, &error_fatal);
    vmstate_register_ram_global(&s->flash);
    memory_region_add_subregion(sysmem, CH32_FLASH_BASE, &s->flash);

    /*
     * CH32 手册表 1-1：启动模式决定 0x00000000 映射到哪个区域。
     *
     * boot-mode=flash  (BOOT0=0, 默认)：
     *   0x00000000 映射到 Flash（0x08000000 内容）
     *   CPU resetvec = CH32_FLASH_BASE (0x08000000)
     *
     * boot-mode=sysmem (BOOT0=1, BOOT1=0)：
     *   0x00000000 映射到系统存储器（0x1FFF8000）
     *   CPU resetvec = CH32_SYSMEM_BASE (0x1FFF8000)
     *   系统存储器（BootLoader ROM）初始化为全 0xFF
     *
     * boot-mode=sram   (BOOT0=1, BOOT1=1)：
     *   0x00000000 仅能以 0x20000000 地址访问，不建立 0x00000000 映射
     *   CPU resetvec = CH32_SRAM_BASE (0x20000000)
     */
    {
        g_autofree char *sysmem_name =
            g_strdup_printf("%s.sysmem.rom", bd->tag);
        g_autofree char *boot_remap_name =
            g_strdup_printf("%s.boot.remap", bd->tag);
    
        /* 初始化系统存储器 ROM（全 0xFF）并注册到 0x1FFF8000 */
        memory_region_init_ram_nomigrate(&s->sysmem_rom, OBJECT(machine),
                                         sysmem_name, CH32_SYSMEM_SIZE,
                                         &error_fatal);
        vmstate_register_ram_global(&s->sysmem_rom);
        {
            void *sp = memory_region_get_ram_ptr(&s->sysmem_rom);
            memset(sp, 0xff, CH32_SYSMEM_SIZE);
        }
        memory_region_add_subregion(sysmem, CH32_SYSMEM_BASE, &s->sysmem_rom);
    
        switch (s->boot_mode) {
        case CH32_BOOT_FLASH:
        default:
            /*
             * 程序闪存存储器启动（默认）：
             * Flash 映射到 0x08000000，同时映射到 0x00000000。
             */
            memory_region_init_alias(&s->boot_remap, OBJECT(machine),
                                     boot_remap_name,
                                     &s->flash, 0, flash_sz);
            memory_region_add_subregion_overlap(sysmem,
                                                CH32_BOOT_REMAP_BASE,
                                                &s->boot_remap, -1);
            elf_entry = CH32_FLASH_BASE;
            break;
        case CH32_BOOT_SYSMEM:
            /*
             * 系统存储器启动：
             * 系统存储器映射到 0x1FFF8000，同时映射到 0x00000000。
             * CPU resetvec = 0x1FFF8000。
             */
            memory_region_init_alias(&s->boot_remap, OBJECT(machine),
                                     boot_remap_name,
                                     &s->sysmem_rom, 0, CH32_SYSMEM_SIZE);
            memory_region_add_subregion_overlap(sysmem,
                                                CH32_BOOT_REMAP_BASE,
                                                &s->boot_remap, -1);
            elf_entry = CH32_SYSMEM_BASE;
            break;
        case CH32_BOOT_SRAM:
            /*
             * 内部 SRAM 启动：
             * SRAM 已注册到 0x20000000，不建立 0x00000000 映射。
             * CPU resetvec = 0x20000000。
             */
            elf_entry = CH32_SRAM_BASE;
            break;
        }
    }
    
    memory_region_add_subregion(sysmem, CH32_SRAM_BASE, machine->ram);
    {
        hwaddr gap_base = CH32_SRAM_BASE + memory_region_size(machine->ram);
        hwaddr gap_len;

        if (gap_base < CH32_PERIPH_BUS_BASE) {
            gap_len = CH32_PERIPH_BUS_BASE - gap_base;
            memory_region_init_io(&s->sram_gap, OBJECT(machine), &ch32_sram_gap_ops,
                                   NULL, "ch32-sram-gap", gap_len);
            memory_region_add_subregion_overlap(sysmem, gap_base, &s->sram_gap, -1);
        }
    }

    {
        uint8_t *flash_ram = memory_region_get_ram_ptr(&s->flash);

        memset(flash_ram, 0xff, flash_sz);
    }

    if (machine->kernel_filename && machine->kernel_filename[0]) {
        loaded = load_elf_ram_sym(machine->kernel_filename,
                                    NULL, NULL, NULL,
                                    &elf_entry, &low, &high, NULL,
                                    ELFDATA2LSB, EM_RISCV,
                                    0, 0, &address_space_memory, false, NULL);
        if (loaded < 0) {
            error_report("%s: could not load ELF '%s'", bd->tag,
                         machine->kernel_filename);
            exit(EXIT_FAILURE);
        }
        /*
         * 确定复位 PC：
         *
         * 情形 A：ELF 链接到 Flash 物理地址（0x08000000 起）。
         *   elf_entry 在 [CH32_FLASH_BASE, CH32_FLASH_BASE+flash_sz) 内，
         *   且不等于基地址（避免无意义的 0x08000000 入口）。
         *   此时直接以 elf_entry 作为 resetvec。
         *
         * 情形 B：ELF 链接到 0x00000000（WCH MRS 工具链常见输出）。
         *   elf_entry 在 [0, flash_sz) 内，通过启动重映射区执行。
         *   若 elf_entry == 0，复位向量设为 0（启动重映射首地址）。
         *   若 elf_entry 非零（如 handle_reset 地址），直接使用该入口。
         *   关键：auipc 等 PC 相对指令必须在正确的地址空间（0x0000xxxx）
         *   下执行，否则 SP 等寄存器会被加上 0x08000000 偏移导致错误。
         *
         * 其他情形（elf_entry 在上述范围之外）：
         *   回退到 CH32_FLASH_BASE，行为与旧版一致。
         */
        if (elf_entry >= CH32_FLASH_BASE &&
            elf_entry < CH32_FLASH_BASE + flash_sz &&
            (elf_entry & 3) == 0 &&
            elf_entry != CH32_FLASH_BASE) {
            /* 情形 A：链接到 0x08000000 */
            reset_pc = elf_entry;
        } else if (elf_entry < flash_sz && (elf_entry & 3) == 0) {
            /* 情形 B：链接到 0x00000000，通过启动重映射执行 */
            reset_pc = elf_entry;   /* 启动重映射地址空间（0x0000xxxx） */
        } else {
            reset_pc = CH32_FLASH_BASE;
        }
        elf_entry = reset_pc;
    } else {
        uint8_t *flash_ram = memory_region_get_ram_ptr(&s->flash);
        bool is_hex = g_str_has_suffix(s->flash_image, ".hex") ||
                      g_str_has_suffix(s->flash_image, ".HEX");

        if (is_hex) {
            /*
             * Intel HEX 模式：解析 HEX 文件并按地址写入 Flash RAM。
             * 无需文件大小检查（解析器内部按地址窗口过滤）。
             */
            uint64_t hex_entry = CH32_FLASH_BASE;
            bool hex_have_ela = false;
            if (!ch32_load_hex_image(flash_ram, flash_sz, s->flash_image,
                                     &hex_entry, &hex_have_ela)) {
                error_report("%s: failed to load HEX '%s'", bd->tag, s->flash_image);
                exit(EXIT_FAILURE);
            }
            /*
             * WCH MRS 工具链生成的 HEX 通常链接到 0x00000000（无 ELA 记录）。
             * 此时代码中所有绝对地址均为 0x0000xxxx，CPU 应从启动重映射区
             * (0x00000000) 开始执行，而非 Flash 物理地址 (0x08000000)。
             * 若 HEX 含 ELA 记录（链接到 0x08000000），则保留原始入口地址。
             */
            if (!hex_have_ela) {
                elf_entry = CH32_BOOT_REMAP_BASE; /* 无 ELA：从 0 启动 */
            } else {
                elf_entry = hex_entry;
            }
        } else {
            GStatBuf sb;
            ssize_t n;

            if (g_stat(s->flash_image, &sb) != 0) {
                error_report("%s: flash-image '%s': %s", bd->tag, s->flash_image,
                             g_strerror(errno));
                exit(EXIT_FAILURE);
            }
            if ((uint64_t)sb.st_size > flash_sz) {
                error_report("%s: flash-image '%s' size %" PRIu64
                             " exceeds flash size %" PRIu64, bd->tag,
                             s->flash_image, (uint64_t)sb.st_size,
                             flash_sz);
                exit(EXIT_FAILURE);
            }
            n = load_image_size(s->flash_image, flash_ram, flash_sz);
            if (n < 0) {
                error_report("%s: could not load flash-image '%s'", bd->tag,
                             s->flash_image);
                exit(EXIT_FAILURE);
            }
            elf_entry = CH32_FLASH_BASE;
        }
    }

    {
        uint8_t *flash_ram = memory_region_get_ram_ptr(&s->flash);

        ch32_flash_patch_vector_jal_if_needed(flash_ram, flash_sz);
        /*
         * flash-image 首条常为「jal x0, 应用入口」（如前 4KiB 填 FF + 跳板），若仍从
         * reset PC 执行该 jal 会写入 ra，与 -kernel 直接把 resetvec 设为 ELF
         * 入口不同，易导致后续控制流异常。若首条为 jal x0 且目标在合理范围内，
         * 则将复位 PC 设为目标地址，与 -kernel 行为对齐。
         * 注：无 ELA 的 HEX（链接到 0x0）elf_entry=0，目标地址空间为 [0, flash_sz)；
         *     有 ELA 的 HEX/bin（链接到 0x08000000）elf_entry=0x08000000，目标在 Flash 物理范围内。
         */
        if (!(machine->kernel_filename && machine->kernel_filename[0])) {
            uint32_t w0 = ldl_le_p(flash_ram);

            if ((w0 & 0x7f) == 0x6f && ((w0 >> 7) & 0x1f) == 0) {
                uint32_t t = ch32_decode_jal_target(elf_entry, w0);
                bool t_valid;

                if (elf_entry == CH32_BOOT_REMAP_BASE) {
                    /* 无 ELA 模式：目标应在 [4, flash_sz) 且 4 字节对齐 */
                    t_valid = (t >= 4 && t < (uint32_t)flash_sz && (t & 3u) == 0u);
                } else {
                    /* 有 ELA / bin 模式：目标应在 Flash 物理范围内 */
                    t_valid = (t > CH32_FLASH_BASE && t < CH32_FLASH_BASE + flash_sz &&
                               (t & 3u) == 0u);
                }
                if (t_valid) {
                    elf_entry = t;
                }
            }
        }
    }

    object_initialize_child(OBJECT(machine), "cpus", &s->cpus,
                            TYPE_RISCV_HART_ARRAY);
    object_property_set_str(OBJECT(&s->cpus), "cpu-type",
                            machine->cpu_type, &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "num-harts", 1, &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "resetvec",
                            (int64_t)elf_entry, &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->cpus), &error_fatal);
    /*
     * USBFS 必须在 USBHS 之前初始化：qbus_init 用 QLIST_INSERT_HEAD 将
     * 子总线加入 parent，后创建的排在最前。用户 -device usb-xxx 不带 bus=
     * 时 QEMU 会选中第一个 USB 总线，USBHS 后创建所以排前面成为默认总线。
     */
    ch32_usbfs_host_machine_init(s);
    ch32_usbhost_machine_init(s);

    if (tcg_enabled()) {
        riscv_aclint_swi_create(CH32_QEMU_CLINT_BASE, 0, 1, false);
        riscv_aclint_mtimer_create(CH32_QEMU_CLINT_BASE + RISCV_ACLINT_SWI_SIZE,
                                   RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
                                   0, 1,
                                   RISCV_ACLINT_DEFAULT_MTIMECMP,
                                   RISCV_ACLINT_DEFAULT_MTIME,
                                   RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ, true);
        ch32_mtimer_quiesce_mtip();
    }

    ch32_usart1_init_mr(s, OBJECT(machine));
    memory_region_add_subregion_overlap(
        sysmem, CH32_USART1_BASE,
        &s->usart1.iomem, 2);

    /*
     * 通用软状态初始化：RCC/Flash/RNG/CRC/WWDG/PWR/BKP/SDIO/RTC shadow。
     * 与 ch32_machine_reset 共享同一段代码（ch32_soft_state_reset）。
     */
    ch32_soft_state_reset(s);

    /* PFIC / STK 模拟所需的独立软状态（不属于 soft_state_reset） */
    memset(&s->stk, 0, sizeof(s->stk));
    memset(s->pfic_ienr, 0, sizeof(s->pfic_ienr));
    s->stk_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->stk_cnt_at_epoch = 0;
    s->stk_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ch32_stk_tick, s);

    ch32_gpio_afio_reset(s);

    {
        g_autofree char *n = g_strdup_printf("%s.dbgmcu.ram", bd->tag);

        memory_region_init_ram_nomigrate(&s->dbg_ram, OBJECT(machine), n,
                                         CH32_DBGMCU_SIZE, &error_fatal);
        vmstate_register_ram_global(&s->dbg_ram);
        memory_region_add_subregion_overlap(sysmem, CH32_DBGMCU_BASE,
                                            &s->dbg_ram, 1);
    }

    ch32_afio_exti_init_mr(s, OBJECT(machine));
    memory_region_add_subregion_overlap(sysmem, CH32_AFIO_EXTI_BASE,
                                        &s->afio_exti_iomem, 2);

    ch32_gpio_init_mr(s, OBJECT(machine));
    memory_region_add_subregion_overlap(sysmem, CH32_GPIO_BLOCK_BASE,
                                        &s->gpio_iomem, 2);

    ch32_iwdt_init_mr(s, OBJECT(machine));
    memory_region_add_subregion_overlap(sysmem, CH32_IWDG_BASE,
                                        &s->iwdt_iomem, 3);

    /* TIM2~TIM7 MMIO 仿真（每个 1KB，优先级=2） */
    ch32_tim_init_all(s, OBJECT(machine));

    ch32_dma_init_mr(s, OBJECT(machine));
    memory_region_add_subregion_overlap(sysmem, CH32_DMA1_BLOCK_BASE,
                                        &s->dma.iomem, 1);
    {
        g_autofree char *n = g_strdup_printf("%s.ahb.crc-usbhs.ram", bd->tag);

        memory_region_init_ram_nomigrate(&s->ahb_misc_ram, OBJECT(machine), n,
                                         CH32_AHB_MISC_SIZE, &error_fatal);
        vmstate_register_ram_global(&s->ahb_misc_ram);
        memory_region_add_subregion_overlap(sysmem, CH32_AHB_MISC_BASE,
                                            &s->ahb_misc_ram, 1);
        ch32_ahb_misc_reset_defaults(s);
    }
    memory_region_init_io(&s->crc_iomem, OBJECT(machine), &ch32_crc_ops,
                          s, "ch32-crc", CH32_CRC_SIZE);
    memory_region_add_subregion_overlap(sysmem, CH32_CRC_BASE,
                                        &s->crc_iomem, 3);
    memory_region_init_io(&s->rng_iomem, OBJECT(machine), &ch32_rng_ops, s,
                          "ch32-rng-stub", CH32_RNG_SIZE);
    memory_region_add_subregion_overlap(sysmem, CH32_RNG_BASE, &s->rng_iomem, 3);
    ch32_usb_init_mr(s, OBJECT(machine));
    memory_region_add_subregion_overlap(sysmem, CH32_USBHS_BASE,
                                        &s->usbhs_iomem, 3);
    memory_region_add_subregion_overlap(sysmem, CH32_USBFS_BASE,
                                        &s->usbfs_iomem, 2);

    memory_region_init_io(&s->rcc_iomem, OBJECT(machine), &ch32_rcc_ops,
                          s, "ch32-rcc-stub", CH32_RCC_SIZE);
    memory_region_add_subregion_overlap(sysmem, CH32_RCC_BASE, &s->rcc_iomem, 1);

    memory_region_init_io(&s->rtc_iomem, OBJECT(machine), &ch32_rtc_ops, s,
                          "ch32-rtc-stub", CH32_RTC_SIZE);
    memory_region_add_subregion_overlap(sysmem, CH32_RTC_BASE, &s->rtc_iomem, 2);

    /* WWDG 窗口看门狗（地址 0x40002C00） */
    memory_region_init_io(&s->wwdg_iomem, OBJECT(machine), &ch32_wwdg_ops, s,
                          "ch32-wwdg", CH32_WWDG_SIZE);
    memory_region_add_subregion_overlap(sysmem, CH32_WWDG_BASE,
                                        &s->wwdg_iomem, 3);

    /* BKP 备份寄存器域（地址 0x40006C00） */
    memory_region_init_io(&s->bkp_iomem, OBJECT(machine), &ch32_bkp_ops, s,
                          "ch32-bkp", CH32_BKP_SIZE);
    memory_region_add_subregion_overlap(sysmem, CH32_BKP_BASE,
                                        &s->bkp_iomem, 2);

    /* PWR 电源控制（地址 0x40007000） */
    memory_region_init_io(&s->pwr_iomem, OBJECT(machine), &ch32_pwr_ops, s,
                          "ch32-pwr", CH32_PWR_SIZE);
    memory_region_add_subregion_overlap(sysmem, CH32_PWR_BASE,
                                        &s->pwr_iomem, 2);

    /* USART2/3 + UART4~8（轻量级仿真，DMA1/DMA2，手册表 18-2 ~ 18-9 基址） */
    ch32_usart_lite_init_mr(s, OBJECT(machine));
    memory_region_add_subregion_overlap(sysmem, CH32_USART2_BASE,
                                        &s->usart2.iomem, 2);
    memory_region_add_subregion_overlap(sysmem, CH32_USART3_BASE,
                                        &s->usart3.iomem, 2);
    memory_region_add_subregion_overlap(sysmem, CH32_UART6_BASE,
                                        &s->usart6.iomem, 2);
    memory_region_add_subregion_overlap(sysmem, CH32_UART7_BASE,
                                        &s->usart7.iomem, 2);
    memory_region_add_subregion_overlap(sysmem, CH32_UART8_BASE,
                                        &s->usart8.iomem, 2);
    memory_region_add_subregion_overlap(sysmem, CH32_UART4_BASE,
                                        &s->usart4.iomem, 2);
    memory_region_add_subregion_overlap(sysmem, CH32_UART5_BASE,
                                        &s->usart5.iomem, 2);

    /* SDIO stub（地址 0x40018000） */
    memory_region_init_io(&s->sdio_iomem, OBJECT(machine), &ch32_sdio_ops, s,
                          "ch32-sdio", CH32_SDIO_SIZE);
    memory_region_add_subregion_overlap(sysmem, CH32_SDIO_BASE,
                                        &s->sdio_iomem, 2);

    /* SPI1/SPI2/SPI3 stub */
    ch32_spi_init_mr(s, OBJECT(machine));
    memory_region_add_subregion_overlap(sysmem, 0x40013000ULL,
                                        &s->spi1_iomem, 2);
    memory_region_add_subregion_overlap(sysmem, 0x40003800ULL,
                                        &s->spi2_iomem, 2);
    memory_region_add_subregion_overlap(sysmem, 0x40003C00ULL,
                                        &s->spi3_iomem, 2);

    /* I2C1/I2C2 stub */
    ch32_i2c_init_mr(s, OBJECT(machine));
    memory_region_add_subregion_overlap(sysmem, 0x40005400ULL,
                                        &s->i2c1_iomem, 2);
    memory_region_add_subregion_overlap(sysmem, 0x40005800ULL,
                                        &s->i2c2_iomem, 2);

    /* RTC 实时计数状态由 ch32_soft_state_reset() 统一初始化 */

    /* 统一 PFIC IO handler：覆盖全部 0x1000 字节，包括 ISR/IPR/IENR/IRER/IPSR/IPRR/IACTR/IPRIOR/CFGR */
    memory_region_init_io(&s->pfic_io, OBJECT(machine), &ch32_pfic_ops, s,
                          "ch32-pfic", CH32_PFIC_SIZE);
    memory_region_add_subregion(sysmem, CH32_PFIC_BASE, &s->pfic_io);

    memory_region_init_io(&s->stk_iomem, OBJECT(machine), &ch32_stk_ops, s,
                          "ch32-stk", CH32_STK_SIZE);
    memory_region_add_subregion(sysmem, CH32_STK_BASE, &s->stk_iomem);

    {
        g_autofree char *esig_name = g_strdup_printf("%s.esig.rom", bd->tag);
        void *ep;

        memory_region_init_ram_nomigrate(&s->esig_rom, OBJECT(machine),
                                         esig_name, CH32_ESIG_SIZE, &error_fatal);
        vmstate_register_ram_global(&s->esig_rom);
        ep = memory_region_get_ram_ptr(&s->esig_rom);
        ch32_esig_populate(ep, CH32_ESIG_SIZE, bd, flash_sz);
        memory_region_set_readonly(&s->esig_rom, true);
        memory_region_add_subregion(sysmem, CH32_ESIG_BASE, &s->esig_rom);
    }

    memory_region_init_io(&s->flash_reg_iomem, OBJECT(machine), &ch32_flashreg_ops,
                          s, "ch32-flash-regs", CH32_FLASHREG_SIZE);
    memory_region_add_subregion_overlap(sysmem, CH32_FLASHREG_BASE,
                                        &s->flash_reg_iomem, 1);

    {
        g_autofree char *n = g_strdup_printf("%s.periph.bus.ram", bd->tag);

        /*
         * 外设总线背景 IO region：读返回 0，写忽略。
         * 之前用 RAM（memory_region_init_ram_nomigrate）会产生
         * TLB_NOTDIRTY 条目，导致 ETH MMIO（IO region， overlap
         * 优先级 10）被 RAM notdirty TLB 条目污染，最终
         * iotlb_to_section 断言失败。改用 IO region 后 QEMU
         * TCG TLB 对 IO region 使用普通 MMIO 路径，不
         * 会与高优先级 overlap MMIO 产生冲突。
         */
        memory_region_init_io(&s->periph_bus_ram, OBJECT(machine),
                              &ch32_sram_gap_ops, NULL, n,
                              CH32_PERIPH_BUS_SIZE);
        memory_region_add_subregion_overlap(sysmem, CH32_PERIPH_BUS_BASE,
                                            &s->periph_bus_ram, 0);
    }
    {
        static const hwaddr adc_bases[3] = {
            CH32_ADC1_BASE, CH32_ADC2_BASE, CH32_ADC3_BASE,
        };
        unsigned i;

        for (i = 0; i < 3; i++) {
            g_autofree char *an =
                g_strdup_printf("%s.adc%u.evt", bd->tag, i + 1u);

            memory_region_init_io(&s->adc_evt_iomem[i], OBJECT(machine),
                                  &ch32_adc_evt_ops, NULL, an,
                                  CH32_ADC_EVT_SIZE);
            memory_region_add_subregion_overlap(sysmem, adc_bases[i],
                                                &s->adc_evt_iomem[i], 4);
        }
    }

    if (bd->default_nic) {
        if (!strcmp(bd->default_nic, "ch32-eth-10m")) {
            /* CH32V20x_D8 ETH10M（10M MAC，与 EMAC+DMA 完全不同） */
            DeviceState *edev = qdev_new(TYPE_CH32_ETH_10M);

            qemu_configure_nic_device(edev, true, NULL);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(edev), &error_fatal);
            memory_region_add_subregion_overlap(
                sysmem, CH32_ETH_MMIO_BASE,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(edev), 0), 10);
            ch32_eth_10m_set_machine(edev, s);
        } else if (!strcmp(bd->default_nic, "ch32-eth-dwmac-10m")) {
            /*
             * CH32V307：标准 EMAC+DMA MAC 控制器（寄存器布局与 CH32V317 相同），
             * 内置 10M PHY（通过 EXTEN_CTR.ETH_10M_EN 使能）。设置 phy-model="phy10m"
             * 使 PHY 模拟返回 10M 默认值、ANLPAR=0 等。
             */
            DeviceState *edev = qdev_new(TYPE_CH32_ETH_DWMAC);

            qdev_prop_set_string(edev, "phy-model", "phy10m");
            qemu_configure_nic_device(edev, true, NULL);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(edev), &error_fatal);
            memory_region_add_subregion_overlap(
                sysmem, CH32_ETH_MMIO_BASE,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(edev), 0), 10);
            ch32_eth_set_machine(edev, s);
        } else if (!strcmp(bd->default_nic, "ch32-eth-dwmac-rtl8211f")) {
            /*
             * CH32V307/CH32V317（新默认）：标准 EMAC+DMA MAC 通过 RGMII 外接
             * Realtek RTL8211F 千兆 PHY。设置 phy-model="rtl8211f" 启用
             * RTL8211F Clause22/Page0xd04/MMD 寄存器模型；BCR 复位值 0x1140
             * 以兑现千兆 FD 自协商结果，PHYSR link-up 后返回 1000M FD 0x002C。
             */
            DeviceState *edev = qdev_new(TYPE_CH32_ETH_DWMAC);

            qdev_prop_set_string(edev, "phy-model", "rtl8211f");
            qemu_configure_nic_device(edev, true, NULL);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(edev), &error_fatal);
            memory_region_add_subregion_overlap(
                sysmem, CH32_ETH_MMIO_BASE,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(edev), 0), 10);
            ch32_eth_set_machine(edev, s);
        } else {
            /* CH32V30x / V317 等标准 EMAC+DMA ETH */
            DeviceState *edev = qdev_new(TYPE_CH32_ETH_DWMAC);

            qemu_configure_nic_device(edev, true, NULL);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(edev), &error_fatal);
            memory_region_add_subregion_overlap(
                sysmem, CH32_ETH_MMIO_BASE,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(edev), 0), 10);
            /*
             * 设置 ETH 外设反向指针，供 ch32_eth_meip_resync 通过 machine 指针
             * 访问 pfic_ienr 和 CPU env，实现 ETH_IRQn=77 中断路由。
             */
            ch32_eth_set_machine(edev, s);
        }
    }

    /*
     * 注册 VTF mret 回调：在 helper_mret 的 VTF 返回路径调用
     * ch32_pfic_on_mret，清除 IACTR/override/MEIP 并重扫挂起中断。
     */
    ch32_pfic_register_mret_cb(s);
}

#define CH32_M_STR(s) #s
#define CH32_MACHINE_ONE(mach, bdglobal)                                      \
    static void mach##_machine_init(MachineState *machine)                      \
    {                                                                           \
        ch32_machine_init(machine, &bdglobal);                                  \
    }                                                                           \
    static void mach##_machine_class_init(ObjectClass *oc, const void *data)  \
    {                                                                           \
        MachineClass *mc = MACHINE_CLASS(oc);                                   \
        const Ch32BoardDesc *bd = &bdglobal;                                    \
        (void)data;                                                             \
        mc->desc = bd->pretty;                                                   \
        mc->init = mach##_machine_init;                                         \
        mc->reset = ch32_machine_reset;                                         \
        mc->max_cpus = 1;                                                       \
        mc->default_cpu_type = bd->default_cpu_type;                            \
        mc->valid_cpu_types = bd->valid_cpu_types;                              \
        mc->default_ram_size = bd->default_ram_size;                            \
        mc->default_ram_id = bd->ram_id;                                        \
        if (bd->default_nic) {                                                  \
            mc->default_nic = bd->default_nic;                                 \
        }                                                                        \
        object_class_property_add_str(oc, "flash-image",                         \
                                      ch32_prop_get_flash_image,               \
                                      ch32_prop_set_flash_image);               \
        object_class_property_add_str(oc, "flash-size",                        \
                                      ch32_prop_get_flash_size,                \
                                      ch32_prop_set_flash_size);                \
        object_class_property_add_str(oc, "boot-mode",                          \
                                      ch32_prop_get_boot_mode,                 \
                                      ch32_prop_set_boot_mode);                \
    }                                                                           \
    static const TypeInfo mach##_machine_typeinfo = {                           \
        .name = MACHINE_TYPE_NAME(CH32_M_STR(mach)),                            \
        .parent = TYPE_MACHINE,                                                 \
        .class_init = mach##_machine_class_init,                                \
        .instance_size = sizeof(Ch32MachineState),                              \
        .instance_init = ch32_machine_instance_init,                            \
        .instance_finalize = ch32_machine_finalize,                             \
    };                                                                          \
    static void mach##_machine_register_types(void)                             \
    {                                                                           \
        type_register_static(&mach##_machine_typeinfo);                         \
    }                                                                           \
    type_init(mach##_machine_register_types)

CH32_MACHINE_ONE(ch32v317, ch32_board_ch32v317)
CH32_MACHINE_ONE(ch32v307, ch32_board_ch32v307)
CH32_MACHINE_ONE(ch32v305, ch32_board_ch32v305)
CH32_MACHINE_ONE(ch32v303, ch32_board_ch32v303)
CH32_MACHINE_ONE(ch32v203, ch32_board_ch32v203)
CH32_MACHINE_ONE(ch32v203rb, ch32_board_ch32v203rb)
CH32_MACHINE_ONE(ch32v103, ch32_board_ch32v103)
CH32_MACHINE_ONE(ch32v003, ch32_board_ch32v003)
CH32_MACHINE_ONE(ch32v407, ch32_board_ch32v407)
CH32_MACHINE_ONE(ch32h417, ch32_board_ch32h417)
