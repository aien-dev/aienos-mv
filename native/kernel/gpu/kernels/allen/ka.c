/* native/kernel/gpu/kernels/allen/ka.c -- see ka.h. No floats. */
#include "ka.h"

static void fail(ka_t *k, const char *m) { if (!k->err) k->err = m; }

static void put(ka_t *k, uint32_t x)
{
    if (k->n >= KA_MAX_WORDS) { fail(k, "ka: code buffer full"); return; }
    k->w[k->n++] = x;
}

/* emit through gcn_enc into a scratch buffer, then copy words */
#define EMIT(call) do { uint8_t b_[8]; size_t p_ = 0; size_t m_ = 8; uint8_t *buf = b_; \
    (void)m_; if (call) fail(k, "ka: encoder overflow"); \
    for (size_t i_ = 0; i_ < p_; i_ += 4) put(k, (uint32_t)b_[i_] | ((uint32_t)b_[i_+1] << 8) | \
        ((uint32_t)b_[i_+2] << 16) | ((uint32_t)b_[i_+3] << 24)); } while (0)
#define B buf, &p_, m_

void ka_init(ka_t *k)
{
    k->n = 0; k->err = 0; k->nlab = 0; k->nfix = 0; k->max_vgpr = 0; k->max_sgpr = 0;
}

static void note(ka_t *k, uint32_t code, uint32_t cnt)
{
    if (code >= 256 && code < 512) { if (code - 256 + cnt > k->max_vgpr) k->max_vgpr = code - 256 + cnt; }
    else if (code < 104) { if (code + cnt > k->max_sgpr) k->max_sgpr = code + cnt; }
}

static int is_v(ko o) { return !(o >> 32) && (uint32_t)o >= 256 && (uint32_t)o < 512; }

/* operand -> 9-bit code; literals collected in *lit */
static uint32_t enc(ka_t *k, ko o, uint32_t *lit, int *nlit)
{
    if (!(o >> 32)) { note(k, (uint32_t)o, 1); return (uint32_t)o; }
    uint32_t x = (uint32_t)o;
    if (x <= 64) return 128u + x;
    if ((int32_t)x < 0 && (int32_t)x >= -16) return 192u + (uint32_t)(-(int32_t)x);
    switch (x) {
    case 0x3F000000u: return 240; case 0xBF000000u: return 241;
    case 0x3F800000u: return 242; case 0xBF800000u: return 243;
    case 0x40000000u: return 244; case 0xC0000000u: return 245;
    case 0x40800000u: return 246; case 0xC0800000u: return 247;
    case 0x3E22F983u: return 248;
    default: break;
    }
    if (*nlit && *lit != x) fail(k, "ka: two different literals in one instruction");
    *lit = x; *nlit = 1;
    return 255u;
}

int ka_label(ka_t *k)
{
    if (k->nlab >= KA_MAX_LABELS) { fail(k, "ka: too many labels"); return 0; }
    k->lab[k->nlab] = 0xFFFFFFFFu;
    return (int)k->nlab++;
}

void ka_bind(ka_t *k, int l) { k->lab[l] = k->n; }

void ka_branch(ka_t *k, GcnSoppOp op, int l)
{
    if (k->nfix >= KA_MAX_FIX) { fail(k, "ka: too many branches"); return; }
    k->fix_at[k->nfix] = k->n; k->fix_lab[k->nfix] = (uint16_t)l; k->nfix++;
    EMIT(gcn_emit_sopp(B, op, 0));
}

int ka_finish(ka_t *k)
{
    for (uint32_t i = 0; i < k->nfix; i++) {
        uint32_t t = k->lab[k->fix_lab[i]];
        if (t == 0xFFFFFFFFu) { fail(k, "ka: unbound label"); break; }
        int32_t d = (int32_t)t - (int32_t)(k->fix_at[i] + 1);
        if (d < -32768 || d > 32767) { fail(k, "ka: branch out of range"); break; }
        k->w[k->fix_at[i]] = (k->w[k->fix_at[i]] & 0xFFFF0000u) | ((uint32_t)d & 0xFFFFu);
    }
    return k->err ? -1 : 0;
}

/* ---- scalar ---- */
void ka_sop2(ka_t *k, GcnSop2Op op, uint32_t d, ko a, ko b)
{
    uint32_t lit = 0; int nl = 0;
    uint32_t ca = enc(k, a, &lit, &nl), cb = enc(k, b, &lit, &nl);
    note(k, d, 2);
    EMIT(gcn_emit_sop2(B, op, (uint8_t)d, (uint8_t)cb, (uint8_t)ca));
    if (nl) put(k, lit);
}
void ka_sop1(ka_t *k, GcnSop1Op op, uint32_t d, ko a)
{
    uint32_t lit = 0; int nl = 0;
    uint32_t ca = enc(k, a, &lit, &nl);
    note(k, d, 1);
    EMIT(gcn_emit_sop1(B, op, (uint8_t)d, (uint8_t)ca));
    if (nl) put(k, lit);
}
void ka_smov(ka_t *k, uint32_t d, ko a) { ka_sop1(k, GCN_SOP1_S_MOV_B32, d, a); }
void ka_sopk(ka_t *k, GcnSopkOp op, uint32_t d, uint16_t imm)
{
    note(k, d, 1);
    EMIT(gcn_emit_sopk(B, op, (uint8_t)d, imm));
}
void ka_sopc(ka_t *k, GcnSopcOp op, ko a, ko b)
{
    uint32_t lit = 0; int nl = 0;
    uint32_t ca = enc(k, a, &lit, &nl), cb = enc(k, b, &lit, &nl);
    EMIT(gcn_emit_sopc(B, op, (uint8_t)cb, (uint8_t)ca));
    if (nl) put(k, lit);
}
void ka_sopp(ka_t *k, GcnSoppOp op, uint16_t imm) { EMIT(gcn_emit_sopp(B, op, imm)); }
void ka_end(ka_t *k) { ka_sopp(k, GCN_SOPP_S_ENDPGM, 0); }

void ka_wait(ka_t *k, int vm, int exp, int lgkm)
{
    uint32_t v = (vm < 0 ? 15u : (uint32_t)vm) | ((exp < 0 ? 7u : (uint32_t)exp) << 4) |
                 ((lgkm < 0 ? 15u : (uint32_t)lgkm) << 8);
    ka_sopp(k, GCN_SOPP_S_WAITCNT, (uint16_t)v);
}

static uint32_t smem_n(GcnSmemOp op)
{
    switch (op & 7) { case 0: return 1; case 1: return 2; case 2: return 4; case 3: return 8; default: return 16; }
}
void ka_smem(ka_t *k, GcnSmemOp op, uint32_t d, uint32_t sbase, uint32_t imm)
{
    if (sbase & 1u) fail(k, "ka: SMEM base must be an even SGPR");
    note(k, d, smem_n(op));
    EMIT(gcn_emit_smem(B, op, (uint8_t)d, (uint8_t)(sbase / 2), imm, 0));
}
void ka_smem_s(ka_t *k, GcnSmemOp op, uint32_t d, uint32_t sbase, uint32_t soff)
{
    if (sbase & 1u) fail(k, "ka: SMEM base must be an even SGPR");
    note(k, d, smem_n(op));
    EMIT(gcn_emit_smem_soff(B, op, (uint8_t)d, (uint8_t)(sbase / 2), (uint8_t)soff, 0));
}

/* ---- vector ---- */
static void emit_vop3_raw(ka_t *k, uint32_t op3, uint32_t vd, ko a, ko b, ko c, int neg, int abs, int b_form, uint32_t sdst)
{
    uint32_t lit = 0; int nl = 0;
    uint32_t ca = enc(k, a, &lit, &nl), cb = enc(k, b, &lit, &nl), cc = enc(k, c, &lit, &nl);
    if (nl) fail(k, "ka: VOP3 cannot take a literal on gfx8 (load it into an SGPR)");
    if (b_form) { note(k, sdst, 2); EMIT(gcn_emit_vop3b(B, (uint16_t)op3, (uint8_t)vd, (uint8_t)sdst, ca, cb, cc, (uint8_t)neg, 0, 0)); }
    else EMIT(gcn_emit_vop3(B, (uint16_t)op3, (uint8_t)vd, 1, ca, cb, cc, (uint8_t)neg, (uint8_t)abs, 0, 0));
}

void ka_vop3(ka_t *k, uint32_t op3, uint32_t vd, ko a, ko b, ko c, int neg, int abs)
{
    note(k, 256 + vd, 1);
    emit_vop3_raw(k, op3, vd, a, b, c, neg, abs, 0, 0);
}

void ka_fma(ka_t *k, uint32_t vd, ko a, ko b, ko c, int neg)
{
    ka_vop3(k, K3_FMA_F32, vd, a, b, c, neg, 0);
}

void ka_vop1(ka_t *k, GcnVop1Op op, uint32_t vd, ko a)
{
    uint32_t lit = 0; int nl = 0;
    uint32_t ca = enc(k, a, &lit, &nl);
    note(k, 256 + vd, 1);
    EMIT(gcn_emit_vop1(B, op, (uint8_t)vd, ca));
    if (nl) put(k, lit);
}

static int commutes(GcnVop2Op op)
{
    switch ((int)op) {
    case 1: case 5: case 8: case 10: case 11: case 12: case 13: case 14: case 15:
    case 19: case 20: case 21: case 25: return 1;
    default: return 0;
    }
}

void ka_vop2(ka_t *k, GcnVop2Op op, uint32_t vd, ko a, ko b)
{
    note(k, 256 + vd, 1);
    if (!is_v(b) && is_v(a)) {
        if (commutes(op)) { ko t = a; a = b; b = t; }
        else if (op == GCN_VOP2_V_SUB_F32) { ko t = a; a = b; b = t; op = GCN_VOP2_V_SUBREV_F32; }
        else if (op == GCN_VOP2_V_SUBREV_F32) { ko t = a; a = b; b = t; op = GCN_VOP2_V_SUB_F32; }
        else if (op == GCN_VOP2_V_SUB_U32) { ko t = a; a = b; b = t; op = GCN_VOP2_V_SUBREV_U32; }
        else if (op == GCN_VOP2_V_SUBREV_U32) { ko t = a; a = b; b = t; op = GCN_VOP2_V_SUB_U32; }
    }
    if (is_v(b)) {
        uint32_t lit = 0; int nl = 0;
        uint32_t ca = enc(k, a, &lit, &nl);
        note(k, (uint32_t)b, 1);
        EMIT(gcn_emit_vop2(B, op, (uint8_t)vd, (uint8_t)((uint32_t)b - 256u), ca));
        if (nl) put(k, lit);
        return;
    }
    if (op >= 25 && op <= 30) emit_vop3_raw(k, 0x100u + op, vd, a, b, KS(0), 0, 0, 1, 106);
    else emit_vop3_raw(k, 0x100u + op, vd, a, b, KS(0), 0, 0, 0, 0);
}

void ka_vcmp(ka_t *k, GcnVopcOp op, uint32_t sd, ko a, ko b)
{
    if (sd == 106 && is_v(b)) {
        uint32_t lit = 0; int nl = 0;
        uint32_t ca = enc(k, a, &lit, &nl);
        note(k, (uint32_t)b, 1);
        EMIT(gcn_emit_vopc(B, op, (uint8_t)((uint32_t)b - 256u), ca));
        if (nl) put(k, lit);
        return;
    }
    note(k, sd, 2);
    emit_vop3_raw(k, (uint32_t)op, sd, a, b, KS(0), 0, 0, 0, 0);
}

void ka_cnd(ka_t *k, uint32_t vd, ko a, ko b, uint32_t mask)
{
    note(k, 256 + vd, 1);
    if (mask == 106 && is_v(b)) {
        uint32_t lit = 0; int nl = 0;
        uint32_t ca = enc(k, a, &lit, &nl);
        EMIT(gcn_emit_vop2(B, GCN_VOP2_V_CNDMASK_B32, (uint8_t)vd, (uint8_t)((uint32_t)b - 256u), ca));
        if (nl) put(k, lit);
        return;
    }
    note(k, mask, 2);
    emit_vop3_raw(k, 0x100u, vd, a, b, KS(mask), 0, 0, 0, 0);
}

/* ---- memory ---- */
void ka_buf(ka_t *k, GcnMubufOp op, uint32_t vdata, uint32_t vaddr, uint32_t srsrc,
            ko soffset, uint32_t imm, int offen, int glc)
{
    uint32_t lit = 0; int nl = 0;
    uint32_t cs = enc(k, soffset, &lit, &nl);
    if (nl) fail(k, "ka: MUBUF soffset cannot be a literal");
    if (srsrc & 3u) fail(k, "ka: MUBUF descriptor must start at a multiple of 4");
    if (imm > 4095) fail(k, "ka: MUBUF offset > 4095");
    uint32_t n = 1;
    if (op == GCN_MUBUF_BUFFER_LOAD_DWORDX2 || op == GCN_MUBUF_BUFFER_STORE_DWORDX2) n = 2;
    if (op == GCN_MUBUF_BUFFER_LOAD_DWORDX4 || op == GCN_MUBUF_BUFFER_STORE_DWORDX4) n = 4;
    note(k, 256 + vdata, n); if (offen) note(k, 256 + vaddr, 1);
    EMIT(gcn_emit_mubuf(B, op, (uint8_t)vdata, (uint8_t)vaddr, (uint8_t)(srsrc / 4), (uint8_t)cs,
                        (uint16_t)imm, offen, 0, glc, 0));
}

void ka_ds(ka_t *k, GcnDsOp op, uint32_t vdst, uint32_t addr, uint32_t d0, uint32_t d1,
           uint8_t off0, uint8_t off1)
{
    note(k, 256 + addr, 1);
    note(k, 256 + vdst, (op == GCN_DS_DS_READ2_B32 || op == GCN_DS_DS_READ_B64) ? 2 : 1);
    note(k, 256 + d0, op == GCN_DS_DS_WRITE_B64 ? 2 : 1);
    EMIT(gcn_emit_ds(B, op, (uint8_t)vdst, (uint8_t)addr, (uint8_t)d0, (uint8_t)d1, off0, off1, 0));
}
