/* aic.c -- Apple Interrupt Controller and FIQ path (see aic.h). Pure C over
 * ck_aic_io: the kernel passes real accessors (aic_live.c), the host tests
 * pass chip models (tests/test_apple_aic.c). */
#include "aic.h"

/* TEST-ONLY mutants (apple.mk apple-mutants): host builds only. */
#if (defined(CK_AIC_MUTANT_DIE_STRIDE) || defined(CK_AIC_MUTANT_V3_OFFSET) || defined(CK_AIC_MUTANT_IPI_ACK_LATE) || \
     defined(CK_AIC_MUTANT_NO_ENABLE)) &&                                                                         \
    !defined(CK_APPLE_HOST_TEST)
#error "CK_AIC_MUTANT_* is a host test mutant"
#endif

#define AIC1_INFO 0x0004u
#define AIC1_EVENT 0x2004u
#define AIC1_IPI_ACK 0x200cu
#define AIC1_IPI_MASK_SET 0x2024u
#define AIC1_TARGET_CPU 0x3000u
#define AIC1_MAX_IRQ 0x400u
#define AIC1_IPI_OTHER 1u
#define AIC1_IPI_SELF (1u << 31)

#define AIC2_INFO1 0x0004u
#define AIC2_INFO3 0x000cu
#define AIC2_CONFIG 0x0014u
#define AIC2_CONFIG_ENABLE 1u
#define AIC2_IRQ_CFG 0x2000u
#define AIC3_IRQ_CFG 0x10000u

#define CTL_ENABLE 1u
#define CTL_IMASK 2u
#define CTL_ISTATUS 4u

static uint32_t rd(const struct ck_aic *a, uint32_t off) { return a->io->rd32(a->io->ctx, a->base + off); }
static void wr(const struct ck_aic *a, uint32_t off, uint32_t v) { a->io->wr32(a->io->ctx, a->base + off, v); }

int ck_aic_init(struct ck_aic *a, const struct ck_aic_io *io, uint32_t version, uint64_t base, uint64_t size,
                uint64_t event, uint64_t event_size)
{
    struct ck_aic t = {0};
    uint32_t start;
    if (version < 1u || version > 3u) return CK_AIC_E_VERSION;
    t.io = io;
    t.version = version;
    t.base = base;
    if (version == 1u) {
        uint32_t info = io->rd32(io->ctx, base + AIC1_INFO);
        t.nr_irq = info & 0xffffu;
        t.max_irq = AIC1_MAX_IRQ;
        t.nr_die = t.max_die = 1;
        start = t.target_cpu = AIC1_TARGET_CPU;
        t.event = base + AIC1_EVENT;
    } else {
        if (event_size < 4u) return CK_AIC_E_EVENT;
        uint32_t info1 = io->rd32(io->ctx, base + AIC2_INFO1), info3 = io->rd32(io->ctx, base + AIC2_INFO3);
        t.nr_irq = info1 & 0xffffu;
        t.nr_die = ((info1 >> 24) & 0xfu) + 1u;
        t.max_irq = info3 & 0xffffu;
        t.max_die = (info3 >> 24) & 0xfu;
#ifdef CK_AIC_MUTANT_V3_OFFSET
        start = t.irq_cfg = AIC2_IRQ_CFG; /* AICv3 read as AIC2: wrong block */
#else
        start = t.irq_cfg = version == 2u ? AIC2_IRQ_CFG : AIC3_IRQ_CFG;
#endif
        t.event = event;
    }
    if (t.nr_irq == 0 || t.nr_irq > t.max_irq || (t.max_irq & 31u) || t.max_die == 0 || t.nr_die > t.max_die)
        return CK_AIC_E_INFO;
    uint32_t words = (t.max_irq / 32u) * 4u; /* bytes of one bitmap */
    uint32_t off = start + 4u * t.max_irq;   /* past TARGET_CPU / IRQ_CFG */
    t.sw_set = off;
    t.sw_clr = off + words;
    t.mask_set = off + 2u * words;
    t.mask_clr = off + 3u * words;
    t.hw_state = off + 4u * words;
    t.die_stride = off + 5u * words - start;
#ifdef CK_AIC_MUTANT_DIE_STRIDE
    t.die_stride += 4u; /* wrong die stride */
#endif
    uint64_t end = (uint64_t)start + (uint64_t)t.die_stride * t.nr_die;
    if (end > size) return CK_AIC_E_SIZE;
    if (version != 1u && t.event >= base + start && t.event < base + end) return CK_AIC_E_SIZE; /* event inside a die block */

    *a = t;
    for (uint32_t die = 0; die < a->nr_die; die++) {
        uint32_t d = die * a->die_stride;
        for (uint32_t i = 0; i < (a->nr_irq + 31u) / 32u; i++) {
            wr(a, a->mask_set + d + 4u * i, 0xffffffffu);
            wr(a, a->sw_clr + d + 4u * i, 0xffffffffu);
        }
        if (a->target_cpu)
            for (uint32_t i = 0; i < a->nr_irq; i++) wr(a, a->target_cpu + d + 4u * i, 1u);
    }
    if (version == 1u) {
        /* Fast IPIs carry every IPI here: the MMIO IPIs stay acked and masked. */
        wr(a, AIC1_IPI_ACK, AIC1_IPI_SELF | AIC1_IPI_OTHER);
        wr(a, AIC1_IPI_MASK_SET, AIC1_IPI_SELF | AIC1_IPI_OTHER);
    } else {
#ifdef CK_AIC_MUTANT_NO_ENABLE
        wr(a, AIC2_CONFIG, rd(a, AIC2_CONFIG) & ~AIC2_CONFIG_ENABLE); /* enable never set */
#else
        wr(a, AIC2_CONFIG, rd(a, AIC2_CONFIG) | AIC2_CONFIG_ENABLE);
#endif
    }
    return 0;
}

static int bit_op(struct ck_aic *a, uint32_t reg, uint32_t die, uint32_t irq)
{
    if (!a->io || die >= a->nr_die || irq >= a->nr_irq) return CK_AIC_E_IRQ;
    wr(a, reg + die * a->die_stride + 4u * (irq >> 5), 1u << (irq & 31u));
    return 0;
}

int ck_aic_mask(struct ck_aic *a, uint32_t die, uint32_t irq) { return bit_op(a, a->mask_set, die, irq); }
int ck_aic_unmask(struct ck_aic *a, uint32_t die, uint32_t irq) { return bit_op(a, a->mask_clr, die, irq); }
int ck_aic_sw_set(struct ck_aic *a, uint32_t die, uint32_t irq) { return bit_op(a, a->sw_set, die, irq); }
int ck_aic_sw_clr(struct ck_aic *a, uint32_t die, uint32_t irq) { return bit_op(a, a->sw_clr, die, irq); }

int ck_aic_target(struct ck_aic *a, uint32_t irq, uint32_t cpu_mask)
{
    if (!a->target_cpu) return CK_AIC_E_UNSUP;
    if (irq >= a->nr_irq) return CK_AIC_E_IRQ;
    wr(a, a->target_cpu + 4u * irq, cpu_mask);
    return 0;
}

uint32_t ck_aic_handle_irq(struct ck_aic *a, ck_aic_irq_fn on_irq, void *ctx, uint32_t budget)
{
    uint32_t n = 0;
    while (n < budget) {
        uint32_t ev = a->io->rd32(a->io->ctx, a->event);
        if (ev == 0) break;
        n++;
        uint32_t type = CK_AIC_EV_TYPE(ev), die = CK_AIC_EV_DIE(ev), num = CK_AIC_EV_NUM(ev);
        if (type == CK_AIC_EV_TYPE_IRQ && die < a->nr_die && num < a->nr_irq) {
            a->irqs++;
            if (!on_irq || on_irq(ctx, die, num)) ck_aic_unmask(a, die, num);
        } else if (type == CK_AIC_EV_TYPE_IPI) {
            a->ipis++; /* AIC1 MMIO IPI: masked at init, never expected */
        } else {
            a->spurious++;
        }
    }
    return n;
}

static int timer_firing(uint64_t ctl) { return (ctl & (CTL_ENABLE | CTL_IMASK | CTL_ISTATUS)) == (CTL_ENABLE | CTL_ISTATUS); }

uint32_t ck_aic_fiq_pending(const struct ck_aic_io *io, int apple)
{
    uint32_t p = 0;
    if (apple && (io->rdsr(io->ctx, CK_SR_IPI_SR) & 1u)) p |= CK_FIQ_IPI;
    if (timer_firing(io->rdsr(io->ctx, CK_SR_CNTP_CTL))) p |= CK_FIQ_TMR_PHYS;
    if (timer_firing(io->rdsr(io->ctx, CK_SR_CNTV_CTL))) p |= CK_FIQ_TMR_VIRT;
    if (apple) {
        uint64_t pm = io->rdsr(io->ctx, CK_SR_PMCR0);
        if (((pm >> 8) & 7u) == 4u && (pm & (1u << 11))) p |= CK_FIQ_PMC;
        uint64_t upm = io->rdsr(io->ctx, CK_SR_UPMCR0);
        if (((upm >> 16) & 7u) == 4u && (io->rdsr(io->ctx, CK_SR_UPMSR) & 1u)) p |= CK_FIQ_UPMC;
    }
    return p;
}

static void timer_after(struct ck_aic *a, const struct ck_aic_io *io, enum ck_aic_sr reg)
{
    uint64_t ctl = io->rdsr(io->ctx, reg);
    if (timer_firing(ctl)) { /* handler left it firing: mask, never loop */
        io->wrsr(io->ctx, reg, ctl | CTL_IMASK);
        if (a) a->storms++;
    }
}

uint32_t ck_aic_handle_fiq(struct ck_aic *a, const struct ck_aic_io *io, const struct ck_fiq_handlers *h)
{
    uint32_t p = ck_aic_fiq_pending(io, 1);
    if (p & CK_FIQ_IPI) {
#ifndef CK_AIC_MUTANT_IPI_ACK_LATE
        io->wrsr(io->ctx, CK_SR_IPI_SR, 1u);
#endif
        if (a) a->ipis++;
        if (h && h->ipi) h->ipi(h->ctx);
#ifdef CK_AIC_MUTANT_IPI_ACK_LATE
        io->wrsr(io->ctx, CK_SR_IPI_SR, 1u); /* ack after the handler: an IPI sent meanwhile is lost */
#endif
    }
    if (p & CK_FIQ_TMR_PHYS) {
        if (h && h->timer_phys) h->timer_phys(h->ctx);
        timer_after(a, io, CK_SR_CNTP_CTL);
    }
    if (p & CK_FIQ_TMR_VIRT) {
        if (h && h->timer_virt) h->timer_virt(h->ctx);
        timer_after(a, io, CK_SR_CNTV_CTL);
    }
    if (p & CK_FIQ_PMC) { /* nobody asked for PMU FIQs: IMODE off, IACT clear */
        uint64_t pm = io->rdsr(io->ctx, CK_SR_PMCR0);
        io->wrsr(io->ctx, CK_SR_PMCR0, pm & ~((uint64_t)7u << 8 | (uint64_t)1u << 11));
    }
    if (p & CK_FIQ_UPMC) {
        uint64_t upm = io->rdsr(io->ctx, CK_SR_UPMCR0);
        io->wrsr(io->ctx, CK_SR_UPMCR0, upm & ~((uint64_t)7u << 16));
    }
    return p;
}

void ck_aic_ipi_send(const struct ck_aic_io *io, uint64_t target)
{
    uint64_t me = io->rdsr(io->ctx, CK_SR_MPIDR);
    uint64_t core = target & 0xffu, cluster = (target >> 8) & 0xffu;
    if (((me >> 8) & 0xffu) == cluster)
        io->wrsr(io->ctx, CK_SR_IPI_RR_LOCAL, core);
    else
        io->wrsr(io->ctx, CK_SR_IPI_RR_GLOBAL, core | cluster << 16);
}
