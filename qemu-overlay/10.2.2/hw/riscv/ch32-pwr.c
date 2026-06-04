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
 * File:     ch32-pwr.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 PWR（电源控制）仿真。
 *
 *     CTLR@+0: bit8=DBP 后备域写使能
 *     CSR @+4
 */

#include "ch32-machine-internal.h"

static uint64_t ch32_pwr_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v = 0;

    if ((addr & ~3u) == 0) {
        v = m->pwr_ctlr;
    } else if ((addr & ~3u) == 4) {
        v = m->pwr_csr;
    }
    if (size == 1) {
        return (v >> ((addr & 3u) * 8u)) & 0xffu;
    }
    if (size == 2) {
        return v & 0xffffu;
    }
    return v;
}

static void ch32_pwr_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Ch32MachineState *m = opaque;

    (void)size;
    if ((addr & ~3u) == 0) {
        m->pwr_ctlr = (uint32_t)val;
    } else if ((addr & ~3u) == 4) {
        m->pwr_csr = (uint32_t)val;
    }
}

const MemoryRegionOps ch32_pwr_ops = {
    .read = ch32_pwr_read,
    .write = ch32_pwr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
