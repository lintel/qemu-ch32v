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
 * File:     ch32-rng.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 RNG（随机数发生器）仿真。
 *
 *     CTLR 使能位控制 DRDY；DATAR 返回混合熵源的伪随机数。
 */

#include "ch32-machine-internal.h"

static uint64_t ch32_rng_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;

    if (addr >= CH32_RNG_SIZE) {
        return 0;
    }
    if (addr == 0) {
        /* CTLR 读返回当前控制寄存器值 */
        return m->rng_ctlr;
    }
    if (addr == 4) {
        /* STATR.DRDY：RNGEN（bit2，RNG_CR_RNGEN=0x4）置位后即就绪 */
        return (m->rng_ctlr & 0x4u) ? 1u : 0u;
    }
    if (addr == 8) {
        /* DATAR：返回随机数
         * 混合三个熵源：
         *   1. 虚拟时钟的高 32 位（每次读 DATAR 时已经有 ns 级进展）
         *   2. g_random_int()：GLib PRNG，与时钟无相关，弥补时钟低位的可预测性
         *   3. rng_seq 自增序列号，防止同一 ns 内多次读返回相同值
         */
        uint64_t clk = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint32_t x = (uint32_t)(clk >> 17) ^
                     g_random_int() ^
                     m->rng_seq++;

        return (uint64_t)(x ? x : 0xA5A5A5A5u);
    }
    return 0;
}

static void ch32_rng_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Ch32MachineState *m = opaque;

    if (addr >= CH32_RNG_SIZE) {
        return;
    }
    if (addr == 0) {
        m->rng_ctlr = (uint32_t)val;
    }
}

const MemoryRegionOps ch32_rng_ops = {
    .read = ch32_rng_read,
    .write = ch32_rng_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
