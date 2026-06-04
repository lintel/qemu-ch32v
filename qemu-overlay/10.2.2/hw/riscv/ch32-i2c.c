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
 * File:     ch32-i2c.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32V30x I2C1/I2C2 MMIO stub。
 *
 *     仿真策略（简化 Master 发送/接收）：
 *       - 模拟 I2C 主模式的基本状态机，使 EVT 示例能通过轮询通过
 *       - GenerateSTART -> SB=1, BUSY=1, MSL=1
 *       - Send7bitAddress -> SB=0, ADDR=1, TXE=1, TRA=1
 *       - SendData -> TXE=1, BTF=1（即时完成）
 *       - GenerateSTOP -> STOPF=1, BUSY=0, MSL=0
 *       - 不模拟真实总线时序、仲裁、PEC、SMBus
 *
 *     寄存器布局（每个 16-bit，后接 16-bit 保留）：
 *       0x00 CTLR1   0x04 CTLR2   0x08 OADDR1  0x0C OADDR2
 *       0x10 DATAR   0x14 STAR1   0x18 STAR2   0x1C CKCFGR
 *       0x20 RTR
 */

#include "ch32-machine-internal.h"

#define CH32_I2C_REG_SIZE  0x24u

/* 寄存器偏移 */
#define I2C_CTLR1_OFF   0x00u
#define I2C_CTLR2_OFF   0x04u
#define I2C_OADDR1_OFF  0x08u
#define I2C_OADDR2_OFF  0x0Cu
#define I2C_DATAR_OFF   0x10u
#define I2C_STAR1_OFF   0x14u
#define I2C_STAR2_OFF   0x18u
#define I2C_CKCFGR_OFF  0x1Cu
#define I2C_RTR_OFF     0x20u

/* CTLR1 位 */
#define I2C_CTLR1_PE    0x0001u
#define I2C_CTLR1_START 0x0100u
#define I2C_CTLR1_STOP  0x0200u
#define I2C_CTLR1_ACK   0x0400u

/* STAR1 位 */
#define I2C_STAR1_SB    0x0001u
#define I2C_STAR1_ADDR  0x0002u
#define I2C_STAR1_BTF   0x0004u
#define I2C_STAR1_STOPF 0x0010u
#define I2C_STAR1_TXE   0x0080u
#define I2C_STAR1_RXNE  0x0040u

/* STAR2 位 */
#define I2C_STAR2_MSL   0x0001u
#define I2C_STAR2_BUSY  0x0002u
#define I2C_STAR2_TRA   0x0004u

static uint16_t ch32_i2c_read_reg(Ch32I2cState *ip, hwaddr off)
{
    switch (off) {
    case I2C_CTLR1_OFF:  return ip->ctlr1;
    case I2C_CTLR2_OFF:  return ip->ctlr2;
    case I2C_OADDR1_OFF: return ip->oaddr1;
    case I2C_OADDR2_OFF: return ip->oaddr2;
    case I2C_DATAR_OFF:  return ip->datar;
    case I2C_STAR1_OFF:
        {
            uint16_t v = ip->star1;
            /* 若处于数据发送阶段且未停止，保持 TXE|BTF */
            if (ip->addr_pending && !ip->stop_pending) {
                v |= I2C_STAR1_TXE | I2C_STAR1_BTF;
            }
            return v;
        }
    case I2C_STAR2_OFF:
        {
            uint16_t v = ip->star2;
            /* 读 STAR2 同时会清除 ADDR（硬件行为） */
            if (ip->star1 & I2C_STAR1_ADDR) {
                ip->star1 &= ~I2C_STAR1_ADDR;
            }
            return v;
        }
    case I2C_CKCFGR_OFF: return ip->ckcfgr;
    case I2C_RTR_OFF:    return ip->rtr;
    default:             return 0;
    }
}

static void ch32_i2c_update_state(Ch32I2cState *ip)
{
    /* START 置位后：SB=1, BUSY=1, MSL=1 */
    if (ip->start_pending) {
        ip->star1 |= I2C_STAR1_SB;
        ip->star2 |= I2C_STAR2_BUSY | I2C_STAR2_MSL;
    }
    /* Send7bitAddress 后：ADDR=1, TXE=1, BTF=1, TRA=1, SB 清 */
    if (ip->addr_pending) {
        ip->star1 |= I2C_STAR1_ADDR | I2C_STAR1_TXE | I2C_STAR1_BTF;
        ip->star1 &= ~I2C_STAR1_SB;
        ip->star2 |= I2C_STAR2_TRA;
    }
    /* STOP 后：STOPF=1, BUSY=0, MSL=0 */
    if (ip->stop_pending) {
        ip->star1 |= I2C_STAR1_STOPF;
        ip->star1 &= ~(I2C_STAR1_ADDR | I2C_STAR1_SB |
                       I2C_STAR1_TXE | I2C_STAR1_BTF);
        ip->star2 &= ~(I2C_STAR2_BUSY | I2C_STAR2_MSL | I2C_STAR2_TRA);
    }
}

static void ch32_i2c_write_reg(Ch32I2cState *ip, hwaddr off, uint16_t val)
{
    switch (off) {
    case I2C_CTLR1_OFF:
        if (val & I2C_CTLR1_START) {
            ip->start_pending = true;
            ip->addr_pending  = false;
            ip->stop_pending  = false;
        }
        if (val & I2C_CTLR1_STOP) {
            ip->stop_pending  = true;
            ip->start_pending = false;
            ip->addr_pending  = false;
        }
        ip->ctlr1 = val;
        ch32_i2c_update_state(ip);
        break;
    case I2C_CTLR2_OFF:
        ip->ctlr2 = val;
        break;
    case I2C_OADDR1_OFF:
        ip->oaddr1 = val;
        break;
    case I2C_OADDR2_OFF:
        ip->oaddr2 = val;
        break;
    case I2C_DATAR_OFF:
        ip->datar = (uint8_t)val;
        /* 写 DATAR：如果 start_pending（发送地址字节），转入 addr 阶段 */
        if (ip->start_pending) {
            ip->start_pending = false;
            ip->addr_pending  = true;
        }
        ch32_i2c_update_state(ip);
        break;
    case I2C_STAR1_OFF:
        /* 软件写 0 清除位（错误标志清零方式） */
        ip->star1 &= val;
        break;
    case I2C_STAR2_OFF:
        ip->star2 = val;
        break;
    case I2C_CKCFGR_OFF:
        ip->ckcfgr = val;
        break;
    case I2C_RTR_OFF:
        ip->rtr = val;
        break;
    default:
        break;
    }
}

/*
 * read/write 回调：opaque 直接指向 Ch32I2cState 实例，
 * addr 是相对于 MemoryRegion 基址的偏移（0 ~ CH32_I2C_REG_SIZE-1）。
 */
static uint64_t ch32_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32I2cState *ip = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = addr + i;
        uint8_t b = 0;

        if (a < CH32_I2C_REG_SIZE) {
            hwaddr reg_off = a & ~3u;
            unsigned byte_in_reg = a & 3u;
            if (byte_in_reg < 2u) {
                uint16_t reg = ch32_i2c_read_reg(ip, reg_off);
                b = (uint8_t)(reg >> (8u * byte_in_reg));
            }
        }
        v |= (uint64_t)b << (8u * i);
    }
    return v;
}

static void ch32_i2c_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Ch32I2cState *ip = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = addr + i;

        if (a < CH32_I2C_REG_SIZE) {
            hwaddr reg_off = a & ~3u;
            unsigned byte_in_reg = a & 3u;
            if (byte_in_reg < 2u) {
                uint16_t old = ch32_i2c_read_reg(ip, reg_off);
                uint8_t  new_byte = (uint8_t)(val >> (8u * i));
                uint16_t merged;
                if (byte_in_reg == 0) {
                    merged = (old & 0xFF00u) | new_byte;
                } else {
                    merged = (old & 0x00FFu) | ((uint16_t)new_byte << 8u);
                }
                ch32_i2c_write_reg(ip, reg_off, merged);
            }
        }
    }
}

const MemoryRegionOps ch32_i2c_ops = {
    .read = ch32_i2c_read,
    .write = ch32_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

void ch32_i2c_reset(Ch32MachineState *m)
{
    unsigned i;

    for (i = 0; i < CH32_I2C_NUM; i++) {
        Ch32I2cState *ip = &m->i2c[i];
        memset(ip, 0, sizeof(*ip));
    }
}

void ch32_i2c_init_mr(Ch32MachineState *m, Object *owner)
{
    ch32_i2c_reset(m);

    /* 每个实例传入独立的 Ch32I2cState* 作为 opaque */
    memory_region_init_io(&m->i2c1_iomem, owner, &ch32_i2c_ops, &m->i2c[0],
                          "ch32-i2c1", CH32_I2C_REG_SIZE);
    memory_region_init_io(&m->i2c2_iomem, owner, &ch32_i2c_ops, &m->i2c[1],
                          "ch32-i2c2", CH32_I2C_REG_SIZE);
}
