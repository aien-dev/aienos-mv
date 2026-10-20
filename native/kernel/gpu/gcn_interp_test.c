/* native/kernel/gpu/gcn_interp_test.c -- Test runner: GCN interpreter + all kernels.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -I. -I.. gcn_enc.c gcn_interp.c pm4.c \
 *       kernels/fill_rect.c kernels/blit_alpha.c \
 *       kernels/star_splat.c kernels/bh_bend.c \
 *       gcn_interp_test.c -lm -o gcn_interp_test
 *
 * Run:   ./gcn_interp_test
 */
#include "gcn_enc.h"
#include "gcn_interp.h"
#include "pm4.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Kernel test functions declared here */
int fill_rect_test(void);
int blit_alpha_test(void);
int star_splat_test(void);
int bh_bend_test(void);

/* Kernel instruction count functions */
size_t fill_rect_encode(uint8_t *buf, size_t max);
size_t blit_alpha_encode(uint8_t *buf, size_t max);
size_t star_splat_encode(uint8_t *buf, size_t max);
size_t bh_bend_encode(uint8_t *buf, size_t max);

/* ---- Interpreter basic self-test ---- */
static int interp_selftest(void)
{
    /* Emit a tiny shader: s0 = 42, s1 = 7, s0 = s0 + s1, s_endpgm */
    uint8_t code_bytes[64];
    size_t pos = 0;

    gcn_emit_sopk(code_bytes, &pos, sizeof code_bytes, GCN_SOPK_S_MOVK_I32, 0, 42);
    gcn_emit_sopk(code_bytes, &pos, sizeof code_bytes, GCN_SOPK_S_MOVK_I32, 1, 7);
    gcn_emit_sop2(code_bytes, &pos, sizeof code_bytes, GCN_SOP2_S_ADD_U32, 0, 1, 0);
    gcn_emit_sopp(code_bytes, &pos, sizeof code_bytes, GCN_SOPP_S_ENDPGM, 0);

    uint32_t code[16];
    memcpy(code, code_bytes, pos);
    size_t ndw = pos / 4;

    gcn_wave_t w;
    gcn_wave_init(&w);
    gcn_wave_run(&w, code, ndw);

    if (w.sgpr[0] != 49) {
        fprintf(stderr, "interp_selftest FAIL: s0=%u expected 49\n", w.sgpr[0]);
        return 1;
    }
    printf("interp_selftest: PASS (s0 = %u = 42+7)\n", w.sgpr[0]);
    return 0;
}

/* ---- VOP2 self-test ---- */
static int vop2_selftest(void)
{
    uint8_t code_bytes[128];
    size_t pos = 0;

    /* v0 = 10.0f, v1 = 3.0f, v2 = v0 + v1, v3 = v0 * v1 */
    uint32_t ten  = 0x41200000u; /* 10.0f */
    uint32_t three= 0x40400000u; /* 3.0f */

    gcn_emit_vop1(code_bytes, &pos, sizeof code_bytes, GCN_VOP1_V_MOV_B32, 0, GCN_LITERAL);
    /* literal = 10.0f follows */
    code_bytes[pos++] = (ten)       & 0xff;
    code_bytes[pos++] = (ten >>  8) & 0xff;
    code_bytes[pos++] = (ten >> 16) & 0xff;
    code_bytes[pos++] = (ten >> 24) & 0xff;

    gcn_emit_vop1(code_bytes, &pos, sizeof code_bytes, GCN_VOP1_V_MOV_B32, 1, GCN_LITERAL);
    code_bytes[pos++] = (three)       & 0xff;
    code_bytes[pos++] = (three >>  8) & 0xff;
    code_bytes[pos++] = (three >> 16) & 0xff;
    code_bytes[pos++] = (three >> 24) & 0xff;

    gcn_emit_vop2(code_bytes, &pos, sizeof code_bytes,
                  GCN_VOP2_V_ADD_F32, 2, 1, GCN_VGPR(0)); /* v2 = v0 + v1 */
    gcn_emit_vop2(code_bytes, &pos, sizeof code_bytes,
                  GCN_VOP2_V_MUL_F32, 3, 1, GCN_VGPR(0)); /* v3 = v0 * v1 */
    gcn_emit_sopp(code_bytes, &pos, sizeof code_bytes, GCN_SOPP_S_ENDPGM, 0);

    uint32_t code[32];
    memcpy(code, code_bytes, pos);
    size_t ndw = pos / 4;

    gcn_wave_t w;
    gcn_wave_init(&w);
    gcn_wave_set_threads_1d(&w, 4, 0, 0);
    gcn_wave_run(&w, code, ndw);

    uint32_t v2 = w.vgpr[0][2]; /* lane 0 */
    uint32_t v3 = w.vgpr[0][3];
    float sum, prod;
    __builtin_memcpy(&sum,  &v2, 4);
    __builtin_memcpy(&prod, &v3, 4);

    int ok = (sum == 13.0f && prod == 30.0f);
    if (!ok)
        fprintf(stderr, "vop2_selftest FAIL: v2=%f (exp 13), v3=%f (exp 30)\n",
                (double)sum, (double)prod);
    else
        printf("vop2_selftest: PASS (10+3=%g, 10*3=%g)\n",
               (double)sum, (double)prod);
    return ok ? 0 : 1;
}

/* ---- PM4 self-test ---- */
static int pm4_selftest(void)
{
    uint32_t ring_buf[64];
    pm4_ring_t ring = { ring_buf, 64, 0 };

    /* SET_SH_REG for COMPUTE_PGM_LO and COMPUTE_PGM_HI */
    uint32_t addrs[2] = { 0xDEAD0000u, 0x00000001u };
    if (pm4_set_sh_reg(&ring, mmCOMPUTE_PGM_LO, addrs, 2) != 0) {
        fprintf(stderr, "pm4_selftest FAIL: set_sh_reg overflow\n");
        return 1;
    }
    /* Verify header: TYPE3, IT_SET_SH_REG, count=3 */
    uint32_t hdr = ring_buf[0];
    int ok = ((hdr >> 30) == 3) &&
             (((hdr >> 8) & 0xff) == PM4_IT_SET_SH_REG) &&
             (((hdr >> 16) & 0x3fff) == 2); /* count-1 = 2 */

    /* DISPATCH_DIRECT */
    if (pm4_dispatch_direct(&ring, 16, 1, 1, PM4_DISPATCH_INITIATOR_CS_EN) != 0)
        ok = 0;

    /* ACQUIRE_MEM */
    if (pm4_acquire_mem(&ring, 0x00600000u) != 0)
        ok = 0;

    if (ok)
        printf("pm4_selftest: PASS (ring_wptr=%u DWORDs)\n", ring.wptr);
    else
        fprintf(stderr, "pm4_selftest FAIL\n");
    return ok ? 0 : 1;
}

/* ---- Instruction count report ---- */
static void report_insn_counts(void)
{
    uint8_t buf[512];
    size_t sizes[4];
    sizes[0] = fill_rect_encode(buf, sizeof buf);
    sizes[1] = blit_alpha_encode(buf, sizeof buf);
    sizes[2] = star_splat_encode(buf, sizeof buf);
    sizes[3] = bh_bend_encode(buf, sizeof buf);
    const char *names[] = {"fill_rect", "blit_alpha", "star_splat", "bh_bend"};
    printf("\nKernel instruction counts (encoded bytes / 4 = DWORDs):\n");
    for (int i = 0; i < 4; i++)
        printf("  %-12s  %3zu bytes  = %zu instructions (32-bit equiv)\n",
               names[i], sizes[i], sizes[i] / 4);
}

/* ---- main ---- */
int main(void)
{
    int fails = 0;

    printf("=== GCN interpreter + kernel tests ===\n\n");

    fails += interp_selftest();
    fails += vop2_selftest();
    fails += pm4_selftest();

    printf("\n--- kernel reference + interpreter tests ---\n");
    fails += fill_rect_test();
    fails += blit_alpha_test();
    fails += star_splat_test();
    fails += bh_bend_test();

    report_insn_counts();

    printf("\n");
    if (fails == 0)
        printf("GCN_INTERP_TEST: PASS\n");
    else
        printf("GCN_INTERP_TEST: FAIL (%d failures)\n", fails);
    return fails ? 1 : 0;
}
