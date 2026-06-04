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
 * File:     ch32-boot.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     WCH CH32 启动期固件修正与 ESIG/DBGMCU 内容填充。
 *
 *     从 ch32-v.c 拆出的启动辅助逻辑，集中处理：
 *       1) Flash 首 4 字节 JAL 桩指令的合法性判定与修正（WCH ISP 常见问题）；
 *       2) ESIG 区（0x1FFFF7E0-0x1FFFF7FF）的 chip_id/flash_kb/UID 填充；
 */

#include "ch32-machine-internal.h"

/* ---------- Flash 首指 JAL 修正 ---------- */

#define CH32_FLASH_BAD_VEC_JAL   0x2020006fu
#define CH32_FLASH_FIXED_VEC_JAL 0x2020106fu

uint32_t ch32_decode_jal_target(uint64_t pc, uint32_t inst)
{
    int32_t imm = (((inst >> 31) & 1) << 20) |
                  (((inst >> 12) & 0xff) << 12) |
                  (((inst >> 20) & 1) << 11) |
                  (((inst >> 21) & 0x3ff) << 1);

    imm = (imm << 11) >> 11;
    return (uint32_t)((int64_t)pc + imm);
}

/*
 * 判定 JAL 的跳转目标是否指向合法的 startup 第一条指令。
 * WCH MRS startup.S 的 handle_reset 首条是 auipc gp, ...（opcode=0x17, rd=3/gp），
 * 用这一模式简易区分"WCH 典型 startup"与"野指针"。
 */
static bool ch32_flash_reset_entry_is_valid(const uint8_t *flash_ram,
                                            uint64_t flash_sz,
                                            uint32_t target)
{
    uint64_t off;

    if (target < CH32_FLASH_BASE) {
        return false;
    }
    off = (hwaddr)(target - CH32_FLASH_BASE);
    if (off + 4 > flash_sz) {
        return false;
    }
    {
        uint32_t w = ldl_le_p(flash_ram + off);

        return (w & 0x7fu) == 0x17u && ((w >> 7) & 0x1fu) == 3u;
    }
}

void ch32_flash_patch_vector_jal_if_needed(uint8_t *flash_ram, uint64_t flash_sz)
{
    uint32_t w;
    uint32_t jal_target;

    if (flash_sz < 4) {
        return;
    }
    w = ldl_le_p(flash_ram);
    if (w != CH32_FLASH_BAD_VEC_JAL) {
        return;
    }
    jal_target = ch32_decode_jal_target(CH32_FLASH_BASE, w);
    if (ch32_flash_reset_entry_is_valid(flash_ram, flash_sz, jal_target)) {
        return;
    }
    stl_le_p(flash_ram, CH32_FLASH_FIXED_VEC_JAL);
}

/* ---------- ESIG / DBGMCU IDCODE 填充 ---------- */

void ch32_esig_populate(uint8_t *p, size_t sz, const Ch32BoardDesc *bd,
                        uint64_t flash_size_bytes)
{
    /*
     * DBGMCU IDCODE / ESIG chip_id 数据库来源：
     *   [1] reference-project/wlink/src/chips.rs::chip_id_to_chip_name
     *       （WCH 官方 wlink 调试器的 CHIPID → 型号名映射）
     *   [2] reference-project/OrayOS-Tiny/src/drivers/misc/chipid.h
     *       （与本仓库固件深度集成的内部枚举）
     *
     * 已知两来源冲突：对 0x3173b508 / 0x3174b508
     *   wlink chips.rs：0x3170b508=VCT6 / 0x3173b508=WCU6 / 0x3175b508=TCU6
     *   OrayOS-Tiny   ：0x3173b508=VCT6 / 0x3174b508=WCU6（未定义 TCU6）
     * 本文件选用 OrayOS-Tiny 方言，与固件侧 chipid_detect_type() 的 V317VCT6
     * 分支一致，保证 SRAM=64KB / 内置 100M PHY 等路径正确命中。
     *
     * chip_id 低 4 位为硅修订版本（driver 里 `& 0xFFFFFF0F` 掩码后匹配），
     * 因此对未严格对应真机 revision 的场景，保留原有「低字节 0x14/0x18」
     * 调整以触发固件特定分支。
     */
    uint32_t id = 0x30700518u;
    uint16_t flash_kb;

    if (sz < 0x100) {
        return;
    }
    memset(p, 0, sz);
    if (!strcmp(bd->tag, "ch32v317")) {
        /* CHIP_CH32V317VCT6 (OrayOS-Tiny chipid.h) */
        id = 0x3173b508u;
    } else if (!strcmp(bd->tag, "ch32v307")) {
        /*
         * CH32V307VCT6 CHIP_ID：按 CH32V30x 系列格式，低字节 0x14 使
         * (ChipId & 0xf0) == 0x10，驱动 eth_driver_10M.c 识别为 CH32V30x 平台。
         * 驱动中 WCHNET_RecProcess 的 RBU 异常处理分支依赖此判断。
         * 来源：OrayOS-Tiny CHIP_CH32V307VCT6=0x30700508 → 调整 revision=0x14。
         */
        id = 0x30700514u;
    } else if (!strcmp(bd->tag, "ch32v305")) {
        /* CH32V305RBT6：wlink chips.rs 0x30500508 */
        id = 0x30500508u;
    } else if (!strcmp(bd->tag, "ch32v303")) {
        /* CH32V303CBT6：OrayOS-Tiny CHIP_CH32V303CBT6=0x30330504 */
        id = 0x30330504u;
    } else if (!strcmp(bd->tag, "ch32v203rb")) {
        /* CH32V203RBT6：OrayOS-Tiny CHIP_CH32V203RBT6=0x2034050C */
        id = 0x2034050cu;
    } else if (!strcmp(bd->tag, "ch32v203")) {
        /*
         * CH32V203C8T6 CHIP_ID：DEVID=0x2031，低8位为硅修订版本。
         * 来源：wlink chips.rs 0x20310500 / OrayOS-Tiny CHIP_CH32V203C8T6。
         */
        id = 0x20310500u;
    } else if (!strcmp(bd->tag, "ch32v003")) {
        /* CH32V003F4P6：wlink chips.rs 0x00300500 */
        id = 0x00300500u;
    }
    if (flash_size_bytes / 1024 > 65535) {
        flash_kb = 65535;
    } else {
        flash_kb = (uint16_t)(flash_size_bytes / 1024);
    }
    stl_le_p(p + 0x04, id);
    stw_le_p(p + 0xe0, flash_kb);
    stl_le_p(p + 0xe8, 0x11223344u);
    stl_le_p(p + 0xec, 0x55667788u);
    stl_le_p(p + 0xf0, 0x99aabbccu);
}
