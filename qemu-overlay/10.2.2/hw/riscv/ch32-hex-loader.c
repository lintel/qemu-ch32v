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
 * File:     ch32-hex-loader.c
 * Author:   lintel <lintel.huang@gmail.com>
 * Date:     2026-04-27
 *
 * Description:
 *     WCH CH32 Intel HEX 固件加载器。
 *
 *     从 ch32-v.c 拆出的独立模块，负责将 Intel HEX 格式文件按记录地址写入
 *     Flash RAM，兼容 WCH MRS（链接到 0x00000000，无 ELA 记录）与标准
 *     工具链（链接到 0x08000000，含 type-04 ELA 记录）两种寻址约定。
 */

#include "ch32-machine-internal.h"

#include <errno.h>

/*
 * Intel HEX 解析器：将 HEX 文件内容写入 flash_ram[0..flash_sz)。
 *
 * 地址映射规则：
 *   - 若 HEX 数据地址在 [CH32_FLASH_BASE, CH32_FLASH_BASE+flash_sz) 内，
 *     写入偏移 = abs_addr - CH32_FLASH_BASE
 *   - 若 HEX 数据地址在 [0, flash_sz) 内（无 Extended Linear Address，如
 *     WCH MRS 工具链生成的 .hex），直接以偏移写入（兼容两种寻址约定）
 *   - 超出上述两个窗口的记录报 warn_report 并跳过
 *
 * 若 HEX 包含类型 05（Start Linear Address）记录，则更新 *entry。
 * 返回 true 表示成功，失败时已调用 error_report 并返回 false（调用方应 exit）。
 */
bool ch32_load_hex_image(uint8_t *flash_ram, uint64_t flash_sz,
                        const char *path, uint64_t *entry,
                        bool *out_have_ela)
{
    gchar *content = NULL;
    gsize  content_len = 0;
    GError *gerr = NULL;
    const gchar *p, *end;
    uint32_t ulba = 0;   /* upper linear base address (from record type 04) */
    uint32_t sba  = 0;   /* segment base address     (from record type 02) */
    bool have_ela = false;
    bool ok = false;
    int  lineno = 0;

    if (!g_file_get_contents(path, &content, &content_len, &gerr)) {
        error_report("ch32: could not read HEX file '%s': %s",
                     path, gerr ? gerr->message : "unknown");
        g_clear_error(&gerr);
        return false;
    }

    p   = content;
    end = content + content_len;

    while (p < end) {
        uint8_t  buf[512];
        int      nbytes = 0;
        uint8_t  sum = 0;
        uint8_t  rec_len;
        uint16_t rec_addr;
        uint8_t  rec_type;
        int      i;

        /* 跳过空白行 / CR */
        while (p < end && (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')) {
            p++;
        }
        if (p >= end) {
            break;
        }
        if (*p != ':') {
            error_report("ch32: HEX '%s' line %d: expected ':' got '%c'",
                         path, lineno + 1, *p);
            goto out;
        }
        p++;
        lineno++;

        /* 解析该行的所有 hex 字节（含 checksum）到 buf[] */
        while (p < end && *p != '\r' && *p != '\n') {
            char hi;
            char lo;
            int  hv;
            int  lv;

            hi = *p++;
            if (p >= end || *p == '\r' || *p == '\n') {
                error_report("ch32: HEX '%s' line %d: odd nibble", path, lineno);
                goto out;
            }
            lo = *p++;
            hv = g_ascii_xdigit_value(hi);
            lv = g_ascii_xdigit_value(lo);
            if (hv < 0 || lv < 0) {
                error_report("ch32: HEX '%s' line %d: invalid hex char", path, lineno);
                goto out;
            }
            if (nbytes >= (int)sizeof(buf)) {
                error_report("ch32: HEX '%s' line %d: record too long", path, lineno);
                goto out;
            }
            buf[nbytes++] = (uint8_t)((hv << 4) | lv);
        }

        /* 最小记录：LLAAAATT CC = 5 字节 */
        if (nbytes < 5) {
            error_report("ch32: HEX '%s' line %d: record too short (%d bytes)",
                         path, lineno, nbytes);
            goto out;
        }

        /* 校验和：全部字节累加 & 0xFF == 0 */
        for (i = 0; i < nbytes; i++) {
            sum = (uint8_t)(sum + buf[i]);
        }
        if (sum != 0) {
            error_report("ch32: HEX '%s' line %d: checksum error (got 0x%02x)",
                         path, lineno, sum);
            goto out;
        }

        rec_len  = buf[0];
        rec_addr = (uint16_t)((buf[1] << 8) | buf[2]);
        rec_type = buf[3];

        /* nbytes = rec_len + 5（含 LL AAAA TT CC） */
        if (nbytes != rec_len + 5) {
            error_report("ch32: HEX '%s' line %d: length mismatch (LL=%u, bytes=%d)",
                         path, lineno, rec_len, nbytes);
            goto out;
        }

        switch (rec_type) {
        case 0x00: { /* Data */
            /*
             * 计算 32-bit 绝对地址：
             *   若已见过 type-04（ELA）记录，使用 ulba；
             *   否则用 type-02（ESA）的 sba（段地址 × 16）。
             */
            uint32_t abs_addr;
            uint64_t off;

            if (have_ela) {
                abs_addr = (ulba << 16) | rec_addr;
            } else {
                abs_addr = sba + rec_addr;
            }

            /* 优先映射：[CH32_FLASH_BASE, CH32_FLASH_BASE + flash_sz) */
            if (abs_addr >= (uint32_t)CH32_FLASH_BASE &&
                abs_addr <  (uint32_t)(CH32_FLASH_BASE + flash_sz)) {
                off = abs_addr - (uint32_t)CH32_FLASH_BASE;
            } else if (!have_ela &&
                       abs_addr < flash_sz) {
                /*
                 * 兼容模式：HEX 无 ELA 记录且地址 < flash_sz，
                 * 视为相对于 Flash 起始的偏移（MRS 工具链常见输出）。
                 */
                off = abs_addr;
            } else {
                warn_report("ch32: HEX '%s' line %d: address 0x%08" PRIx32 " "
                            "outside flash [0x%08" PRIx32 ", 0x%08" PRIx64 "), skipped",
                            path, lineno, abs_addr,
                            (uint32_t)CH32_FLASH_BASE,
                            (uint64_t)(CH32_FLASH_BASE + flash_sz));
                break;
            }

            if (off + rec_len > flash_sz) {
                uint64_t clip;

                warn_report("ch32: HEX '%s' line %d: record [0x%" PRIx64
                            ", 0x%" PRIx64 ") exceeds flash, truncated",
                            path, lineno, off, off + rec_len);
                clip = flash_sz - off;
                memcpy(flash_ram + off, buf + 4, (size_t)clip);
            } else {
                memcpy(flash_ram + off, buf + 4, rec_len);
            }
            break;
        }
        case 0x01: /* End Of File */
            ok = true;
            goto out;

        case 0x02: /* Extended Segment Address */
            if (rec_len != 2) {
                error_report("ch32: HEX '%s' line %d: type 02 bad length", path, lineno);
                goto out;
            }
            sba = (uint32_t)((buf[4] << 8) | buf[5]) << 4;
            break;

        case 0x03: /* Start Segment Address (CS:IP) - 忽略，嵌入式不用 */
            break;

        case 0x04: /* Extended Linear Address */
            if (rec_len != 2) {
                error_report("ch32: HEX '%s' line %d: type 04 bad length", path, lineno);
                goto out;
            }
            ulba = (uint32_t)((buf[4] << 8) | buf[5]);
            have_ela = true;
            break;

        case 0x05: /* Start Linear Address */
            if (rec_len != 4) {
                error_report("ch32: HEX '%s' line %d: type 05 bad length", path, lineno);
                goto out;
            }
            *entry = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                     ((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7];
            break;

        default:
            warn_report("ch32: HEX '%s' line %d: unknown record type 0x%02x, ignored",
                        path, lineno, rec_type);
            break;
        }
    }

    /* 若遍历完文件未遇到 EOF 记录，视为截断文件 */
    if (!ok) {
        warn_report("ch32: HEX '%s': no EOF record found, treating as complete", path);
        ok = true;
    }

out:
    if (out_have_ela) {
        *out_have_ela = have_ela;
    }
    g_free(content);
    return ok;
}
