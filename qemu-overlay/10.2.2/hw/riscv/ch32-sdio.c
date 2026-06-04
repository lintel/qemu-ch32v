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
 * File:     ch32-sdio.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 SDIO stub（基址 0x40018000， IRQ 65）
 *
 *     最小可运行模拟：
 *       所有寄存器读: shadow 数组；STA 独立维护。
 *       CMD 写入且 CPSMEN（bit10）置位：自动置 STA.CMDSENT(bit7)；
 *         若命令有响应（!CMD0）同时置 STA.CMDREND(bit6)。
 *       RESP1（addr 0x14）返回 OCR=0x00FF8000（卡就绪）。
 *       ICR（addr 0x38）写入清对应 STA 标志。
 */

#include "ch32-machine-internal.h"

/* 寄存器偏移索引定义 */
#define SDIO_REG_POWER   0       /* +0x00: POWER   */
#define SDIO_REG_CLKCR   1       /* +0x04: CLKCR   */
#define SDIO_REG_ARG     2       /* +0x08: ARG     */
#define SDIO_REG_CMD     3       /* +0x0C: CMD     */
#define SDIO_REG_RESPCMD 4       /* +0x10: RESPCMD */
#define SDIO_REG_RESP1   5       /* +0x14: RESP1   */
#define SDIO_REG_RESP2   6       /* +0x18: RESP2   */
#define SDIO_REG_RESP3   7       /* +0x1C: RESP3   */
#define SDIO_REG_RESP4   8       /* +0x20: RESP4   */
#define SDIO_REG_DTIMER  9       /* +0x24: DTIMER  */
#define SDIO_REG_DLEN    10      /* +0x28: DLEN    */
#define SDIO_REG_DCTRL   11      /* +0x2C: DCTRL   */
#define SDIO_REG_DCOUNT  12      /* +0x30: DCOUNT  */
/* +0x34: STA (sdio_sta) */
#define SDIO_REG_ICR     14      /* +0x38: ICR     */
#define SDIO_REG_MASK    15      /* +0x3C: MASK    */

#define SDIO_STA_CMDREND  (1u << 6)  /* 命令响应已收到 */
#define SDIO_STA_CMDSENT  (1u << 7)  /* 命令已发送 */
#define SDIO_CMD_CPSMEN   (1u << 10) /* 命令路径状态机使能 */

static uint64_t ch32_sdio_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v;
    unsigned idx;

    if (addr >= CH32_SDIO_SIZE) {
        return 0;
    }
    /* STA 寄存器 特殊处理 */
    if ((addr & ~3u) == 0x34) {
        v = m->sdio_sta;
        goto out;
    }
    /* RESP1: 返回 OCR 表示卡就绪 */
    if ((addr & ~3u) == 0x14) {
        v = 0x00FF8000u;
        goto out;
    }
    idx = (unsigned)(addr >> 2);
    if (idx >= 20u) {
        return 0;
    }
    v = m->sdio_reg[idx];
out:
    if (size == 1) {
        return (v >> ((addr & 3u) * 8u)) & 0xffu;
    }
    if (size == 2) {
        return v & 0xffffu;
    }
    return v;
}

static void ch32_sdio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v = (uint32_t)val;
    unsigned idx;

    if (addr >= CH32_SDIO_SIZE) {
        return;
    }
    (void)size;

    /* ICR 写入清除 STA 对应标志位 */
    if ((addr & ~3u) == 0x38) {
        m->sdio_sta &= ~v;
        return;
    }

    /* CMD 写入：若 CPSMEN 置位，模拟命令已发送/已响应 */
    if ((addr & ~3u) == 0x0c) {
        m->sdio_reg[SDIO_REG_CMD] = v & ~(uint32_t)SDIO_CMD_CPSMEN;
        if (v & SDIO_CMD_CPSMEN) {
            uint32_t cmd_idx = v & 0x3fu;

            m->sdio_sta |= SDIO_STA_CMDSENT;
            if (cmd_idx != 0) {
                /* 有响应的命令：同时置 CMDREND */
                m->sdio_sta |= SDIO_STA_CMDREND;
                m->sdio_reg[SDIO_REG_RESPCMD] = cmd_idx;
            }
        }
        return;
    }

    idx = (unsigned)(addr >> 2);
    if (idx < 20u) {
        m->sdio_reg[idx] = v;
    }
}

const MemoryRegionOps ch32_sdio_ops = {
    .read = ch32_sdio_read,
    .write = ch32_sdio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
