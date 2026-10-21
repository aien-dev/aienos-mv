/* native/kernel/gpu/kernels/allen/ka.h -- small gfx803 assembler for the Allen kernels.
 *
 * Wraps gcn_enc with labels, branch fixups, literal and inline-constant
 * selection, and the VOP2 -> VOP3 fallback. Kernel-side code: no floats.
 * Float constants are passed as their IEEE binary32 bit patterns (KK()).
 */
#ifndef KA_H
#define KA_H

#include <stdint.h>
#include <stddef.h>
#include "../../gcn_enc.h"

typedef uint64_t ko; /* operand: 9-bit source code, or KK() constant */

#define KS(n)   ((ko)(uint32_t)(n))
#define KV(n)   ((ko)(256u + (uint32_t)(n)))
#define KVCC    ((ko)106u)
#define KEXEC   ((ko)126u)
#define KM0     ((ko)124u)
#define KK(x)   (((ko)1u << 32) | (ko)(uint32_t)(x)) /* inline if possible, else literal */

#define KA_MAX_WORDS 32768
#define KA_MAX_LABELS 256
#define KA_MAX_FIX 512

/* fixed SGPRs every kernel uses (see allen_kern.c) */
#define KA_S_KARG   0   /* s[0:1] kernarg pointer (user SGPRs) */
#define KA_S_TGID   2   /* s2 workgroup id x */
#define KA_S_DESC   4   /* s[4:7] image buffer descriptor (from kernarg) */
#define KA_S_ARG    8   /* s[8:19] kernel parameters p0..p11 */
#define KA_S_MASK0  96  /* s[96:97] compare mask scratch (math routines) */
#define KA_S_MASK1  98  /* s[98:99] */
#define KA_S_K0     100 /* constant scratch for VOP3 operands */
#define KA_S_K1     101

typedef struct {
    uint32_t w[KA_MAX_WORDS];
    uint32_t n;
    const char *err;
    uint32_t lab[KA_MAX_LABELS];
    uint32_t nlab;
    uint32_t fix_at[KA_MAX_FIX];
    uint16_t fix_lab[KA_MAX_FIX];
    uint32_t nfix;
    uint32_t max_vgpr, max_sgpr; /* highest register index used + 1 */
} ka_t;

void ka_init(ka_t *k);
int  ka_finish(ka_t *k);          /* resolves branches; 0 ok, -1 error (k->err) */

int  ka_label(ka_t *k);
void ka_bind(ka_t *k, int l);
void ka_branch(ka_t *k, GcnSoppOp op, int l);

/* scalar */
void ka_sop2(ka_t *k, GcnSop2Op op, uint32_t d, ko a, ko b);
void ka_sop1(ka_t *k, GcnSop1Op op, uint32_t d, ko a);
void ka_sopk(ka_t *k, GcnSopkOp op, uint32_t d, uint16_t imm);
void ka_sopc(ka_t *k, GcnSopcOp op, ko a, ko b);
void ka_sopp(ka_t *k, GcnSoppOp op, uint16_t imm);
void ka_smov(ka_t *k, uint32_t d, ko a);           /* s_mov_b32 */
void ka_smem(ka_t *k, GcnSmemOp op, uint32_t d, uint32_t sbase, uint32_t imm);
void ka_smem_s(ka_t *k, GcnSmemOp op, uint32_t d, uint32_t sbase, uint32_t soff);
void ka_wait(ka_t *k, int vm, int exp, int lgkm);  /* -1 = do not wait on that counter */
void ka_end(ka_t *k);

/* vector */
void ka_vop1(ka_t *k, GcnVop1Op op, uint32_t vd, ko a);
void ka_vop2(ka_t *k, GcnVop2Op op, uint32_t vd, ko a, ko b);
/* VOP3a by VOP3 opcode (0x100+vop2, 0x140+vop1, native); neg/abs bit i = source i */
void ka_vop3(ka_t *k, uint32_t op3, uint32_t vd, ko a, ko b, ko c, int neg, int abs);
/* compare: sd = 106 (VCC) uses VOPC e32 when possible, else VOP3 into SGPR pair sd */
void ka_vcmp(ka_t *k, GcnVopcOp op, uint32_t sd, ko a, ko b);
/* vd = mask ? b : a (mask = SGPR pair or VCC) */
void ka_cnd(ka_t *k, uint32_t vd, ko a, ko b, uint32_t mask);
void ka_fma(ka_t *k, uint32_t vd, ko a, ko b, ko c, int neg);

/* memory */
void ka_buf(ka_t *k, GcnMubufOp op, uint32_t vdata, uint32_t vaddr, uint32_t srsrc,
            ko soffset, uint32_t imm, int offen, int glc);
void ka_ds(ka_t *k, GcnDsOp op, uint32_t vdst, uint32_t addr, uint32_t d0, uint32_t d1,
           uint8_t off0, uint8_t off1);

/* VOP3 native opcodes used (gfx8, llvm-mc checked) */
#define K3_MAD_U32_U24 0x1C3u
#define K3_BFE_U32     0x1C8u
#define K3_FMA_F32     0x1CBu
#define K3_MUL_LO_U32  0x285u
#define K3_READLANE    0x289u
#define K3_MBCNT_LO    0x28Cu
#define K3_MBCNT_HI    0x28Du

#endif
