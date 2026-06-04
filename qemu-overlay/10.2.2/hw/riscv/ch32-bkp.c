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
 * File:     ch32-bkp.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 BKP（备份寄存器域）仿真。
 *
 *     布局（EVT ch32v30x.h BKP_TypeDef）：
 *       +0x04..+0x28: DATAR1~10  (16-bit, stride 4)
 *       +0x2C:        OCTLR
 *       +0x30..0x34:  TPCTLR/TPCSR
 *       +0x40..0xF0:  DATAR11~42 (16-bit, stride 4)
 *     QEMU 实现：内部索引化到 bkp_datar[0..41] 数组。
 */

#include "ch32-machine-internal.h"

void ch32_bkp_reset(Ch32MachineState *m)
{
    memset(m->bkp_datar, 0, sizeof(m->bkp_datar));
    m->bkp_octlr = 0;
    m->bkp_tpcsr = 0;
}

/*
 * BKP 寄存器地址 -> 数组索引 (0-based)。
 * DR1~10: 住在 +0x04..+0x28 (偏移 4,8,...,0x28)
 * DR11~42: 住在 +0x40..+0xF0 (偏移 0x40,0x44,...,0xF0)
 * 返回 -1 表示非 DATAR 偏移。
 */
static int ch32_bkp_addr_to_idx(hwaddr addr)
{
    if (addr >= 0x04 && addr <= 0x28 && ((addr - 4) & 3) == 0) {
        return (int)((addr - 4) / 4); /* 0..9 */
    }
    if (addr >= 0x40 && addr <= 0xF0 && ((addr - 0x40) & 3) == 0) {
        return 10 + (int)((addr - 0x40) / 4); /* 10..41 */
    }
    return -1;
}

static uint64_t ch32_bkp_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;
    int idx = ch32_bkp_addr_to_idx(addr & ~(hwaddr)3);

    if (idx >= 0 && idx < CH32_BKP_NREGS) {
        uint32_t v = m->bkp_datar[idx]; /* 16-bit 寄存器 */

        if (size == 1) {
            return (v >> ((addr & 3u) * 8u)) & 0xffu;
        }
        return v & 0xffffu;
    }
    if ((addr & ~3u) == 0x2c) {
        return m->bkp_octlr;
    }
    if ((addr & ~3u) == 0x30 || (addr & ~3u) == 0x34) {
        return m->bkp_tpcsr;
    }
    return 0;
}

static void ch32_bkp_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Ch32MachineState *m = opaque;
    int idx = ch32_bkp_addr_to_idx(addr & ~(hwaddr)3);

    (void)size;
    if (idx >= 0 && idx < CH32_BKP_NREGS) {
        m->bkp_datar[idx] = (uint16_t)(val & 0xffffu);
        return;
    }
    if ((addr & ~3u) == 0x2c) {
        m->bkp_octlr = (uint32_t)val;
        return;
    }
    if ((addr & ~3u) == 0x30 || (addr & ~3u) == 0x34) {
        m->bkp_tpcsr = (uint32_t)val;
    }
}

const MemoryRegionOps ch32_bkp_ops = {
    .read = ch32_bkp_read,
    .write = ch32_bkp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
