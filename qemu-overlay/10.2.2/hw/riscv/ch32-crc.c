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
 * File:     ch32-crc.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 CRC（循环冗余校验）计算单元仿真。
 *
 *     芯片内置 CRC-32：多项式 0x4C11DB7，初始值 0xFFFFFFFF。
 *     计算规则：逐个 32 位字 MSB-first，不反射输入也不反射输出，无最终异或。
 *     和 STM32 / CH32 硬件完全一致：对本示例数据期望结果 = 0x199AC3CA。
 */

#include "ch32-machine-internal.h"

/* CRC-32 单字迭代（按位，MSB 优先，不反射） */
static uint32_t ch32_crc32_word(uint32_t crc, uint32_t data)
{
    int i;

    crc ^= data;
    for (i = 0; i < 32; i++) {
        if (crc & 0x80000000u) {
            crc = (crc << 1) ^ 0x04C11DB7u;
        } else {
            crc <<= 1;
        }
    }
    return crc;
}

static uint64_t ch32_crc_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;

    switch (addr & ~3u) {
    case 0x00: /* DATAR：读出当前累积 CRC */
        return m->crc_dr;
    case 0x04: /* IDATAR：8 位独立缓冲 */
        return m->crc_idr;
    case 0x08: /* CTLR：RST 为只写，读回始终为 0 */
        return 0;
    default:
        return 0;
    }
}

static void ch32_crc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v32 = (uint32_t)val;

    switch (addr & ~3u) {
    case 0x00: /* DATAR 写入：将 32 位字送入 CRC 引擎 */
        /*
         * 硬件仅支持 32 位输入。固件通常将整个 u32 数组写入。
         * 不足 32 位的访问按常规补零处理。
         */
        m->crc_dr = ch32_crc32_word(m->crc_dr, v32);
        break;
    case 0x04: /* IDATAR：8 位可读写，程序用作标记用 */
        m->crc_idr = (uint8_t)v32;
        break;
    case 0x08: /* CTLR：RST（bit0）= 1 复位 CRC，硬件自动清零 */
        if (v32 & 1u) {
            m->crc_dr = 0xFFFFFFFFu;
        }
        break;
    default:
        break;
    }
}

const MemoryRegionOps ch32_crc_ops = {
    .read = ch32_crc_read,
    .write = ch32_crc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
