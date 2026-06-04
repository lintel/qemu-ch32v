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
 * File:     ch32-spi.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32V30x SPI1/SPI2/SPI3 MMIO stub。
 *
 *     仿真策略（回环模式）：
 *       - STATR.TXE  始终为 1（发送寄存器空，可立即写入）
 *       - STATR.RXNE 在写 DATAR 后置 1，读 DATAR 后清 0
 *       - DATAR 写操作将数据存入内部 rx_shadow，读操作返回 rx_shadow
 *       - 支持 8-bit / 16-bit 数据模式（由 CTLR1.DFF bit11 决定）
 *       - 不模拟 CRC、I2S、NSS、实际时钟时序
 *
 *     寄存器布局（每个寄存器 16-bit，后接 16-bit 保留）：
 *       0x00 CTLR1   0x04 CTLR2   0x08 STATR   0x0C DATAR
 *       0x10 CRCR    0x14 RCRCR   0x18 TCRCR   0x1C I2SCFGR
 *       0x20 I2SPR   0x24 HSCR
 */

#include "ch32-machine-internal.h"

#define CH32_SPI_REG_SIZE  0x28u

/* 寄存器偏移 */
#define SPI_CTLR1_OFF   0x00u
#define SPI_CTLR2_OFF   0x04u
#define SPI_STATR_OFF   0x08u
#define SPI_DATAR_OFF   0x0Cu
#define SPI_CRCR_OFF    0x10u
#define SPI_RCRCR_OFF   0x14u
#define SPI_TCRCR_OFF   0x18u
#define SPI_I2SCFGR_OFF 0x1Cu
#define SPI_I2SPR_OFF   0x20u
#define SPI_HSCR_OFF    0x24u

/* STATR 位 */
#define SPI_STATR_RXNE  0x0001u
#define SPI_STATR_TXE   0x0002u
#define SPI_STATR_BSY   0x0080u

/* CTLR1 位 */
#define SPI_CTLR1_SPE   0x0040u
#define SPI_CTLR1_DFF   0x0800u

static uint16_t ch32_spi_read_reg(Ch32SpiState *sp, hwaddr off)
{
    switch (off) {
    case SPI_CTLR1_OFF:   return sp->ctlr1;
    case SPI_CTLR2_OFF:   return sp->ctlr2;
    case SPI_STATR_OFF:
        {
            uint16_t v = sp->statr;
            v |= SPI_STATR_TXE;   /* TXE 始终为 1 */
            if (sp->rx_ne) {
                v |= SPI_STATR_RXNE;
            } else {
                v &= ~SPI_STATR_RXNE;
            }
            if ((sp->ctlr1 & SPI_CTLR1_SPE) == 0) {
                v &= ~SPI_STATR_BSY;
            }
            return v;
        }
    case SPI_DATAR_OFF:
        sp->rx_ne = false;
        sp->statr &= ~SPI_STATR_RXNE;
        return sp->rx_shadow;
    case SPI_CRCR_OFF:    return sp->crcr;
    case SPI_RCRCR_OFF:   return sp->rcrcr;
    case SPI_TCRCR_OFF:   return sp->tcrcr;
    case SPI_I2SCFGR_OFF: return sp->i2scfgr;
    case SPI_I2SPR_OFF:   return sp->i2spr;
    case SPI_HSCR_OFF:    return sp->hscr;
    default:              return 0;
    }
}

static void ch32_spi_write_reg(Ch32SpiState *sp, hwaddr off, uint16_t val)
{
    switch (off) {
    case SPI_CTLR1_OFF:
        sp->ctlr1 = val;
        break;
    case SPI_CTLR2_OFF:
        sp->ctlr2 = val;
        break;
    case SPI_STATR_OFF:
        if ((val & SPI_STATR_RXNE) == 0) {
            sp->rx_ne = false;
            sp->statr &= ~SPI_STATR_RXNE;
        }
        break;
    case SPI_DATAR_OFF:
        sp->datar = val;
        /* 回环：写入的数据立即到接收端 */
        sp->rx_shadow = val;
        sp->rx_ne = true;
        sp->statr |= SPI_STATR_RXNE;
        break;
    case SPI_CRCR_OFF:
        sp->crcr = val;
        break;
    case SPI_RCRCR_OFF:
        sp->rcrcr = val;
        break;
    case SPI_TCRCR_OFF:
        sp->tcrcr = val;
        break;
    case SPI_I2SCFGR_OFF:
        sp->i2scfgr = val;
        break;
    case SPI_I2SPR_OFF:
        sp->i2spr = val;
        break;
    case SPI_HSCR_OFF:
        sp->hscr = val;
        break;
    default:
        break;
    }
}

/*
 * read/write 回调：opaque 直接指向 Ch32SpiState 实例，
 * addr 是相对于 MemoryRegion 基址的偏移（0 ~ CH32_SPI_REG_SIZE-1）。
 */
static uint64_t ch32_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32SpiState *sp = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = addr + i;
        uint8_t b = 0;

        if (a < CH32_SPI_REG_SIZE) {
            /* 每个寄存器占 4 字节（16-bit 值 + 16-bit 保留），低 2 字节有效 */
            hwaddr reg_off = a & ~3u;        /* 对齐到 4 字节基地址 */
            unsigned byte_in_reg = a & 3u;   /* 0/1 有效，2/3 为保留 */
            if (byte_in_reg < 2u) {
                uint16_t reg = ch32_spi_read_reg(sp, reg_off);
                b = (uint8_t)(reg >> (8u * byte_in_reg));
            }
        }
        v |= (uint64_t)b << (8u * i);
    }
    return v;
}

static void ch32_spi_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Ch32SpiState *sp = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = addr + i;

        if (a < CH32_SPI_REG_SIZE) {
            hwaddr reg_off = a & ~3u;
            unsigned byte_in_reg = a & 3u;
            if (byte_in_reg < 2u) {
                /* 取出 16-bit 值：先读旧值，再替换对应字节 */
                uint16_t old = ch32_spi_read_reg(sp, reg_off);
                uint8_t  new_byte = (uint8_t)(val >> (8u * i));
                uint16_t merged;
                if (byte_in_reg == 0) {
                    merged = (old & 0xFF00u) | new_byte;
                } else {
                    merged = (old & 0x00FFu) | ((uint16_t)new_byte << 8u);
                }
                ch32_spi_write_reg(sp, reg_off, merged);
            }
        }
    }
}

const MemoryRegionOps ch32_spi_ops = {
    .read = ch32_spi_read,
    .write = ch32_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

void ch32_spi_reset(Ch32MachineState *m)
{
    unsigned i;

    for (i = 0; i < CH32_SPI_NUM; i++) {
        Ch32SpiState *sp = &m->spi[i];
        memset(sp, 0, sizeof(*sp));
        sp->ctlr1 = 0x0000;
        sp->ctlr2 = 0x0000;
        sp->statr = SPI_STATR_TXE;
        sp->crcr  = 0x0007;
    }
}

void ch32_spi_init_mr(Ch32MachineState *m, Object *owner)
{
    ch32_spi_reset(m);

    /* 每个实例传入独立的 Ch32SpiState* 作为 opaque */
    memory_region_init_io(&m->spi1_iomem, owner, &ch32_spi_ops, &m->spi[0],
                          "ch32-spi1", CH32_SPI_REG_SIZE);
    memory_region_init_io(&m->spi2_iomem, owner, &ch32_spi_ops, &m->spi[1],
                          "ch32-spi2", CH32_SPI_REG_SIZE);
    memory_region_init_io(&m->spi3_iomem, owner, &ch32_spi_ops, &m->spi[2],
                          "ch32-spi3", CH32_SPI_REG_SIZE);
}
