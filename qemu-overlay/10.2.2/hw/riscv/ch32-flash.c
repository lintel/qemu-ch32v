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
 * File:     ch32-flash.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32V30x Flash 控制器 MMIO（0x40022000）与片内 Flash RAM 擦除语义。
 */

#include "ch32-machine-internal.h"

#define CH32_FLASH_KEY1        0x45670123u
#define CH32_FLASH_KEY2        0xCDEF89ABu
#define CH32_FLASH_STATR_BSY   0x00000001u
#define CH32_FLASH_STATR_WRBSY 0x00000002u
#define CH32_FLASH_STATR_WRPRTERR 0x00000010u
#define CH32_FLASH_STATR_EOP   0x00000020u
#define CH32_FLASH_STATR_EHMODS 0x00000080u
#define CH32_FLASH_STATR_W1C_MASK (CH32_FLASH_STATR_BSY | CH32_FLASH_STATR_WRBSY | \
                                   CH32_FLASH_STATR_WRPRTERR | CH32_FLASH_STATR_EOP | \
                                   CH32_FLASH_STATR_EHMODS)
#define CH32_FLASH_CTLR_PG     0x00000001u
#define CH32_FLASH_CTLR_PER    0x00000002u
#define CH32_FLASH_CTLR_MER    0x00000004u
#define CH32_FLASH_CTLR_OPTPG  0x00000010u
#define CH32_FLASH_CTLR_OPTER  0x00000020u
#define CH32_FLASH_CTLR_STRT   0x00000040u
#define CH32_FLASH_CTLR_LOCK   0x00000080u
#define CH32_FLASH_CTLR_OPTWRE 0x00000200u
#define CH32_FLASH_CTLR_FLOCK  0x00008000u
#define CH32_FLASH_CTLR_PAGE_PG 0x00010000u
#define CH32_FLASH_CTLR_PAGE_ER 0x00020000u
#define CH32_FLASH_CTLR_BER32  0x00040000u
#define CH32_FLASH_CTLR_PG_STRT 0x00200000u

static bool ch32_flash_sector_wp(Ch32MachineState *m, uint32_t flash_addr)
{
    uint64_t mr_size = memory_region_size(&m->flash);
    uint32_t wpr = m->flash_reg_shadow[8];
    uint32_t sec;

    if (flash_addr < CH32_FLASH_BASE || flash_addr >= CH32_FLASH_BASE + mr_size) {
        return false;
    }
    sec = (uint32_t)((flash_addr - (uint32_t)CH32_FLASH_BASE) / 4096u);
    if (sec >= 31u) {
        return (wpr & 0x80000000u) != 0;
    }
    return ((wpr >> sec) & 1u) != 0;
}

static bool ch32_flash_any_sector_wp(Ch32MachineState *m)
{
    uint64_t sz = memory_region_size(&m->flash);
    uint64_t off;

    for (off = 0; off < sz; off += 4096u) {
        if (ch32_flash_sector_wp(m, (uint32_t)(CH32_FLASH_BASE + off))) {
            return true;
        }
    }
    return false;
}

static void ch32_flash_op_complete(Ch32MachineState *m, bool wrprot_err)
{
    m->flash_statr &= ~(uint32_t)(CH32_FLASH_STATR_BSY | CH32_FLASH_STATR_WRBSY);
    if (wrprot_err) {
        m->flash_statr |= CH32_FLASH_STATR_WRPRTERR;
    }
    m->flash_statr |= CH32_FLASH_STATR_EOP;
}

static void ch32_flash_erase_4k(Ch32MachineState *m, uint32_t flash_addr)
{
    uint8_t *ram;
    uint64_t mr_size;
    hwaddr off;

    mr_size = memory_region_size(&m->flash);
    if (flash_addr < CH32_FLASH_BASE || flash_addr >= CH32_FLASH_BASE + mr_size) {
        ch32_flash_op_complete(m, false);
        return;
    }
    off = (hwaddr)(flash_addr - CH32_FLASH_BASE) & ~(hwaddr)0xfff;
    if (off + 4096 > mr_size) {
        ch32_flash_op_complete(m, false);
        return;
    }
    if (ch32_flash_sector_wp(m, (uint32_t)(CH32_FLASH_BASE + off))) {
        ch32_flash_op_complete(m, true);
        return;
    }
    ram = memory_region_get_ram_ptr(&m->flash);
    memset(ram + off, 0xff, 4096);
    ch32_flash_op_complete(m, false);
}

static void ch32_flash_erase_256(Ch32MachineState *m, uint32_t flash_addr)
{
    uint8_t *ram;
    uint64_t mr_size;
    hwaddr off;

    mr_size = memory_region_size(&m->flash);
    if (flash_addr < CH32_FLASH_BASE || flash_addr >= CH32_FLASH_BASE + mr_size) {
        ch32_flash_op_complete(m, false);
        return;
    }
    off = (hwaddr)(flash_addr - CH32_FLASH_BASE) & ~(hwaddr)0xff;
    if (off + 256 > mr_size) {
        ch32_flash_op_complete(m, false);
        return;
    }
    if (ch32_flash_sector_wp(m, (uint32_t)(CH32_FLASH_BASE + off))) {
        ch32_flash_op_complete(m, true);
        return;
    }
    ram = memory_region_get_ram_ptr(&m->flash);
    memset(ram + off, 0xff, 256);
    ch32_flash_op_complete(m, false);
}

static void ch32_flash_erase_32k(Ch32MachineState *m, uint32_t flash_addr)
{
    uint8_t *ram;
    uint64_t mr_size;
    hwaddr off, chunk, o;

    mr_size = memory_region_size(&m->flash);
    if (flash_addr < CH32_FLASH_BASE || flash_addr >= CH32_FLASH_BASE + mr_size) {
        ch32_flash_op_complete(m, false);
        return;
    }
    off = (hwaddr)(flash_addr - CH32_FLASH_BASE) & ~(hwaddr)0x7fff;
    chunk = 32 * 1024;
    if (off + chunk > mr_size) {
        chunk = mr_size - off;
    }
    for (o = off; o < off + chunk; o += 4096u) {
        if (ch32_flash_sector_wp(m, (uint32_t)(CH32_FLASH_BASE + o))) {
            ch32_flash_op_complete(m, true);
            return;
        }
    }
    ram = memory_region_get_ram_ptr(&m->flash);
    memset(ram + off, 0xff, chunk);
    ch32_flash_op_complete(m, false);
}

static void ch32_flash_mass_erase(Ch32MachineState *m)
{
    uint8_t *ram;
    uint64_t mr_size;

    if (ch32_flash_any_sector_wp(m)) {
        ch32_flash_op_complete(m, true);
        return;
    }
    mr_size = memory_region_size(&m->flash);
    ram = memory_region_get_ram_ptr(&m->flash);
    memset(ram, 0xff, mr_size);
    ch32_flash_op_complete(m, false);
}

static void ch32_flash_key_write(Ch32MachineState *m, uint32_t v)
{
    if (v == CH32_FLASH_KEY1) {
        m->flash_key_step = 1;
        return;
    }
    if (m->flash_key_step == 1 && v == CH32_FLASH_KEY2) {
        m->flash_unlocked = true;
        m->flash_reg_shadow[4] &= ~(uint32_t)(CH32_FLASH_CTLR_LOCK | CH32_FLASH_CTLR_FLOCK);
        m->flash_key_step = 0;
        return;
    }
    m->flash_key_step = 0;
}

static void ch32_flash_obkey_write(Ch32MachineState *m, uint32_t v)
{
    if (v == CH32_FLASH_KEY1) {
        m->flash_ob_key_step = 1;
        return;
    }
    if (m->flash_ob_key_step == 1 && v == CH32_FLASH_KEY2) {
        m->flash_reg_shadow[4] |= CH32_FLASH_CTLR_OPTWRE;
        m->flash_ob_key_step = 0;
        return;
    }
    m->flash_ob_key_step = 0;
}

static void ch32_flash_ctl_write(Ch32MachineState *m, uint32_t val)
{
    uint32_t nw = val;

    if (nw & CH32_FLASH_CTLR_LOCK) {
        m->flash_unlocked = false;
        nw &= ~(uint32_t)CH32_FLASH_CTLR_OPTWRE;
    }

    if (m->flash_unlocked && (nw & CH32_FLASH_CTLR_STRT)) {
        uint32_t addr = m->flash_reg_shadow[5];

        if (nw & CH32_FLASH_CTLR_MER) {
            ch32_flash_mass_erase(m);
            nw &= ~(uint32_t)(CH32_FLASH_CTLR_STRT | CH32_FLASH_CTLR_MER);
        } else if (nw & CH32_FLASH_CTLR_OPTER) {
            ch32_flash_op_complete(m, false);
            nw &= ~(uint32_t)(CH32_FLASH_CTLR_STRT | CH32_FLASH_CTLR_OPTER);
        } else if (nw & CH32_FLASH_CTLR_BER32) {
            ch32_flash_erase_32k(m, addr);
            nw &= ~(uint32_t)(CH32_FLASH_CTLR_STRT | CH32_FLASH_CTLR_BER32);
        } else if (nw & CH32_FLASH_CTLR_PER) {
            ch32_flash_erase_4k(m, addr);
            nw &= ~(uint32_t)(CH32_FLASH_CTLR_STRT | CH32_FLASH_CTLR_PER);
        } else if (nw & CH32_FLASH_CTLR_PAGE_ER) {
            ch32_flash_erase_256(m, addr);
            nw &= ~(uint32_t)(CH32_FLASH_CTLR_STRT | CH32_FLASH_CTLR_PAGE_ER);
        } else if (nw & CH32_FLASH_CTLR_OPTPG) {
            ch32_flash_op_complete(m, false);
            nw &= ~(uint32_t)(CH32_FLASH_CTLR_STRT | CH32_FLASH_CTLR_OPTPG);
        } else {
            nw &= ~(uint32_t)CH32_FLASH_CTLR_STRT;
            ch32_flash_op_complete(m, false);
        }
    }

    if (nw & CH32_FLASH_CTLR_PG_STRT) {
        nw &= ~(uint32_t)CH32_FLASH_CTLR_PG_STRT;
        ch32_flash_op_complete(m, false);
    }

    m->flash_reg_shadow[4] = nw;
}

static uint64_t ch32_flashreg_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;
    uint32_t v;

    if (addr >= CH32_FLASHREG_SIZE) {
        ch32_mmio_log_bad_offset("flash", addr, size, false);
        return 0;
    }
    if (size != 1 && size != 2 && size != 4) {
        ch32_mmio_log_bad_width("flash", addr, size, false);
        return 0;
    }
    if (addr == 0x04) {
        v = 0;
    } else if (addr == 0x0c) {
        v = m->flash_statr;
    } else if (!(addr & 3)) {
        v = m->flash_reg_shadow[addr / 4];
    } else {
        /* 非 4 字节对齐地址：尝试对齐读字，然后按字节内偏移提取 */
        hwaddr aligned = addr & ~(hwaddr)3;

        if (aligned >= CH32_FLASHREG_SIZE) {
            ch32_mmio_log_bad_offset("flash", addr, size, false);
            return 0;
        }
        v = m->flash_reg_shadow[aligned / 4];
        /* 对齐地址 + 内偏移 = 请求地址，右移 (addr&3)*8 位返回目标字节/半字 */
        v >>= (addr & 3) * 8;
    }

    if (size == 4) {
        return v;
    }
    if (size == 2) {
        return v & 0xffffu;
    }
    return v & 0xffu;
}

static void ch32_flashreg_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Ch32MachineState *m = opaque;

    if (addr >= CH32_FLASHREG_SIZE || (addr & 3)) {
        ch32_mmio_log_bad_offset("flash", addr, size, true);
        return;
    }
    if (size != 4) {
        ch32_mmio_log_bad_width("flash", addr, size, true);
        return;
    }
    if (addr == 0x04) {
        ch32_flash_key_write(m, (uint32_t)val);
        return;
    }
    if (addr == 0x08) {
        ch32_flash_obkey_write(m, (uint32_t)val);
        return;
    }
    if (addr == 0x0c) {
        m->flash_statr &= ~((uint32_t)val & CH32_FLASH_STATR_W1C_MASK);
        return;
    }
    if (addr == 0x10) {
        ch32_flash_ctl_write(m, (uint32_t)val);
        return;
    }
    if (addr == 0x24) {
        ch32_flash_key_write(m, (uint32_t)val);
        return;
    }
    m->flash_reg_shadow[addr / 4] = (uint32_t)val;
}

const MemoryRegionOps ch32_flashreg_ops = {
    .read = ch32_flashreg_read,
    .write = ch32_flashreg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
