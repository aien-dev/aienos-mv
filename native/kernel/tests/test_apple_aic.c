/* test_apple_aic.c -- host chip models for apple/aic.c (AIC1, AIC2, AICv3)
 * and the FIQ path.
 *
 * The models do NOT reuse the driver's layout arithmetic: every register
 * address a model answers is a literal from a hand-written table below
 * (worked out once from the register facts in aic.h), so a driver with a
 * wrong offset or a wrong die stride writes to an address the model does
 * not know (counted as a stray access) or to the wrong die, and the test
 * goes red. Model behaviour: level-triggered lines (hardware line OR
 * software pending), per-IRQ mask bit, reading the event register returns
 * the lowest pending unmasked IRQ and masks it (auto-mask), AIC2/3 deliver
 * nothing until CONFIG bit 0 is set, AIC1 delivers only IRQs targeted at
 * CPU 0. Base addresses and reg sizes come from the pinned trees
 * (t8103 j313, t8112 j413, t6002-like two dies, t8122 j613). INFO register
 * values are model choices that fit those reg sizes (the real values are
 * UNKNOWN until read on hardware: docs/APPLE_SILICON_PLATFORM.md U1). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../apple/aic.h"
#include "ck_test.h"

#define MAXW 64 /* bitmap words per die (2048 IRQs) */
#define MAXD 2

struct layout { /* literal register offsets per die, from the core base */
    uint32_t sw_set[MAXD], sw_clr[MAXD], mask_set[MAXD], mask_clr[MAXD], hw_state[MAXD];
    uint32_t cfg[MAXD]; /* TARGET_CPU (AIC1) or IRQ_CFG (AIC2/3) */
};

struct model {
    uint32_t version, nr_irq, max_irq, ndie, info_last_die, info_max_die;
    uint64_t base, size, event;
    struct layout L;
    uint32_t config;
    uint32_t masked[MAXD][MAXW], sw[MAXD][MAXW], hw[MAXD][MAXW];
    uint32_t target[2048];
    uint32_t stray, writes, ipi_ack, ipi_mask;
    /* system registers */
    uint64_t sr[CK_SR_COUNT];
    uint32_t sr_reads[CK_SR_COUNT], sr_writes[CK_SR_COUNT];
    uint64_t last_rr_local, last_rr_global;
};

static int in_bitmap(uint32_t off, uint32_t start, uint32_t words, uint32_t *w)
{
    if (off < start || off >= start + 4u * words || (off - start) % 4u) return 0;
    *w = (off - start) / 4u;
    return 1;
}

static uint32_t m_rd32(void *ctx, uint64_t addr)
{
    struct model *m = ctx;
    if (addr == m->event) {
        if (m->version >= 2 && !(m->config & 1u)) return 0;
        for (uint32_t d = 0; d < m->ndie; d++)
            for (uint32_t i = 0; i < m->nr_irq; i++) {
                uint32_t w = i / 32u, b = 1u << (i % 32u);
                if (((m->hw[d][w] | m->sw[d][w]) & b) && !(m->masked[d][w] & b)) {
                    if (m->version == 1 && !(m->target[i] & 1u)) continue;
                    m->masked[d][w] |= b; /* auto-mask */
                    return d << 24 | 1u << 16 | i;
                }
            }
        return 0;
    }
    if (addr < m->base || addr >= m->base + m->size) {
        m->stray++;
        return 0;
    }
    uint32_t off = (uint32_t)(addr - m->base);
    if (m->version == 1 && off == 0x0004u) return m->nr_irq;
    if (m->version >= 2 && off == 0x0004u) return m->info_last_die << 24 | m->nr_irq;
    if (m->version >= 2 && off == 0x000cu) return m->info_max_die << 24 | m->max_irq;
    if (m->version >= 2 && off == 0x0014u) return m->config;
    uint32_t words = m->max_irq / 32u, w;
    for (uint32_t d = 0; d < m->ndie; d++)
        if (in_bitmap(off, m->L.mask_set[d], words, &w) || in_bitmap(off, m->L.mask_clr[d], words, &w))
            return m->masked[d][w];
    m->stray++;
    return 0;
}

static void m_wr32(void *ctx, uint64_t addr, uint32_t v)
{
    struct model *m = ctx;
    m->writes++;
    if (addr < m->base || addr >= m->base + m->size) {
        m->stray++;
        return;
    }
    uint32_t off = (uint32_t)(addr - m->base), words = m->max_irq / 32u, w;
    if (m->version >= 2 && off == 0x0014u) {
        m->config = v;
        return;
    }
    if (m->version == 1 && off == 0x200cu) {
        m->ipi_ack |= v;
        return;
    }
    if (m->version == 1 && off == 0x2024u) {
        m->ipi_mask |= v;
        return;
    }
    for (uint32_t d = 0; d < m->ndie; d++) {
        if (in_bitmap(off, m->L.mask_set[d], words, &w)) { m->masked[d][w] |= v; return; }
        if (in_bitmap(off, m->L.mask_clr[d], words, &w)) { m->masked[d][w] &= ~v; return; }
        if (in_bitmap(off, m->L.sw_set[d], words, &w)) { m->sw[d][w] |= v; return; }
        if (in_bitmap(off, m->L.sw_clr[d], words, &w)) { m->sw[d][w] &= ~v; return; }
        if (in_bitmap(off, m->L.cfg[d], m->max_irq, &w)) {
            if (m->version == 1) m->target[w] = v;
            return;
        }
    }
    m->stray++;
}

static uint64_t m_rdsr(void *ctx, enum ck_aic_sr r)
{
    struct model *m = ctx;
    m->sr_reads[r]++;
    return m->sr[r];
}

static void m_wrsr(void *ctx, enum ck_aic_sr r, uint64_t v)
{
    struct model *m = ctx;
    m->sr_writes[r]++;
    if (r == CK_SR_IPI_SR) m->sr[r] &= ~v; /* write 1 to clear */
    else if (r == CK_SR_IPI_RR_LOCAL) m->last_rr_local = v;
    else if (r == CK_SR_IPI_RR_GLOBAL) m->last_rr_global = v;
    else m->sr[r] = v;
}

static struct ck_aic_io io_for(struct model *m)
{
    struct ck_aic_io io = {m_rd32, m_wr32, m_rdsr, m_wrsr, m};
    return io;
}

/* ---- model configurations (literals) ---- */

/* AIC1, t8103 j313: core 0x2_3b10_0000 size 0x8000; 0x400 IRQ slots.
 * TARGET_CPU 0x3000..0x3fff, SW_SET 0x4000, SW_CLR 0x4080, MASK_SET 0x4100,
 * MASK_CLR 0x4180, HW_STATE 0x4200 (each 0x80 bytes). Event 0x2004. */
static void model_aic1(struct model *m)
{
    memset(m, 0, sizeof(*m));
    m->version = 1;
    m->nr_irq = 896;
    m->max_irq = 0x400;
    m->ndie = 1;
    m->base = 0x23b100000ull;
    m->size = 0x8000;
    m->event = m->base + 0x2004u;
    m->L.cfg[0] = 0x3000;
    m->L.sw_set[0] = 0x4000;
    m->L.sw_clr[0] = 0x4080;
    m->L.mask_set[0] = 0x4100;
    m->L.mask_clr[0] = 0x4180;
    m->L.hw_state[0] = 0x4200;
}

/* AIC2, t8112 j413: core 0x2_3b0c_0000 size 0x8000, event 0x2_3b0c_8000.
 * max_irq 0x400: IRQ_CFG 0x2000..0x2fff, SW_SET 0x3000, SW_CLR 0x3080,
 * MASK_SET 0x3100, MASK_CLR 0x3180, HW_STATE 0x3200, die stride 0x1280.
 * ndie 2 (t6002-like): die 1 at +0x1280 (MASK_CLR 0x4400). */
static void model_aic2(struct model *m, uint32_t ndie)
{
    memset(m, 0, sizeof(*m));
    m->version = 2;
    m->nr_irq = 0x3c0;
    m->max_irq = 0x400;
    m->ndie = ndie;
    m->info_last_die = ndie - 1u;
    m->info_max_die = 2;
    m->base = 0x23b0c0000ull;
    m->size = 0x8000;
    m->event = 0x23b0c8000ull;
    static const uint32_t cfg[2] = {0x2000, 0x3280}, sws[2] = {0x3000, 0x4280}, swc[2] = {0x3080, 0x4300},
                          ms[2] = {0x3100, 0x4380}, mc[2] = {0x3180, 0x4400}, hs[2] = {0x3200, 0x4480};
    for (uint32_t d = 0; d < 2; d++) {
        m->L.cfg[d] = cfg[d];
        m->L.sw_set[d] = sws[d];
        m->L.sw_clr[d] = swc[d];
        m->L.mask_set[d] = ms[d];
        m->L.mask_clr[d] = mc[d];
        m->L.hw_state[d] = hs[d];
    }
}

/* AICv3, t8122 j613: core 0x2_d100_0000 size 0x184000, event 0x2_d104_0000.
 * max_irq 0x800: IRQ_CFG 0x10000..0x11fff, SW_SET 0x12000, SW_CLR 0x12100,
 * MASK_SET 0x12200, MASK_CLR 0x12300, HW_STATE 0x12400, die stride 0x2500. */
static void model_aic3(struct model *m, uint32_t ndie)
{
    memset(m, 0, sizeof(*m));
    m->version = 3;
    m->nr_irq = 0x700;
    m->max_irq = 0x800;
    m->ndie = ndie;
    m->info_last_die = ndie - 1u;
    m->info_max_die = 2;
    m->base = 0x2d1000000ull;
    m->size = 0x184000;
    m->event = 0x2d1040000ull;
    static const uint32_t cfg[2] = {0x10000, 0x12500}, sws[2] = {0x12000, 0x14500}, swc[2] = {0x12100, 0x14600},
                          ms[2] = {0x12200, 0x14700}, mc[2] = {0x12300, 0x14800}, hs[2] = {0x12400, 0x14900};
    for (uint32_t d = 0; d < 2; d++) {
        m->L.cfg[d] = cfg[d];
        m->L.sw_set[d] = sws[d];
        m->L.sw_clr[d] = swc[d];
        m->L.mask_set[d] = ms[d];
        m->L.mask_clr[d] = mc[d];
        m->L.hw_state[d] = hs[d];
    }
}

static void raise_line(struct model *m, uint32_t d, uint32_t i) { m->hw[d][i / 32u] |= 1u << (i % 32u); }
static void lower_line(struct model *m, uint32_t d, uint32_t i) { m->hw[d][i / 32u] &= ~(1u << (i % 32u)); }
static int is_masked(struct model *m, uint32_t d, uint32_t i) { return (m->masked[d][i / 32u] >> (i % 32u)) & 1u; }

struct seen {
    struct model *m;
    uint32_t n, die[16], irq[16];
    int keep_masked;
};

static int on_irq(void *ctx, uint32_t die, uint32_t irq)
{
    struct seen *s = ctx;
    if (s->n < 16) {
        s->die[s->n] = die;
        s->irq[s->n] = irq;
    }
    s->n++;
    lower_line(s->m, die, irq); /* the device handler quiets its line */
    return !s->keep_masked;
}

static int all_masked(struct model *m)
{
    for (uint32_t d = 0; d < m->ndie; d++)
        for (uint32_t i = 0; i < m->nr_irq; i++)
            if (!is_masked(m, d, i)) return 0;
    return 1;
}

/* One full round on a model: init, all masked, deliver on the right die. */
static void round_trip(struct model *m, uint64_t event_size, const char *tag)
{
    struct ck_aic a;
    struct ck_aic_io io = io_for(m);
    int rc = ck_aic_init(&a, &io, m->version, m->base, m->size, m->event, event_size);
    if (rc) printf("  %s: init rc=%d\n", tag, rc);
    CHECK(rc == 0);
    CHECK(m->stray == 0);
    CHECK(all_masked(m));
    CHECK(a.nr_die == m->ndie && a.nr_irq == m->nr_irq);
    if (m->version >= 2) CHECK(m->config & 1u);
    if (m->version == 1) {
        CHECK(m->target[0] == 1 && m->target[m->nr_irq - 1] == 1);
        CHECK(m->ipi_ack == (1u << 31 | 1u) && m->ipi_mask == (1u << 31 | 1u));
    }
    struct seen s = {.m = m};
    /* Masked line: no event. */
    raise_line(m, 0, 37);
    CHECK(ck_aic_handle_irq(&a, on_irq, &s, 8) == 0 && s.n == 0);
    /* Unmasked: delivered once, unmasked again after the handler. */
    CHECK(ck_aic_unmask(&a, 0, 37) == 0);
    CHECK(ck_aic_handle_irq(&a, on_irq, &s, 8) == 1 && s.n == 1 && s.die[0] == 0 && s.irq[0] == 37);
    CHECK(!is_masked(m, 0, 37));
    CHECK(ck_aic_handle_irq(&a, on_irq, &s, 8) == 0);
    /* Last die, last IRQ, software-triggered. */
    uint32_t ld = m->ndie - 1u, li = m->nr_irq - 1u;
    CHECK(ck_aic_unmask(&a, ld, li) == 0);
    CHECK(ck_aic_sw_set(&a, ld, li) == 0);
    s.n = 0;
    s.keep_masked = 1; /* handler asks to stay masked */
    CHECK(ck_aic_handle_irq(&a, on_irq, &s, 8) == 1 && s.die[0] == ld && s.irq[0] == li);
    CHECK(is_masked(m, ld, li));
    CHECK(ck_aic_sw_clr(&a, ld, li) == 0);
    CHECK(m->sw[ld][li / 32u] == 0);
    /* Out of range is refused, nothing written. */
    uint32_t w = m->writes;
    CHECK(ck_aic_unmask(&a, m->ndie, 0) == CK_AIC_E_IRQ);
    CHECK(ck_aic_unmask(&a, 0, m->nr_irq) == CK_AIC_E_IRQ);
    CHECK(m->writes == w);
    /* A line that never quiets: bounded by the budget, never a hang. */
    s.n = 0;
    s.keep_masked = 0;
    CHECK(ck_aic_unmask(&a, 0, 5) == 0);
    raise_line(m, 0, 5);
    uint32_t n = ck_aic_handle_irq(&a, 0, 0, 16); /* no handler: line stays high, unmasked each time */
    CHECK(n == 16);
    lower_line(m, 0, 5);
    if (m->version == 1) {
        CHECK(ck_aic_target(&a, 7, 2) == 0 && m->target[7] == 2);
        raise_line(m, 0, 7);
        ck_aic_unmask(&a, 0, 7);
        CHECK(ck_aic_handle_irq(&a, on_irq, &s, 4) == 0); /* targeted away from CPU 0 */
    } else {
        CHECK(ck_aic_target(&a, 7, 2) == CK_AIC_E_UNSUP);
    }
    CHECK(m->stray == 0);
}

static void test_layouts(void)
{
    struct model *m = calloc(1, sizeof(*m));
    model_aic1(m);
    round_trip(m, 0, "aic1");
    model_aic2(m, 1);
    round_trip(m, 4, "aic2");
    model_aic2(m, 2);
    round_trip(m, 4, "aic2x2");
    CHECK(m->masked[1][0] == 0xffffffffu); /* die 1 masked at its own address */
    model_aic3(m, 1);
    round_trip(m, 4, "aic3");
    model_aic3(m, 2);
    round_trip(m, 4, "aic3x2");
    free(m);
}

static void test_refusals(void)
{
    struct model *m = calloc(1, sizeof(*m));
    struct ck_aic a;
    struct ck_aic_io io;

    model_aic2(m, 1);
    io = io_for(m);
    CHECK(ck_aic_init(&a, &io, 4, m->base, m->size, m->event, 4) == CK_AIC_E_VERSION);
    CHECK(ck_aic_init(&a, &io, 2, m->base, m->size, m->event, 0) == CK_AIC_E_EVENT);
    CHECK(ck_aic_init(&a, &io, 2, m->base, 0x3000, m->event, 4) == CK_AIC_E_SIZE); /* block past reg size */
    CHECK(ck_aic_init(&a, &io, 2, m->base, m->size, m->base + 0x3100, 4) == CK_AIC_E_SIZE); /* event in a die block */
    m->nr_irq = 0x401; /* more IRQs than slots */
    CHECK(ck_aic_init(&a, &io, 2, m->base, m->size, m->event, 4) == CK_AIC_E_INFO);
    m->nr_irq = 0;
    CHECK(ck_aic_init(&a, &io, 2, m->base, m->size, m->event, 4) == CK_AIC_E_INFO);
    model_aic2(m, 2);
    CHECK(m->writes == 0);
    m->info_max_die = 1; /* two dies reported, one allowed */
    io = io_for(m);
    CHECK(ck_aic_init(&a, &io, 2, m->base, m->size, m->event, 4) == CK_AIC_E_INFO);
    model_aic2(m, 1);
    CHECK(m->writes == 0);
    m->max_irq = 0x3f0; /* not a multiple of 32 */
    m->nr_irq = 0x10;
    io = io_for(m);
    CHECK(ck_aic_init(&a, &io, 2, m->base, m->size, m->event, 4) == CK_AIC_E_INFO);
    CHECK(m->writes == 0); /* every refusal before any write */
    free(m);
}

/* ---- FIQ path ---- */
struct fiqctx {
    struct model *m;
    uint32_t ipi, tp, tv;
    uint64_t ipi_sr_at_handler;
    int resend, fix_timer;
};

static void h_ipi(void *c)
{
    struct fiqctx *f = c;
    f->ipi++;
    f->ipi_sr_at_handler = f->m->sr[CK_SR_IPI_SR];
    if (f->resend) f->m->sr[CK_SR_IPI_SR] |= 1u; /* another CPU sends again meanwhile */
}

static void h_tp(void *c)
{
    struct fiqctx *f = c;
    f->tp++;
    if (f->fix_timer) f->m->sr[CK_SR_CNTP_CTL] = 1u; /* reprogrammed: enabled, not yet firing */
}

static void h_tv(void *c)
{
    struct fiqctx *f = c;
    f->tv++;
    f->m->sr[CK_SR_CNTV_CTL] = 0; /* disabled */
}

static void test_fiq(void)
{
    struct model *m = calloc(1, sizeof(*m));
    struct ck_aic a = {0};
    struct ck_aic_io io = io_for(m);
    struct fiqctx f = {.m = m, .fix_timer = 1};
    struct ck_fiq_handlers h = {h_ipi, h_tp, h_tv, &f};

    /* Nothing pending. */
    CHECK(ck_aic_handle_fiq(&a, &io, &h) == 0);
    /* Timer firing = ENABLE | ISTATUS, IMASK clear. */
    m->sr[CK_SR_CNTP_CTL] = 1u | 4u;
    CHECK(ck_aic_fiq_pending(&io, 1) == CK_FIQ_TMR_PHYS);
    m->sr[CK_SR_CNTP_CTL] = 1u | 2u | 4u; /* masked: not a FIQ source */
    CHECK(ck_aic_fiq_pending(&io, 1) == 0);
    m->sr[CK_SR_CNTP_CTL] = 4u; /* disabled: not a FIQ source */
    CHECK(ck_aic_fiq_pending(&io, 1) == 0);
    m->sr[CK_SR_CNTP_CTL] = 1u | 4u;
    m->sr[CK_SR_CNTV_CTL] = 1u | 4u;
    CHECK(ck_aic_handle_fiq(&a, &io, &h) == (CK_FIQ_TMR_PHYS | CK_FIQ_TMR_VIRT));
    CHECK(f.tp == 1 && f.tv == 1 && a.storms == 0 && m->sr[CK_SR_CNTP_CTL] == 1u);
    /* Handler that leaves the timer firing: masked, counted, not re-entered. */
    f.fix_timer = 0;
    m->sr[CK_SR_CNTP_CTL] = 1u | 4u;
    ck_aic_handle_fiq(&a, &io, &h);
    CHECK(a.storms == 1 && (m->sr[CK_SR_CNTP_CTL] & 2u));
    CHECK(ck_aic_fiq_pending(&io, 1) == 0);
    /* Fast IPI: acknowledged BEFORE the handler, a resend stays pending. */
    m->sr[CK_SR_IPI_SR] = 1u;
    f.resend = 1;
    CHECK(ck_aic_handle_fiq(&a, &io, &h) == CK_FIQ_IPI);
    CHECK(f.ipi == 1 && f.ipi_sr_at_handler == 0);
    CHECK(m->sr[CK_SR_IPI_SR] == 1u); /* the resend is not lost */
    f.resend = 0;
    CHECK(ck_aic_handle_fiq(&a, &io, &h) == CK_FIQ_IPI && f.ipi == 2 && m->sr[CK_SR_IPI_SR] == 0);
    /* PMU FIQs nobody asked for: turned off. */
    m->sr[CK_SR_PMCR0] = (4u << 8) | (1u << 11) | 0xffu;
    m->sr[CK_SR_UPMCR0] = (4u << 16) | 0x3u;
    m->sr[CK_SR_UPMSR] = 1u;
    CHECK(ck_aic_handle_fiq(&a, &io, &h) == (CK_FIQ_PMC | CK_FIQ_UPMC));
    CHECK(m->sr[CK_SR_PMCR0] == 0xffu && m->sr[CK_SR_UPMCR0] == 0x3u);
    CHECK(ck_aic_fiq_pending(&io, 1) == 0);
    /* Off-Apple: Apple IMP-DEF registers are never read. */
    memset(m->sr_reads, 0, sizeof(m->sr_reads));
    m->sr[CK_SR_IPI_SR] = 1u;
    CHECK(ck_aic_fiq_pending(&io, 0) == 0);
    CHECK(m->sr_reads[CK_SR_IPI_SR] == 0 && m->sr_reads[CK_SR_PMCR0] == 0 && m->sr_reads[CK_SR_UPMCR0] == 0 &&
          m->sr_reads[CK_SR_UPMSR] == 0);
    CHECK(m->sr_reads[CK_SR_CNTP_CTL] == 1 && m->sr_reads[CK_SR_CNTV_CTL] == 1);
    free(m);
}

static void test_ipi_send(void)
{
    struct model *m = calloc(1, sizeof(*m));
    struct ck_aic_io io = io_for(m);
    m->sr[CK_SR_MPIDR] = 0x80010100ull; /* cluster 1 core 0 (P-cluster on M1), bit 31 RES1 */
    ck_aic_ipi_send(&io, 0x10102); /* same cluster, core 2 */
    CHECK(m->sr_writes[CK_SR_IPI_RR_LOCAL] == 1 && m->last_rr_local == 2);
    ck_aic_ipi_send(&io, 0x3); /* cluster 0 core 3 */
    CHECK(m->sr_writes[CK_SR_IPI_RR_GLOBAL] == 1 && m->last_rr_global == 3);
    ck_aic_ipi_send(&io, 0x20205); /* cluster 2 core 5 (Aff2 ignored) */
    CHECK(m->last_rr_global == (5u | 2u << 16));
    free(m);
}

int main(void)
{
    test_layouts();
    test_refusals();
    test_fiq();
    test_ipi_send();
    return ck_t_verdict("test_apple_aic");
}
