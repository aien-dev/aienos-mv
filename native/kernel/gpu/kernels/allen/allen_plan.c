/* native/kernel/gpu/kernels/allen/allen_plan.c -- see allen_plan.h. No floats. */
#include "allen_plan.h"
#include "allen_kern.h"
#include <string.h>

static uint32_t rd(const uint8_t *f, uint64_t at)
{
    return (uint32_t)f[at] | ((uint32_t)f[at + 1] << 8) | ((uint32_t)f[at + 2] << 16) | ((uint32_t)f[at + 3] << 24);
}

static uint32_t alloc(ap_model *m, uint64_t bytes)
{
    uint64_t off = (m->size + 255u) & ~(uint64_t)255u;
    m->size = off + bytes;
    return (uint32_t)off;
}

typedef struct { const uint8_t *f; size_t n; uint64_t at; const char *err; } rdr;

static uint32_t tensor(ap_model *m, rdr *r, int bf16, uint64_t count)
{
    if (r->err) return 0;
    if (r->at + 8 > r->n) { r->err = "AGP1: truncated"; return 0; }
    uint32_t dt = rd(r->f, r->at), cnt = rd(r->f, r->at + 4);
    if (dt != (bf16 ? 0u : 1u) || cnt != count) { r->err = "AGP1: tensor dtype or size differs from the header"; return 0; }
    uint64_t bytes = count * (bf16 ? 2u : 4u);
    if (r->at + 8 + bytes > r->n) { r->err = "AGP1: truncated tensor"; return 0; }
    if (m->ntensor >= sizeof m->dst / sizeof m->dst[0]) { r->err = "AGP1: too many tensors"; return 0; }
    uint32_t off = alloc(m, bytes);
    m->src[m->ntensor] = r->at + 8; m->dst[m->ntensor] = off; m->bytes[m->ntensor] = (uint32_t)bytes;
    m->ntensor++;
    r->at += 8 + bytes;
    return off;
}

int ap_parse(const uint8_t *file, size_t n, uint32_t max_pos, ap_model *m, const char **err)
{
    memset(m, 0, sizeof *m);
    *err = 0;
    if (n < 64 || rd(file, 0) != 0x31504741u || rd(file, 4) != 1u) { *err = "AGP1: bad magic or version"; return -1; }
    uint32_t *h = &m->hidden;
    for (int i = 0; i < 14; i++) h[i] = rd(file, 8 + 4 * (uint64_t)i);
    if (m->layers > AP_MAX_LAYERS || m->kern != 4 || m->vd != 128 || m->hd != 256 || m->rot / 2 > 64 ||
        m->hidden % 512 || m->inter % 512 || (m->vh * m->vd) % 512 || (m->heads * m->hd) % 512 ||
        max_pos > AK_MAX_SPAN || max_pos == 0 || m->kd % 8 || m->vocab % 64 || m->hidden & (m->hidden - 1)) {
        *err = "AGP1: shape outside what the kernels support"; return -1;
    }
    m->max_pos = max_pos;
    for (uint32_t l = 0; l < m->layers; l++) m->attn[l] = rd(file, 64 + 4 * (uint64_t)l);
    rdr r = { file, n, 64 + 4 * (uint64_t)m->layers, 0 };
    uint64_t H = m->hidden, cd = 2 * (uint64_t)m->kh * m->kd + (uint64_t)m->vh * m->vd, vdim = (uint64_t)m->vh * m->vd;
    uint64_t qd = (uint64_t)m->heads * m->hd, kvd = (uint64_t)m->kvh * m->hd;
    m->embed = tensor(m, &r, 1, (uint64_t)m->vocab * H);
    m->final_norm = tensor(m, &r, 0, H);
    for (uint32_t i = 0; i < m->layers; i++) {
        ap_layer *L = &m->l[i];
        L->in_norm = tensor(m, &r, 0, H);
        L->post_norm = tensor(m, &r, 0, H);
        if (!m->attn[i]) {
            L->qkv = tensor(m, &r, 1, cd * H);
            L->z = tensor(m, &r, 1, vdim * H);
            L->a = tensor(m, &r, 1, m->vh * H);
            L->b = tensor(m, &r, 1, m->vh * H);
            L->conv = tensor(m, &r, 0, cd * m->kern);
            L->a_log = tensor(m, &r, 0, m->vh);
            L->dt = tensor(m, &r, 0, m->vh);
            L->norm = tensor(m, &r, 0, m->vd);
            L->out = tensor(m, &r, 1, H * vdim);
        } else {
            L->q = tensor(m, &r, 1, 2 * qd * H);
            L->k = tensor(m, &r, 1, kvd * H);
            L->v = tensor(m, &r, 1, kvd * H);
            L->o = tensor(m, &r, 1, H * qd);
            L->qn = tensor(m, &r, 0, m->hd);
            L->kn = tensor(m, &r, 0, m->hd);
        }
        L->gate = tensor(m, &r, 1, (uint64_t)m->inter * H);
        L->up = tensor(m, &r, 1, (uint64_t)m->inter * H);
        L->down = tensor(m, &r, 1, H * m->inter);
    }
    if (r.err) { *err = r.err; return -1; }
    if (r.at != n) { *err = "AGP1: trailing bytes"; return -1; }
    m->x = alloc(m, H * 4); m->xn = alloc(m, H * 4);
    m->qkv = alloc(m, cd * 4); m->z = alloc(m, vdim * 4); m->a = alloc(m, 256); m->b = alloc(m, 256);
    m->mixed = alloc(m, cd * 4); m->core = alloc(m, vdim * 4); m->y = alloc(m, vdim * 4);
    m->qg = alloc(m, 2 * qd * 4); m->kr = alloc(m, kvd * 4); m->vt = alloc(m, kvd * 4);
    m->q = alloc(m, qd * 4); m->ao = alloc(m, qd * 4);
    m->g = alloc(m, (uint64_t)m->inter * 4); m->u = alloc(m, (uint64_t)m->inter * 4); m->act = alloc(m, (uint64_t)m->inter * 4);
    m->logits = alloc(m, (uint64_t)m->vocab * 4); m->tok = alloc(m, 256);
    m->cos = alloc(m, (uint64_t)max_pos * (m->rot / 2) * 4); m->sin = alloc(m, (uint64_t)max_pos * (m->rot / 2) * 4);
    m->state_lo = alloc(m, 0);
    for (uint32_t i = 0; i < m->layers; i++) {
        ap_layer *L = &m->l[i];
        if (!m->attn[i]) {
            L->tail = alloc(m, (m->kern - 1) * cd * 4);
            L->state = alloc(m, (uint64_t)m->vh * m->kd * m->vd * 4);
        } else {
            L->kc = alloc(m, (uint64_t)max_pos * kvd * 4);
            L->vc = alloc(m, (uint64_t)max_pos * kvd * 4);
        }
    }
    m->state_hi = (uint32_t)m->size;
    if (m->size >= 0xFFFFFFFFull) { *err = "AGP1: image over 4 GB"; return -1; }
    return 0;
}

void ap_fill(const ap_model *m, const uint8_t *file, uint8_t *image)
{
    for (uint32_t i = 0; i < m->ntensor; i++) memcpy(image + m->dst[i], file + m->src[i], m->bytes[i]);
}

typedef struct { ap_disp *d; int n; } list;

static void add(list *s, int id, uint32_t c, uint32_t groups, const uint32_t *p, int np)
{
    if (s->n >= AP_MAX_DISP) return;
    ap_disp *d = &s->d[s->n++];
    memset(d, 0, sizeof *d);
    d->id = id; d->c = c; d->groups = groups;
    for (int i = 0; i < np; i++) d->p[i] = p[i];
}

static void mv(list *s, int acc, uint32_t w, uint32_t x, uint32_t y, uint32_t k, uint32_t n, uint32_t R)
{
    uint32_t p[6] = { w, x, y, k, R, n };
    add(s, acc ? AK_MATVEC_ACC : AK_MATVEC, k / 512, (n + R - 1) / R, p, 6);
}

static void nrm(list *s, int id, uint32_t rows, uint32_t x, uint32_t xs, uint32_t y, uint32_t ys, uint32_t n,
                uint32_t w, uint32_t z, uint32_t zs, uint32_t eps, uint32_t scale_n)
{
    uint32_t p[10] = { x, xs, y, ys, n, w, z, zs, eps, scale_n };
    add(s, id, 0, rows, p, 10);
}

int ap_token(const ap_model *m, uint32_t token, uint32_t pos, ap_disp *d)
{
    list s = { d, 0 };
    uint32_t H = m->hidden, cd = 2 * m->kh * m->kd + m->vh * m->vd, vdim = m->vh * m->vd;
    uint32_t qd = m->heads * m->hd, kvd = m->kvh * m->hd, half = m->rot / 2;
    uint32_t pe[4] = { m->embed, token, m->x, H };
    add(&s, AK_EMBED, 0, 1, pe, 4);
    for (uint32_t i = 0; i < m->layers; i++) {
        const ap_layer *L = &m->l[i];
        nrm(&s, AK_NORM_ZC, 1, m->x, 0, m->xn, 0, H, L->in_norm, 0, 0, m->eps, 0);
        if (!m->attn[i]) {
            mv(&s, 0, L->qkv, m->xn, m->qkv, H, cd, 16);
            mv(&s, 0, L->z, m->xn, m->z, H, vdim, 16);
            mv(&s, 0, L->a, m->xn, m->a, H, m->vh, 16);
            mv(&s, 0, L->b, m->xn, m->b, H, m->vh, 16);
            uint32_t pc[5] = { m->qkv, L->tail, L->conv, m->mixed, cd };
            add(&s, AK_CONV, 0, cd / 64, pc, 5);
            nrm(&s, AK_NORM_L2, m->kh, m->mixed, m->kd * 4, m->mixed, m->kd * 4, m->kd, 0, 0, 0, 0, m->kd);
            nrm(&s, AK_NORM_L2, m->kh, m->mixed + m->kh * m->kd * 4, m->kd * 4, m->mixed + m->kh * m->kd * 4,
                m->kd * 4, m->kd, 0, 0, 0, 0, 0);
            uint32_t sh = 0; while ((m->kh << sh) < m->vh) sh++;
            uint32_t pg[10] = { m->mixed, m->a, m->b, L->a_log, L->dt, L->state, m->core, m->kd, m->kh, sh };
            add(&s, AK_GDN, 0, m->vh, pg, 10);
            nrm(&s, AK_NORM_GATED, m->vh, m->core, m->vd * 4, m->y, m->vd * 4, m->vd, L->norm, m->z, m->vd * 4, m->eps, 0);
            mv(&s, 1, L->out, m->y, m->x, vdim, H, 8);
        } else {
            uint32_t krow = L->kc + pos * kvd * 4, vrow = L->vc + pos * kvd * 4;
            mv(&s, 0, L->q, m->xn, m->qg, H, 2 * qd, 16);
            mv(&s, 0, L->k, m->xn, m->kr, H, kvd, 16);
            mv(&s, 0, L->v, m->xn, m->vt, H, kvd, 16);
            nrm(&s, AK_NORM_ZC, m->heads, m->qg, 2 * m->hd * 4, m->q, m->hd * 4, m->hd, L->qn, 0, 0, m->eps, 0);
            nrm(&s, AK_NORM_ZC, m->kvh, m->kr, m->hd * 4, krow, m->hd * 4, m->hd, L->kn, 0, 0, m->eps, 0);
            uint32_t pr[6] = { m->q, m->hd * 4, m->cos, m->sin, pos, half };
            add(&s, AK_ROPE, 0, m->heads, pr, 6);
            pr[0] = krow;
            add(&s, AK_ROPE, 0, m->kvh, pr, 6);
            uint32_t pcp[2] = { m->vt, vrow };
            add(&s, AK_COPY, 0, kvd / 64, pcp, 2);
            uint32_t sh = 0; while ((m->kvh << sh) < m->heads) sh++;
            uint32_t pa[10] = { m->q, L->kc, L->vc, pos + 1, m->qg + m->hd * 4, 2 * m->hd * 4, m->ao, sh, kvd * 4,
                                0x3D800000u /* 1 / sqrt(256) */ };
            add(&s, AK_ATTN, 0, m->heads, pa, 10);
            mv(&s, 1, L->o, m->ao, m->x, qd, H, 8);
        }
        nrm(&s, AK_NORM_ZC, 1, m->x, 0, m->xn, 0, H, L->post_norm, 0, 0, m->eps, 0);
        mv(&s, 0, L->gate, m->xn, m->g, H, m->inter, 16);
        mv(&s, 0, L->up, m->xn, m->u, H, m->inter, 16);
        uint32_t ps[3] = { m->g, m->u, m->act };
        add(&s, AK_SWIGLU, 0, m->inter / 64, ps, 3);
        mv(&s, 1, L->down, m->act, m->x, m->inter, H, 8);
    }
    nrm(&s, AK_NORM_ZC, 1, m->x, 0, m->xn, 0, H, m->final_norm, 0, 0, m->eps, 0);
    mv(&s, 0, m->embed, m->xn, m->logits, H, m->vocab, 64);
    uint32_t px[3] = { m->logits, m->vocab, m->tok };
    add(&s, AK_ARGMAX, 0, 1, px, 3);
    return s.n;
}
