/*
 * WCH QingKe: XW compressed loads/stores (decode fallback) + helpers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * decode_wch_mcpy32 - WCH QingKe V3B/V3C/V3F/V3V mcpy instruction
 *
 * Encoding (32-bit, R4-type):
 *   [31:27] rs3 = dst_start
 *   [26:25] = 00
 *   [24:20] rs2 = src_start
 *   [19:15] rs1 = src_end
 *   [14:12] func3 = 111
 *   [11:7]  rd   = 00000
 *   [6:0]   opcode = 0x0F
 *
 * MATCH = 0x0000700F, MASK = 0x06007FFF
 *
 * Semantics: memcpy(GPR[rs3], GPR[rs2], GPR[rs1] - GPR[rs2])
 * Implemented as inline TCG loop; rs2 and rs3 advance as side-effect.
 */
static bool decode_wch_mcpy32(DisasContext *ctx, uint32_t insn)
{
    uint32_t opcode = insn & 0x7f;
    uint32_t func3  = (insn >> 12) & 7;
    uint32_t rd     = (insn >> 7)  & 0x1f;

    if (!ctx->cfg_ptr->ext_xwchmcpy) {
        return false;
    }

    /* MATCH / MASK check: opcode=0x0F, func3=7, rd=0, bits[26:25]=00 */
    if (opcode != 0x0f || func3 != 7 || rd != 0 ||
        ((insn >> 25) & 3) != 0) {
        return false;
    }

    int rs1 = (insn >> 15) & 0x1f;  /* src_end   */
    int rs2 = (insn >> 20) & 0x1f;  /* src_start */
    int rs3 = (insn >> 27) & 0x1f;  /* dst_start */

    /*
     * TCG implementation: byte-by-byte copy loop.
     * while (src < end): *dst++ = *src++
     *
     * We implement it as a TCG helper call to avoid generating massive
     * inline TCG for variable-length copies.
     */
    TCGv t_end   = get_gpr(ctx, rs1, EXT_NONE);
    TCGv t_src   = dest_gpr(ctx, rs2);
    TCGv t_dst   = dest_gpr(ctx, rs3);

    /* Copy t_end / t_src / t_dst to temporaries (rs2/rs3 are modified) */
    TCGv v_end = tcg_temp_new();
    TCGv v_src = tcg_temp_new();
    TCGv v_dst = tcg_temp_new();
    TCGv v_byte = tcg_temp_new();
    TCGLabel *loop_start = gen_new_label();
    TCGLabel *loop_end   = gen_new_label();

    tcg_gen_mov_tl(v_end, t_end);
    tcg_gen_mov_tl(v_src, get_gpr(ctx, rs2, EXT_NONE));
    tcg_gen_mov_tl(v_dst, get_gpr(ctx, rs3, EXT_NONE));

    /* Loop: if (v_src >= v_end) goto loop_end */
    gen_set_label(loop_start);
    tcg_gen_brcond_tl(TCG_COND_GEU, v_src, v_end, loop_end);

    /* Load byte from [v_src], store to [v_dst] */
    tcg_gen_qemu_ld_tl(v_byte, v_src, ctx->mem_idx, MO_UB);
    tcg_gen_qemu_st_tl(v_byte, v_dst, ctx->mem_idx, MO_UB);

    tcg_gen_addi_tl(v_src, v_src, 1);
    tcg_gen_addi_tl(v_dst, v_dst, 1);
    tcg_gen_br(loop_start);

    gen_set_label(loop_end);

    /* Write back modified src and dst to GPRs (rs1/end unchanged per spec) */
    if (rs2 != 0) {
        tcg_gen_mov_tl(t_src, v_src);
    }
    if (rs3 != 0) {
        tcg_gen_mov_tl(t_dst, v_dst);
    }
    gen_set_gpr(ctx, rs2, t_src);
    gen_set_gpr(ctx, rs3, t_dst);

    return true;
}

static bool decode_wch_xw16(DisasContext *ctx, uint16_t insn)
{
    arg_lb al;
    arg_sb as;

    if (!ctx->cfg_ptr->ext_xw || !has_ext(ctx, RVC)) {
        return false;
    }

    /*
     * 仅象限 0（[1:0]==00）：c.lbu/c.sb 等。象限 2 的 c.lhu/c.sh 见
     * decode_wch_xw16_quad2()（原 c.fldsp/c.fsdsp 码点）。
     */
    if ((insn & 3) != 0) {
        return false;
    }

    switch (insn & 0xe003) {
    case 0x2000: /* quadrant 0, C.LBU / C.FLD slot */
        /* ext_xw 译码在 translate.c 中先于 insn16；此处不再因 RVD 放弃 XW。 */
        /* 与 binutils EXTRACT_WCH_QK_UIMM5 一致（勿并入 insn[11] 的额外位移项）。 */
        al.imm = extract32(insn, 12, 1) |
                 (extract32(insn, 5, 2) << 1) |
                 (extract32(insn, 10, 2) << 3);
        al.rs1 = 8 + extract32(insn, 7, 3);
        al.rd = 8 + extract32(insn, 2, 3);
        return gen_load(ctx, &al, MO_UB);

    case 0xa000: /* C.SB / C.FSD slot */
        as.imm = extract32(insn, 12, 1) |
                 (extract32(insn, 5, 2) << 1) |
                 (extract32(insn, 10, 2) << 3);
        as.rs1 = 8 + extract32(insn, 7, 3);
        as.rs2 = 8 + extract32(insn, 2, 3);
        return gen_store(ctx, &as, MO_UB);

    case 0x8000: /* XW stack-relative（保留槽；仅象限 0） */
        /*
         * 与 binutils DECLARE_INSN 的语义一致：仅用 (insn & MASK)==MATCH 约束固定位，
         * 勿用 (insn & ~MASK)==0 拒绝合法编码。例如 GCC 15 对 OrayOS-Tiny 会生成
         * 0x8274（c.shsp 槽、MATCH 与 0x8000 在 MASK 下对齐但保留位非零）；旧检查
         * 会落入标准 RVC 误译并触发 fault_load + illegal 风暴。
         */
        if (((insn >> 11) & 0x1f) != 0x10 || (insn & 0x1800) != 0) {
            return false;
        }
        switch ((insn >> 5) & 3) {
        case 0: /* c.lbusp */
            al.imm = extract32(insn, 7, 4);
            al.rs1 = 2;
            al.rd = 8 + extract32(insn, 2, 3);
            return gen_load(ctx, &al, MO_UB);
        case 1: /* c.lhusp */
            al.imm = (extract32(insn, 8, 3) << 1) |
                     (extract32(insn, 7, 1) << 4);
            al.rs1 = 2;
            al.rd = 8 + extract32(insn, 2, 3);
            return gen_load(ctx, &al, MO_UW);
        case 2: /* c.sbsp */
            as.imm = extract32(insn, 7, 4);
            as.rs1 = 2;
            as.rs2 = 8 + extract32(insn, 2, 3);
            return gen_store(ctx, &as, MO_UB);
        case 3: /* c.shsp */
            as.imm = (extract32(insn, 8, 3) << 1) |
                     (extract32(insn, 7, 1) << 4);
            as.rs1 = 2;
            as.rs2 = 8 + extract32(insn, 2, 3);
            return gen_store(ctx, &as, MO_UW);
        }
        return false;

    default:
        return false;
    }
}

/*
 * XW 在象限 2（……10）复用无 D 核上空置的标准槽：
 *   001 … 10  → qk.c.lhu（文档 §5；GCC 与 binutils -M xw 一致）
 *   101 … 10  → qk.c.sh
 * 与 Zcmp（101……10 的部分子码点）并存时，子码点交给 decode_insn16。
 */
static bool decode_wch_xw16_quad2(DisasContext *ctx, uint16_t insn)
{
    arg_lb al;
    arg_sb as;

    if (!ctx->cfg_ptr->ext_xw || !has_ext(ctx, RVC)) {
        return false;
    }
    if ((insn & 3) != 2) {
        return false;
    }
    if (has_ext(ctx, RVD)) {
        return false;
    }

    switch ((insn >> 13) & 7) {
    case 1:
        al.imm = (extract32(insn, 10, 3) << 3) | (extract32(insn, 5, 2) << 1);
        al.rs1 = 8 + extract32(insn, 7, 3);
        al.rd = 8 + extract32(insn, 2, 3);
        return gen_load(ctx, &al, MO_UW);

    case 5:
        if (ctx->cfg_ptr->ext_zcmp || ctx->cfg_ptr->ext_zcmt) {
            unsigned t = (insn >> 10) & 7;

            if (t == 0 || t == 3 || t == 6 || t == 7) {
                return false;
            }
        }
        as.imm = (extract32(insn, 10, 3) << 3) | (extract32(insn, 5, 2) << 1);
        as.rs1 = 8 + extract32(insn, 7, 3);
        as.rs2 = 8 + extract32(insn, 2, 3);
        return gen_store(ctx, &as, MO_UW);

    default:
        return false;
    }
}
