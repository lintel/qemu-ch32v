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
 * File:     ch32-adc.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     CH32 ADC EVT（ADC 事件触发）最小 stub。
 *
 *     返回 EOC 标志和固定采样值，满足 EVT 示例中 ADC 轮询不卡死。
 */

#include "ch32-machine-internal.h"

static uint64_t ch32_adc_evt_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;

    if (addr == 0) {
        return CH32_ADC_FLAG_EOC;
    }
    if (addr == 0x4c) {
        return 0x123u;
    }
    return 0;
}

static void ch32_adc_evt_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)val;
    (void)size;
}

const MemoryRegionOps ch32_adc_evt_ops = {
    .read = ch32_adc_evt_read,
    .write = ch32_adc_evt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};
