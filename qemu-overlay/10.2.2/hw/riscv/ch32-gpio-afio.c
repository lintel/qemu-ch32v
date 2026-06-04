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
 * File:     ch32-gpio-afio.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     GPIO（EVT GPIO_TypeDef：CFGLR/CFGHR/INDR/OUTDR/BSHR/BCR/LCKR）与
 *     AFIO+EXTI 窗口（0x40010000 起 0x800）：AFIO 寄存器 + EXTI 区影子 RAM 语义。
 */

#include "ch32-machine-internal.h"

#define GPIO_REG_CFGLR 0
#define GPIO_REG_CFGHR 4
#define GPIO_REG_INDR  8
#define GPIO_REG_OUTDR 0x0c
#define GPIO_REG_BSHR  0x10
#define GPIO_REG_BCR   0x14
#define GPIO_REG_LCKR  0x18

static uint16_t ch32_gpio_indr_value(const Ch32GpioPort *p)
{
    uint16_t om = 0;
    int i;

    for (i = 0; i < 16; i++) {
        uint32_t m = (i < 8)
                         ? (p->cfglr >> (i * 4)) & 0xfu
                         : (p->cfghr >> ((i - 8) * 4)) & 0xfu;
        bool out = (m & 3u) != 0;

        if (out) {
            om |= (uint16_t)(1u << i);
        }
    }
    return (uint16_t)((p->outdr & om) | (p->indr_in & ~om));
}

void ch32_gpio_afio_reset(Ch32MachineState *m)
{
    unsigned i;

    /*
     * 根据手册表 10-47：
     *   GPIOx_CFGLR / GPIOx_CFGHR 复位值为 0x44444444（所有引脚浮空输入模式）。
     *   GPIOx_OUTDR 复位值为 0x00000000。
     *   GPIOx_LCKR  复位值为 0x00000000。
     */
    for (i = 0; i < CH32_GPIO_NPORTS; i++) {
        m->gpio_ports[i].cfglr   = 0x44444444u;
        m->gpio_ports[i].cfghr   = 0x44444444u;
        m->gpio_ports[i].indr_in = 0xffffu;
        m->gpio_ports[i].outdr   = 0;
        m->gpio_ports[i].lckr    = 0;
    }
    m->afio_ecr = 0;
    m->afio_pcfr1 = 0;
    m->afio_pcfr2 = 0;
    memset(m->afio_exticr, 0, sizeof(m->afio_exticr));
    memset(m->exti_shadow, 0, sizeof(m->exti_shadow));
}

static uint32_t ch32_gpio_port_read(Ch32GpioPort *p, hwaddr roff, unsigned size)
{
    switch (roff) {
    case GPIO_REG_CFGLR:
        return p->cfglr;
    case GPIO_REG_CFGHR:
        return p->cfghr;
    case GPIO_REG_INDR:
        return ch32_gpio_indr_value(p);
    case GPIO_REG_OUTDR:
        return p->outdr & 0xffffu;
    case GPIO_REG_LCKR:
        return p->lckr;
    default:
        return 0;
    }
}

static void ch32_gpio_port_write(Ch32GpioPort *p, hwaddr roff, uint64_t val,
                                 unsigned size)
{
    uint32_t v = (uint32_t)val;

    (void)size;
    switch (roff) {
    case GPIO_REG_CFGLR:
        p->cfglr = v;
        return;
    case GPIO_REG_CFGHR:
        p->cfghr = v;
        return;
    case GPIO_REG_OUTDR:
        p->outdr = v & 0xffffu;
        return;
    case GPIO_REG_BSHR:
        p->outdr = (uint16_t)((p->outdr | (v & 0xffffu)) &
                               ~((uint16_t)((v >> 16) & 0xffffu)));
        return;
    case GPIO_REG_BCR:
        p->outdr &= ~(uint16_t)(v & 0xffffu);
        return;
    case GPIO_REG_LCKR:
        p->lckr = v;
        return;
    default:
        return;
    }
}

static uint64_t ch32_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;
    hwaddr gpio_addr;
    unsigned port;
    hwaddr roff;

    /*
     * The memory region is registered from CH32_GPIO_BLOCK_BASE (0x40010000)
     * but real GPIO starts at 0x40010800. Accesses below offset 0x800 fall in
     * the AFIO range and are handled by the higher-priority AFIO region; we
     * should never reach here for those, but guard anyway.
     */
    if (addr < CH32_GPIO_BLOCK_OFFSET) {
        return 0;
    }
    gpio_addr = addr - CH32_GPIO_BLOCK_OFFSET;
    port = (unsigned)(gpio_addr >> 10);
    roff = gpio_addr & 0x3ffu;

    if (port >= CH32_GPIO_NPORTS) {
        return 0;
    }
    {
        uint32_t w = ch32_gpio_port_read(&m->gpio_ports[port], roff, size);

        if (size == 1) {
            return (w >> ((roff & 3) * 8)) & 0xff;
        }
        if (size == 2) {
            return w & 0xffffu;
        }
        return w;
    }
}

static void ch32_gpio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Ch32MachineState *m = opaque;
    hwaddr gpio_addr;
    unsigned port;
    hwaddr roff;

    /* See ch32_gpio_read: addr < 0x800 is AFIO range, ignore */
    if (addr < CH32_GPIO_BLOCK_OFFSET) {
        return;
    }
    gpio_addr = addr - CH32_GPIO_BLOCK_OFFSET;
    port = (unsigned)(gpio_addr >> 10);
    roff = gpio_addr & 0x3ffu;

    if (port >= CH32_GPIO_NPORTS) {
        return;
    }
    ch32_gpio_port_write(&m->gpio_ports[port], roff, val, size);
}

const MemoryRegionOps ch32_gpio_ops = {
    .read = ch32_gpio_read,
    .write = ch32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t ch32_afio_exti_read(void *opaque, hwaddr addr, unsigned size)
{
    Ch32MachineState *m = opaque;

    if (addr >= CH32_AFIO_EXTI_SIZE) {
        return 0;
    }
    if (addr < 0x400) {
        uint32_t w;

        /* 先按 4-byte 对齐地址读取寄存器字 */
        switch (addr & ~3u) {
        case 0:
            w = m->afio_ecr;
            break;
        case 4:
            w = m->afio_pcfr1;
            break;
        case 8:
            w = m->afio_exticr[0];
            break;
        case 0x0c:
            w = m->afio_exticr[1];
            break;
        case 0x10:
            w = m->afio_exticr[2];
            break;
        case 0x14:
            w = m->afio_exticr[3];
            break;
        case 0x1c:
            w = m->afio_pcfr2;
            break;
        default:
            return 0;
        }
        /* 按访问宽度截断 */
        if (size == 1) {
            return (w >> ((addr & 3u) * 8u)) & 0xffu;
        }
        if (size == 2) {
            return (w >> ((addr & 2u) * 8u)) & 0xffffu;
        }
        return w;
    }
    {
        hwaddr e = addr - 0x400;
        uint32_t w;

        if ((e & 3) || e >= sizeof(m->exti_shadow)) {
            return 0;
        }
        w = m->exti_shadow[e / 4];
        if (size == 1) {
            return (w >> ((addr & 3u) * 8u)) & 0xffu;
        }
        if (size == 2) {
            return w & 0xffffu;
        }
        return w;
    }
}

static void ch32_afio_exti_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    Ch32MachineState *m = opaque;

    if (addr >= CH32_AFIO_EXTI_SIZE) {
        return;
    }
    if (addr < 0x400) {
        if (size == 4 && (addr & 3) == 0) {
            switch (addr) {
            case 0:
                m->afio_ecr = (uint32_t)val;
                return;
            case 4:
                m->afio_pcfr1 = (uint32_t)val;
                return;
            case 8:
                m->afio_exticr[0] = (uint32_t)val;
                return;
            case 0x0c:
                m->afio_exticr[1] = (uint32_t)val;
                return;
            case 0x10:
                m->afio_exticr[2] = (uint32_t)val;
                return;
            case 0x14:
                m->afio_exticr[3] = (uint32_t)val;
                return;
            case 0x1c:
                m->afio_pcfr2 = (uint32_t)val;
                return;
            default:
                return;
            }
        }
        return;
    }
    {
        hwaddr e = addr - 0x400;

        if ((e & 3) || size != 4) {
            return;
        }
        if (e < sizeof(m->exti_shadow)) {
            m->exti_shadow[e / 4] = (uint32_t)val;
        }
    }
}

const MemoryRegionOps ch32_afio_exti_ops = {
    .read = ch32_afio_exti_read,
    .write = ch32_afio_exti_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

void ch32_gpio_init_mr(Ch32MachineState *m, Object *owner)
{
    memory_region_init_io(&m->gpio_iomem, owner, &ch32_gpio_ops, m,
                          "ch32-gpio", CH32_GPIO_BLOCK_SIZE);
}

void ch32_afio_exti_init_mr(Ch32MachineState *m, Object *owner)
{
    memory_region_init_io(&m->afio_exti_iomem, owner, &ch32_afio_exti_ops, m,
                          "ch32-afio-exti", CH32_AFIO_EXTI_SIZE);
}
