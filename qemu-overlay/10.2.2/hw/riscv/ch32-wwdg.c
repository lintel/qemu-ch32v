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
 * File:     ch32-wwdg.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 WWDG（窗口看门狗）仿真。
 *
 *     寄存器：CTLR@+0 / CFGR@+4 / STATR@+8
 *     模拟行为：
 *       CTLR 读始终返回写入值 | 0x40，防止固件 wwdg_tr < wwdg_wr 条件永不成立。
 *       CFGR/STATR shadow 读写。
 */

#include "ch32-machine-internal.h"

static uint64_t ch32_wwdg_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v;

    switch (addr & ~3u) {
    case 0: /* CTLR */
        /*
         * 始终返回写入的计数器 | 0x40（bit6常为1）。
         * EVT 主循环：wwdg_tr = WWDG->CTLR & 0x7F; if (wwdg_tr < wwdg_wr) Feed。
         * 由于我们不递减计数器，需确保 tr >= wr 来避免立即喂狗。
         * 嵌入 bit6 保证包括 window默认 0x5F 时， tr=0x7F >= wr。
         */
        v = m->wwdg_ctlr | 0x40u;
        break;
    case 4: /* CFGR */
        v = m->wwdg_cfgr;
        break;
    case 8: /* STATR */
        v = m->wwdg_statr;
        break;
    default:
        return 0;
    }
    if (size == 1) {
        return (v >> ((addr & 3u) * 8u)) & 0xffu;
    }
    if (size == 2) {
        return v & 0xffffu;
    }
    return v;
}

static void ch32_wwdg_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v = (uint32_t)val;

    (void)size;
    switch (addr & ~3u) {
    case 0: /* CTLR 写：更新计数器（模拟中不递减） */
        m->wwdg_ctlr = v & 0x7fu;
        break;
    case 4: /* CFGR */
        m->wwdg_cfgr = v;
        break;
    case 8: /* STATR：bit0=EWIF 写 0 清除 */
        if (!(v & 1u)) {
            m->wwdg_statr &= ~1u;
        }
        break;
    default:
        break;
    }
}

const MemoryRegionOps ch32_wwdg_ops = {
    .read = ch32_wwdg_read,
    .write = ch32_wwdg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
