/* native/kernel/gpu/gcn_enc_test.c -- Oracle test for the GCN encoder.
 *
 * For each instruction variant we emit bytes with our C encoder, then pipe
 * the same assembly text through llvm-mc-18 (-arch=amdgcn -mcpu=gfx803
 * -show-encoding) and compare byte-by-byte.  The oracle is NEVER in the
 * build; it lives only in this host test.
 *
 * Build (on Spark, no floats in kernel C; this is a host test only):
 *   gcc -O2 -Wall -Wextra -I. gcn_enc.c gcn_enc_test.c -o gcn_enc_test
 * Run:
 *   ./gcn_enc_test
 *
 * Expected output: "GCN_ENC_TEST: N instructions, 0 mismatches" + PASS line.
 */
#include "gcn_enc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- oracle: invoke llvm-mc-18 and parse the encoding bytes ---- */
static int oracle_encode(const char *asm_text, uint8_t out[8], int *nbytes)
{
    /* Build command: echo "TEXT" | llvm-mc-18 -arch=amdgcn -mcpu=gfx803 -show-encoding */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "printf '%%s\\n' '%s' | "
        "llvm-mc-18 -arch=amdgcn -mcpu=gfx803 -show-encoding 2>/dev/null",
        asm_text);

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;

    /* Find "; encoding: [0xXX,0xXX,...]" line */
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *enc = strstr(line, "encoding: [");
        if (!enc)
            continue;
        enc += strlen("encoding: [");
        *nbytes = 0;
        while (*enc && *enc != ']' && *nbytes < 8) {
            if (*enc == '0' && *(enc+1) == 'x') {
                out[*nbytes] = (uint8_t)strtoul(enc, NULL, 16);
                (*nbytes)++;
            }
            while (*enc && *enc != ',' && *enc != ']')
                enc++;
            if (*enc == ',')
                enc++;
        }
        found = 1;
        break;
    }
    pclose(fp);
    return found ? 0 : -1;
}

/* ---- test harness ---- */
static int total = 0;
static int mismatches = 0;

static void check(const char *desc, const char *asm_text,
                  const uint8_t *our_bytes, int our_size)
{
    uint8_t oracle[8] = {0};
    int oracle_size = 0;

    total++;

    if (oracle_encode(asm_text, oracle, &oracle_size) != 0) {
        fprintf(stderr, "ORACLE FAIL: %s\n  asm: %s\n", desc, asm_text);
        mismatches++;
        return;
    }

    if (oracle_size != our_size) {
        fprintf(stderr, "SIZE MISMATCH: %s\n  asm: %s\n"
                "  our=%d oracle=%d\n", desc, asm_text, our_size, oracle_size);
        mismatches++;
        return;
    }

    if (memcmp(our_bytes, oracle, (size_t)our_size) != 0) {
        fprintf(stderr, "BYTES MISMATCH: %s\n  asm: %s\n  our:", desc, asm_text);
        for (int i = 0; i < our_size; i++)
            fprintf(stderr, " %02x", our_bytes[i]);
        fprintf(stderr, "\n  ref:");
        for (int i = 0; i < oracle_size; i++)
            fprintf(stderr, " %02x", oracle[i]);
        fprintf(stderr, "\n");
        mismatches++;
    }
}

/* Helpers that emit one instruction and run check() */
static void test_sop2(GcnSop2Op op, uint8_t sdst, uint8_t s1, uint8_t s0,
                      const char *mnemonic)
{
    uint8_t buf[4]; size_t pos = 0;
    gcn_emit_sop2(buf, &pos, sizeof buf, op, sdst, s1, s0);
    char asm_text[128];
    snprintf(asm_text, sizeof asm_text, "%s s%u, s%u, s%u",
             mnemonic, sdst, s0, s1);
    check(mnemonic, asm_text, buf, 4);
}

static void test_sop1(GcnSop1Op op, uint8_t sdst, uint8_t s0,
                      const char *mnemonic)
{
    uint8_t buf[4]; size_t pos = 0;
    gcn_emit_sop1(buf, &pos, sizeof buf, op, sdst, s0);
    char asm_text[128];
    snprintf(asm_text, sizeof asm_text, "%s s%u, s%u", mnemonic, sdst, s0);
    check(mnemonic, asm_text, buf, 4);
}

static void test_sopk(GcnSopkOp op, uint8_t sdst, uint16_t imm,
                      const char *mnemonic)
{
    uint8_t buf[4]; size_t pos = 0;
    gcn_emit_sopk(buf, &pos, sizeof buf, op, sdst, imm);
    char asm_text[128];
    snprintf(asm_text, sizeof asm_text, "%s s%u, 0x%x", mnemonic, sdst, imm);
    check(mnemonic, asm_text, buf, 4);
}

static void test_sopc(GcnSopcOp op, uint8_t s1, uint8_t s0,
                      const char *mnemonic)
{
    uint8_t buf[4]; size_t pos = 0;
    gcn_emit_sopc(buf, &pos, sizeof buf, op, s1, s0);
    char asm_text[128];
    snprintf(asm_text, sizeof asm_text, "%s s%u, s%u", mnemonic, s0, s1);
    check(mnemonic, asm_text, buf, 4);
}

static void test_sopp_imm(GcnSoppOp op, uint16_t imm, const char *mnemonic,
                          const char *extra)
{
    uint8_t buf[4]; size_t pos = 0;
    gcn_emit_sopp(buf, &pos, sizeof buf, op, imm);
    char asm_text[128];
    if (extra)
        snprintf(asm_text, sizeof asm_text, "%s", extra);
    else
        snprintf(asm_text, sizeof asm_text, "%s 0x%x", mnemonic, imm);
    check(mnemonic, asm_text, buf, 4);
}

static void test_vop1(GcnVop1Op op, uint8_t vd, uint32_t src0,
                      const char *mnemonic, const char *src0_str)
{
    uint8_t buf[4]; size_t pos = 0;
    gcn_emit_vop1(buf, &pos, sizeof buf, op, vd, src0);
    char asm_text[128];
    snprintf(asm_text, sizeof asm_text, "%s_e32 v%u, %s", mnemonic, vd, src0_str);
    check(mnemonic, asm_text, buf, 4);
}

static void test_vop2(GcnVop2Op op, uint8_t vd, uint8_t vs1, uint32_t src0,
                      const char *mnemonic, const char *src0_str)
{
    uint8_t buf[4]; size_t pos = 0;
    gcn_emit_vop2(buf, &pos, sizeof buf, op, vd, vs1, src0);
    char asm_text[128];
    snprintf(asm_text, sizeof asm_text, "%s_e32 v%u, %s, v%u",
             mnemonic, vd, src0_str, vs1);
    check(mnemonic, asm_text, buf, 4);
}

static void test_vopc(GcnVopcOp op, uint8_t vs1, uint32_t src0,
                      const char *mnemonic, const char *src0_str)
{
    uint8_t buf[4]; size_t pos = 0;
    gcn_emit_vopc(buf, &pos, sizeof buf, op, vs1, src0);
    char asm_text[128];
    snprintf(asm_text, sizeof asm_text, "%s_e32 vcc, %s, v%u",
             mnemonic, src0_str, vs1);
    check(mnemonic, asm_text, buf, 4);
}

/* ---- Run all the tests ---- */

static void run_sop2_suite(void)
{
    /* Vary sdst, ssrc0, ssrc1 over a few values */
    static const struct { uint8_t d, s0, s1; } regs[] = {
        {0, 1, 2}, {3, 4, 5}, {7, 0, 3}, {10, 5, 7}, {15, 8, 9},
        {20, 10, 11}, {0, 0, 0}, {1, 2, 3}
    };
    static const struct { GcnSop2Op op; const char *mn; } ops[] = {
        {GCN_SOP2_S_ADD_U32, "s_add_u32"},
        {GCN_SOP2_S_SUB_U32, "s_sub_u32"},
        {GCN_SOP2_S_ADD_I32, "s_add_i32"},
        {GCN_SOP2_S_AND_B32, "s_and_b32"},
        {GCN_SOP2_S_OR_B32,  "s_or_b32"},
        {GCN_SOP2_S_XOR_B32, "s_xor_b32"},
        {GCN_SOP2_S_LSHL_B32,"s_lshl_b32"},
        {GCN_SOP2_S_LSHR_B32,"s_lshr_b32"},
        {GCN_SOP2_S_ASHR_I32,"s_ashr_i32"},
        {GCN_SOP2_S_MUL_I32, "s_mul_i32"},
    };
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++)
        for (size_t j = 0; j < sizeof regs / sizeof *regs; j++)
            test_sop2(ops[i].op, regs[j].d, regs[j].s1, regs[j].s0, ops[i].mn);
}

static void run_sop1_suite(void)
{
    static const struct { uint8_t d, s0; } regs[] = {
        {0,1}, {1,0}, {2,3}, {5,4}, {10,7}
    };
    static const struct { GcnSop1Op op; const char *mn; } ops[] = {
        {GCN_SOP1_S_MOV_B32, "s_mov_b32"},
        {GCN_SOP1_S_NOT_B32, "s_not_b32"},
        {GCN_SOP1_S_BREV_B32,"s_brev_b32"},
        {GCN_SOP1_S_ABS_I32, "s_abs_i32"},
    };
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++)
        for (size_t j = 0; j < sizeof regs / sizeof *regs; j++)
            test_sop1(ops[i].op, regs[j].d, regs[j].s0, ops[i].mn);
}

static void run_sopk_suite(void)
{
    static const struct { uint8_t d; uint16_t imm; } vals[] = {
        {0, 0}, {0, 1}, {0, 42}, {0, 0xFFFF}, {0, 0x7FFF},
        {1, 100}, {5, 0x1234}, {10, 0}, {3, 999}
    };
    for (size_t j = 0; j < sizeof vals / sizeof *vals; j++) {
        test_sopk(GCN_SOPK_S_MOVK_I32, vals[j].d, vals[j].imm, "s_movk_i32");
        test_sopk(GCN_SOPK_S_ADDK_I32, vals[j].d, vals[j].imm, "s_addk_i32");
    }
}

static void run_sopc_suite(void)
{
    static const struct { uint8_t s0, s1; } regs[] = {
        {0,1}, {1,2}, {3,4}, {5,0}
    };
    static const struct { GcnSopcOp op; const char *mn; } ops[] = {
        {GCN_SOPC_S_CMP_EQ_U32, "s_cmp_eq_u32"},
        {GCN_SOPC_S_CMP_LG_U32, "s_cmp_lg_u32"},
        {GCN_SOPC_S_CMP_GT_I32, "s_cmp_gt_i32"},
        {GCN_SOPC_S_CMP_LT_I32, "s_cmp_lt_i32"},
    };
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++)
        for (size_t j = 0; j < sizeof regs / sizeof *regs; j++)
            test_sopc(ops[i].op, regs[j].s1, regs[j].s0, ops[i].mn);
}

static void run_sopp_suite(void)
{
    test_sopp_imm(GCN_SOPP_S_ENDPGM, 0, "s_endpgm", "s_endpgm");
    test_sopp_imm(GCN_SOPP_S_NOP, 0, "s_nop", "s_nop 0");
    test_sopp_imm(GCN_SOPP_S_BARRIER, 0, "s_barrier", "s_barrier");
    /* waitcnt: lgkmcnt(0) vmcnt(0) expcnt(0) = 0xC07F per AMD encoding */
    test_sopp_imm(GCN_SOPP_S_WAITCNT, 0, "s_waitcnt", "s_waitcnt 0");
    test_sopp_imm(GCN_SOPP_S_WAITCNT, 0xC07F, "s_waitcnt_c07f", "s_waitcnt 0xc07f");
    /* branch: relative word offset */
    test_sopp_imm(GCN_SOPP_S_BRANCH, 10, "s_branch", "s_branch 10");
    test_sopp_imm(GCN_SOPP_S_CBRANCH_SCC0, 5, "s_cbranch_scc0", "s_cbranch_scc0 5");
    test_sopp_imm(GCN_SOPP_S_CBRANCH_VCCNZ, 3, "s_cbranch_vccnz", "s_cbranch_vccnz 3");
}

static void run_smem_suite(void)
{
    uint8_t buf[8]; size_t pos;
    uint8_t oracle[8]; int nbytes;
    char asm_text[128];

    /* s_load_dword s0, s[2:3], 0x0 */
    pos = 0;
    gcn_emit_smem(buf, &pos, sizeof buf, GCN_SMEM_S_LOAD_DWORD, 0, 1, 0, 0);
    oracle_encode("s_load_dword s0, s[2:3], 0x0", oracle, &nbytes);
    total++;
    if (pos != 8 || memcmp(buf, oracle, 8) != 0) {
        fprintf(stderr, "MISMATCH: s_load_dword s0,s[2:3],0\n"
                "  our: %02x %02x %02x %02x %02x %02x %02x %02x\n"
                "  ref: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
                oracle[0],oracle[1],oracle[2],oracle[3],oracle[4],oracle[5],oracle[6],oracle[7]);
        mismatches++;
    }

    /* s_load_dword s1, s[2:3], 0x0 */
    pos = 0;
    gcn_emit_smem(buf, &pos, sizeof buf, GCN_SMEM_S_LOAD_DWORD, 1, 1, 0, 0);
    snprintf(asm_text, sizeof asm_text, "s_load_dword s1, s[2:3], 0x0");
    check("s_load_dword s1", asm_text, buf, 8);

    /* s_load_dword s0, s[4:5], 0x0 */
    pos = 0;
    gcn_emit_smem(buf, &pos, sizeof buf, GCN_SMEM_S_LOAD_DWORD, 0, 2, 0, 0);
    snprintf(asm_text, sizeof asm_text, "s_load_dword s0, s[4:5], 0x0");
    check("s_load_dword s0,s[4:5]", asm_text, buf, 8);

    /* s_load_dword s0, s[2:3], 0x8 */
    pos = 0;
    gcn_emit_smem(buf, &pos, sizeof buf, GCN_SMEM_S_LOAD_DWORD, 0, 1, 8, 0);
    snprintf(asm_text, sizeof asm_text, "s_load_dword s0, s[2:3], 0x8");
    check("s_load_dword s0,s[2:3],8", asm_text, buf, 8);

    /* s_load_dwordx2 s[0:1], s[2:3], 0x0 */
    pos = 0;
    gcn_emit_smem(buf, &pos, sizeof buf, GCN_SMEM_S_LOAD_DWORDX2, 0, 1, 0, 0);
    snprintf(asm_text, sizeof asm_text, "s_load_dwordx2 s[0:1], s[2:3], 0x0");
    check("s_load_dwordx2", asm_text, buf, 8);

    /* s_buffer_load_dword s0, s[4:7], 0x0 */
    pos = 0;
    gcn_emit_smem(buf, &pos, sizeof buf, GCN_SMEM_S_BUFFER_LOAD_DWORD, 0, 2, 0, 0);
    snprintf(asm_text, sizeof asm_text, "s_buffer_load_dword s0, s[4:7], 0x0");
    check("s_buffer_load_dword", asm_text, buf, 8);
}

static void run_vop1_suite(void)
{
    static const struct { uint8_t vd; uint32_t s0; } regs[] = {
        {0, GCN_VGPR(1)}, {1, GCN_VGPR(2)}, {3, GCN_VGPR(0)},
        {5, GCN_SGPR(0)}, {0, GCN_SGPR(4)}, {2, GCN_VGPR(5)}
    };
    /* mnemonic, llvm asm with explicit src formatting */
    for (size_t j = 0; j < sizeof regs / sizeof *regs; j++) {
        char src_str[32];
        uint32_t s0 = regs[j].s0;
        if (s0 >= 256)
            snprintf(src_str, sizeof src_str, "v%u", (unsigned)(s0 - 256));
        else
            snprintf(src_str, sizeof src_str, "s%u", (unsigned)s0);
        test_vop1(GCN_VOP1_V_MOV_B32, regs[j].vd, s0, "v_mov_b32", src_str);
        test_vop1(GCN_VOP1_V_NOT_B32, regs[j].vd, s0, "v_not_b32", src_str);
        test_vop1(GCN_VOP1_V_BFREV_B32, regs[j].vd, s0, "v_bfrev_b32", src_str);
    }
    /* Inline constants */
    test_vop1(GCN_VOP1_V_MOV_B32, 0, GCN_IMM(0), "v_mov_b32", "0");
    test_vop1(GCN_VOP1_V_MOV_B32, 0, GCN_IMM(1), "v_mov_b32", "1");
}

static void run_vop2_suite(void)
{
    static const struct { uint8_t vd, vs1; uint32_t s0; } regs[] = {
        {0, 2, GCN_VGPR(1)}, {1, 3, GCN_VGPR(2)}, {5, 1, GCN_VGPR(0)},
        {3, 4, GCN_SGPR(0)}, {0, 5, GCN_IMM(1)}
    };
    static const struct { GcnVop2Op op; const char *mn; } ops[] = {
        {GCN_VOP2_V_ADD_F32,    "v_add_f32"},
        {GCN_VOP2_V_SUB_F32,    "v_sub_f32"},
        {GCN_VOP2_V_MUL_F32,    "v_mul_f32"},
        {GCN_VOP2_V_MAX_F32,    "v_max_f32"},
        {GCN_VOP2_V_MIN_F32,    "v_min_f32"},
        {GCN_VOP2_V_AND_B32,    "v_and_b32"},
        {GCN_VOP2_V_OR_B32,     "v_or_b32"},
        {GCN_VOP2_V_XOR_B32,    "v_xor_b32"},
        {GCN_VOP2_V_LSHLREV_B32,"v_lshlrev_b32"},
        {GCN_VOP2_V_LSHRREV_B32,"v_lshrrev_b32"},
        {GCN_VOP2_V_ASHRREV_I32,"v_ashrrev_i32"},
        {GCN_VOP2_V_MUL_U32_U24,"v_mul_u32_u24"},
    };
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) {
        for (size_t j = 0; j < sizeof regs / sizeof *regs; j++) {
            char src_str[32];
            uint32_t s0 = regs[j].s0;
            if (s0 >= 256)
                snprintf(src_str, sizeof src_str, "v%u", (unsigned)(s0-256));
            else if (s0 >= 128)
                snprintf(src_str, sizeof src_str, "%u", (unsigned)(s0-128)); /* inline const */
            else
                snprintf(src_str, sizeof src_str, "s%u", (unsigned)s0);
            test_vop2(ops[i].op, regs[j].vd, regs[j].vs1, s0,
                      ops[i].mn, src_str);
        }
    }
}

static void run_vopc_suite(void)
{
    static const struct { uint8_t vs1; uint32_t src0; } regs[] = {
        {1, GCN_VGPR(0)}, {2, GCN_VGPR(1)}, {3, GCN_VGPR(2)},
        {0, GCN_VGPR(3)}, {5, GCN_VGPR(4)}
    };
    static const struct { GcnVopcOp op; const char *mn; } ops[] = {
        {GCN_VOPC_V_CMP_EQ_F32, "v_cmp_eq_f32"},
        {GCN_VOPC_V_CMP_LT_F32, "v_cmp_lt_f32"},
        {GCN_VOPC_V_CMP_GE_F32, "v_cmp_ge_f32"},
        {GCN_VOPC_V_CMP_GT_F32, "v_cmp_gt_f32"},
        {GCN_VOPC_V_CMP_EQ_I32, "v_cmp_eq_i32"},
        {GCN_VOPC_V_CMP_LT_I32, "v_cmp_lt_i32"},
        {GCN_VOPC_V_CMP_GE_U32, "v_cmp_ge_u32"},
        {GCN_VOPC_V_CMP_LT_U32, "v_cmp_lt_u32"},
    };
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) {
        for (size_t j = 0; j < sizeof regs / sizeof *regs; j++) {
            char src_str[32];
            uint32_t s0 = regs[j].src0;
            if (s0 >= 256)
                snprintf(src_str, sizeof src_str, "v%u", (unsigned)(s0-256));
            else
                snprintf(src_str, sizeof src_str, "s%u", (unsigned)s0);
            test_vopc(ops[i].op, regs[j].vs1, s0, ops[i].mn, src_str);
        }
    }
}

static void run_mubuf_suite(void)
{
    uint8_t buf[8]; size_t pos; char asm_text[256];

    struct { uint8_t vdst, vaddr, srsrc_d4, soffset; uint16_t off;
             int offen; const char *desc; } cases[] = {
        {0, 0, 0, 128, 0, 1, "v0,v0,s[0:3],0 offen"},
        {1, 0, 0, 128, 0, 1, "v1,v0,s[0:3],0 offen"},
        {0, 1, 0, 128, 0, 1, "v0,v1,s[0:3],0 offen"},
        {0, 0, 1, 128, 0, 1, "v0,v0,s[4:7],0 offen"},
        {0, 0, 0, 144, 0, 1, "v0,v0,s[0:3],16 offen"}, /* soffset=16=IMM(16)=144 */
        /* non-offen case omitted: LLVM requires explicit offset mode */
    };

    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        pos = 0;
        gcn_emit_mubuf(buf, &pos, sizeof buf,
                       GCN_MUBUF_BUFFER_LOAD_DWORD,
                       cases[k].vdst, cases[k].vaddr,
                       cases[k].srsrc_d4, cases[k].soffset,
                       cases[k].off, cases[k].offen, 0, 0, 0);
        /* build asm: soffset field becomes SGPR or inline const in LLVM syntax */
        uint8_t soff = cases[k].soffset;
        const char *soff_str;
        char soff_buf[16];
        if (soff >= 128 && soff <= 192) {
            snprintf(soff_buf, sizeof soff_buf, "%u", (unsigned)(soff - 128));
            soff_str = soff_buf;
        } else {
            snprintf(soff_buf, sizeof soff_buf, "s%u", (unsigned)soff);
            soff_str = soff_buf;
        }
        snprintf(asm_text, sizeof asm_text,
                 "buffer_load_dword v%u, v%u, s[%u:%u], %s%s",
                 cases[k].vdst, cases[k].vaddr,
                 cases[k].srsrc_d4 * 4, cases[k].srsrc_d4 * 4 + 3,
                 soff_str,
                 cases[k].offen ? " offen" : "");
        check(cases[k].desc, asm_text, buf, 8);
    }

    /* buffer_store_dword */
    pos = 0;
    gcn_emit_mubuf(buf, &pos, sizeof buf,
                   GCN_MUBUF_BUFFER_STORE_DWORD,
                   0, 1, 0, 128, 0, 1, 0, 0, 0);
    check("buffer_store_dword v0,v1,s[0:3],0 offen",
          "buffer_store_dword v0, v1, s[0:3], 0 offen", buf, 8);
}

static void run_ds_suite(void)
{
    uint8_t buf[8]; size_t pos; char asm_text[128];

    struct { GcnDsOp op; uint8_t vdst, addr, d0, d1, o0, o1;
             const char *desc; const char *asmtxt; } cases[] = {
        {GCN_DS_DS_READ_B32,   0, 1, 0, 0, 0, 0,
            "ds_read_b32 v0,v1", "ds_read_b32 v0, v1"},
        {GCN_DS_DS_READ_B32,   1, 1, 0, 0, 0, 0,
            "ds_read_b32 v1,v1", "ds_read_b32 v1, v1"},
        {GCN_DS_DS_WRITE_B32,  0, 0, 1, 0, 0, 0,
            "ds_write_b32 v0,v1", "ds_write_b32 v0, v1"},
        {GCN_DS_DS_READ_B32,   2, 3, 0, 0, 0, 0,
            "ds_read_b32 v2,v3", "ds_read_b32 v2, v3"},
        {GCN_DS_DS_WRITE_B32,  0, 2, 4, 0, 0, 0,
            "ds_write_b32 v2,v4", "ds_write_b32 v2, v4"},
    };

    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        pos = 0;
        gcn_emit_ds(buf, &pos, sizeof buf,
                    cases[k].op, cases[k].vdst, cases[k].addr,
                    cases[k].d0, cases[k].d1, cases[k].o0, cases[k].o1, 0);
        check(cases[k].desc, cases[k].asmtxt, buf, 8);
    }

    (void)asm_text;
}

static void run_flat_suite(void)
{
    uint8_t buf[8]; size_t pos; char asm_text[128];

    /* flat_load_dword v0, v[0:1] */
    pos = 0;
    gcn_emit_flat(buf, &pos, sizeof buf, GCN_FLAT_FLAT_LOAD_DWORD, 0, 0, 0, 0, 0);
    check("flat_load_dword v0,v[0:1]",
          "flat_load_dword v0, v[0:1]", buf, 8);

    /* flat_store_dword v[0:1], v2 */
    pos = 0;
    gcn_emit_flat(buf, &pos, sizeof buf, GCN_FLAT_FLAT_STORE_DWORD, 0, 2, 0, 0, 0);
    check("flat_store_dword v[0:1],v2",
          "flat_store_dword v[0:1], v2", buf, 8);

    /* flat_load_dword v2, v[4:5] */
    pos = 0;
    gcn_emit_flat(buf, &pos, sizeof buf, GCN_FLAT_FLAT_LOAD_DWORD, 2, 0, 4, 0, 0);
    snprintf(asm_text, sizeof asm_text, "flat_load_dword v2, v[4:5]");
    check("flat_load_dword v2,v[4:5]", asm_text, buf, 8);

    /* flat_store_dword v[2:3], v5 */
    pos = 0;
    gcn_emit_flat(buf, &pos, sizeof buf, GCN_FLAT_FLAT_STORE_DWORD, 0, 5, 2, 0, 0);
    snprintf(asm_text, sizeof asm_text, "flat_store_dword v[2:3], v5");
    check("flat_store_dword v[2:3],v5", asm_text, buf, 8);
}

int main(void)
{
    printf("GCN encoder oracle test (llvm-mc-18 -arch=amdgcn -mcpu=gfx803)\n");

    run_sop2_suite();
    run_sop1_suite();
    run_sopk_suite();
    run_sopc_suite();
    run_sopp_suite();
    run_smem_suite();
    run_vop1_suite();
    run_vop2_suite();
    run_vopc_suite();
    run_mubuf_suite();
    run_ds_suite();
    run_flat_suite();

    printf("GCN_ENC_TEST: %d instructions, %d mismatches\n", total, mismatches);
    if (mismatches == 0) {
        printf("GCN_ENC_TEST: PASS\n");
        return 0;
    } else {
        printf("GCN_ENC_TEST: FAIL\n");
        return 1;
    }
}
