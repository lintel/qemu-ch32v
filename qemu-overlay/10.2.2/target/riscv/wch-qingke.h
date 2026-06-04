/* (c) 2022-2026 OrayOS-Team. All rights reserved. */

/*
 * WCH QingKe helpers (HPE shadow stack + VTF dispatch helpers for TCG).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef RISCV_WCH_QINGKE_H
#define RISCV_WCH_QINGKE_H

#include "cpu.h"

/* =====================================================================
 * HPE（Hardware Prologue/Epilogue）shadow register bank
 * ===================================================================== */

/* Matches GCC riscv_wch_fast_interrupt_saved_reg (caller-saved + ra). */
static const int wch_hpe_regmap[16] = {
    1, 5, 6, 7,
    10, 11, 12, 13, 14, 15, 16, 17,
    28, 29, 30, 31,
};

static inline void wch_qingke_hpe_push(CPURISCVState *env, const RISCVCPUConfig *cfg)
{
    int i;

    if (!cfg->ext_hpe || env->wch_hpe_depth >= (int)ARRAY_SIZE(env->wch_hpe_bank)) {
        return;
    }
    for (i = 0; i < 16; i++) {
        int r = wch_hpe_regmap[i];

        env->wch_hpe_bank[env->wch_hpe_depth][i] = env->gpr[r];
    }
    env->wch_hpe_depth++;
}

static inline void wch_qingke_hpe_mret_pop(CPURISCVState *env, const RISCVCPUConfig *cfg)
{
    int i;

    /*
     * SW_Handler（MSIP/portYIELD 路径）自己管理所有寄存器，进入时未执行 HPE push，
     * mret 时也不应执行 HPE pop（否则会用旧任务寄存器覆盖新任务已恢复的寄存器）。
     */
    if (env->wch_hpe_sw_handler_skip) {
        env->wch_hpe_sw_handler_skip = false;
        return;
    }

    if (!cfg->ext_hpe || env->wch_hpe_depth == 0) {
        return;
    }
    env->wch_hpe_depth--;
    for (i = 0; i < 16; i++) {
        int r = wch_hpe_regmap[i];

        env->gpr[r] = env->wch_hpe_bank[env->wch_hpe_depth][i];
    }
}

/* =====================================================================
 * VTF（Vector Table Fast）模式辅助函数
 *
 * VTF 由 mtvec[1:0]==3 标识，仅在 ext_hpe CPU 上生效。
 * 所有 VTF 逻辑集中在此头文件，cpu_helper.c / op_helper.c 通过调用
 * 这些函数实现 WCH 特有的中断分发行为，无需直接访问字段。
 * ===================================================================== */

/**
 * wch_is_vtf_mode - 当前 CPU 是否处于 WCH VTF 模式
 * （ext_hpe 使能且 mtvec[1:0]==3）
 */
static inline bool wch_is_vtf_mode(const CPURISCVState *env,
                                    const RISCVCPUConfig *cfg)
{
    return cfg->ext_hpe && ((env->mtvec & 3) == 3);
}

/**
 * wch_vtf_irq_blocked - PFIC 同优先级不嵌套：VTF ISR 执行中阻止新异步中断
 *
 * 真实 WCH PFIC 复位后所有中断默认最低优先级（0 级），相同优先级中断不嵌套。
 * QEMU 尚未实现 PFIC 优先级仲裁，保守按「所有中断同优先级」处理。
 */
static inline bool wch_vtf_irq_blocked(const CPURISCVState *env,
                                        const RISCVCPUConfig *cfg)
{
    return wch_is_vtf_mode(env, cfg) && (env->wch_vtf_in_isr > 0);
}

/**
 * wch_sw_handler_irq_blocked - SW_Handler 执行中阻止所有中断嵌套
 *
 * SW_Handler（IRQ_M_SOFT / portYIELD）是 FreeRTOS 任务切换例程，
 * 自己管理所有寄存器和栈指针，不允许任何中断（MEIP/MTIP）嵌套进来。
 */
static inline bool wch_sw_handler_irq_blocked(const CPURISCVState *env,
                                               const RISCVCPUConfig *cfg)
{
    return wch_is_vtf_mode(env, cfg) && env->wch_hpe_sw_handler_skip;
}

/**
 * wch_vtf_enter - VTF 异步中断入口计数（非 SW_Handler 路径）
 *
 * 每次进入 VTF 异步 ISR（非 MSIP/SW_Handler）时调用，用于同优先级不嵌套检测。
 */
static inline void wch_vtf_enter(CPURISCVState *env)
{
    env->wch_vtf_in_isr++;
}

/**
 * wch_vtf_exit - VTF 异步中断退出计数（mret 时调用）
 *
 * 对应 wch_vtf_enter()，mret 时递减。计数归零后允许新的 VTF ISR 进入。
 */
static inline void wch_vtf_exit(CPURISCVState *env)
{
    if (env->wch_vtf_in_isr > 0) {
        env->wch_vtf_in_isr--;
    }
}

/**
 * wch_hpe_fix_fpu_on_entry - HPE 中断入口 FPU 状态修复
 *
 * 真实 WCH 硬件在中断入口不因 mstatus.FS==Disabled 拒绝浮点指令
 * （编译器生成的 ISR 序言使用 c.fsw/c.flw 等压栈）。
 * 若 FS 当前为 Disabled，将其置为 Initial(01)，避免 REQUIRE_FPU
 * 把这些指令视为非法指令触发 HardFault 无限递归。
 */
static inline void wch_hpe_fix_fpu_on_entry(CPURISCVState *env,
                                             const RISCVCPUConfig *cfg)
{
    if (cfg->ext_hpe && riscv_has_ext(env, RVF) &&
        !get_field(env->mstatus, MSTATUS_FS)) {
        env->mstatus = set_field(env->mstatus, MSTATUS_FS,
                                 EXT_STATUS_INITIAL);
    }
}

/**
 * WCH_EVT_HARDFAULT_WORD - WCH EVT 中同步异常统一路由到的字表索引
 *
 * WCH CH32 EVT .vector 字表中，HardFault_Handler 位于索引 3。
 * 同步异常（load/store fault、非法指令等）统一送此索引，
 * 与 startup_ch32v30x_* 中的向量表布局一致。
 */
#define WCH_EVT_HARDFAULT_WORD  3u

/**
 * wch_vtf_async_vec_word - 异步中断在 EVT .vector 字表中的索引
 *
 * VTF 模式下，异步中断入口地址从 EVT 字表按 NVIC 编号索引取得，
 * 而非 RISC-V 标准 mcause 偏移。映射规则：
 *   IRQ_M_TIMER (7)  → 字索引 12  (SysTick_IRQn)
 *   IRQ_M_SOFT  (3)  → 字索引 14  (Software_IRQn / portYIELD)
 *   IRQ_M_EXT   (11) → wch_evt_mcause_override（外设 NVIC 编号），
 *                       无 override 时退化为 cause 值
 */
static inline uint32_t wch_vtf_async_vec_word(CPURISCVState *env,
                                               target_ulong cause)
{
    if (cause == IRQ_M_TIMER) {
        return 12u;
    }
    if (cause == IRQ_M_SOFT) {
        return 14u;
    }
    if (cause == IRQ_M_EXT && env->wch_evt_mcause_override) {
        return env->wch_evt_mcause_override;
    }
    return (uint32_t)cause;
}

#endif /* RISCV_WCH_QINGKE_H */
