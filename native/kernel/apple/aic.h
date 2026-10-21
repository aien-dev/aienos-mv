/* aic.h -- Apple Interrupt Controller (AIC1, AIC2, AICv3) and the FIQ path.
 *
 * Register facts were read from Linux drivers/irqchip/irq-apple-aic.c and
 * arch/arm64/include/asm/apple_m1_pmu.h (AsahiLinux/linux 94fb2334, GPL,
 * read-only reference): no code copied, every offset below re-derived and
 * pinned by the chip models in tests/test_apple_aic.c.
 *
 * MMIO layout (offsets from the "core" reg):
 *   AIC1: INFO 0x0004 (bits 15:0 nr_irq), WHOAMI 0x2000, EVENT 0x2004,
 *         TARGET_CPU 0x3000 (u32 per IRQ, max 0x400 IRQs), then SW_SET,
 *         SW_CLR, MASK_SET, MASK_CLR, HW_STATE (u32 per 32 IRQs each).
 *   AIC2/AICv3: VERSION 0x0000, INFO1 0x0004 (15:0 nr_irq, 27:24 last die),
 *         INFO3 0x000c (15:0 max_irq, 27:24 max_die), CONFIG 0x0014 (bit 0
 *         enable); per die, from IRQ_CFG (0x2000 AIC2, 0x10000 AICv3):
 *         IRQ_CFG (u32 per IRQ), SW_SET, SW_CLR, MASK_SET, MASK_CLR,
 *         HW_STATE (u32 per 32 IRQs); die stride = the size of that block.
 *         The event register is the separate "event" reg of the tree.
 *   EVENT value: 31:24 die, 23:16 type (1 IRQ, 4 IPI), 15:0 number; reading
 *   it acknowledges and masks that IRQ (auto-mask), so the handler unmasks.
 *
 * FIQ (every Apple chip routes these to the FIQ vector, not through AIC
 * MMIO): fast IPIs (IMP-DEF IPI_SR_EL1 bit 0 pending, write 1 to clear;
 * IPI_RR_LOCAL_EL1 / IPI_RR_GLOBAL_EL1 to send), the architected timers
 * (CNTP/CNTV: firing = ENABLE and ISTATUS set, IMASK clear), and the core
 * and uncore PMU (PMCR0 IMODE 10:8 == 4 with IACT bit 11; UPMCR0 IMODE
 * 18:16 == 4 with UPMSR bit 0). The table rows (t8103 and later) all use
 * fast IPIs, so the AIC1 MMIO IPI registers are never used here.
 *
 * Every MMIO and system register access goes through ck_aic_io so the same
 * code runs against host chip models; the kernel passes real accessors.
 * Every loop is bounded. */
#ifndef AIENOS_CK_APPLE_AIC_H
#define AIENOS_CK_APPLE_AIC_H
#include <stdint.h>

struct ck_apple_soc;

/* System registers the FIQ path uses (index into ck_aic_io sysreg calls). */
enum ck_aic_sr {
    CK_SR_IPI_SR,        /* S3_5_C15_C1_1 IPI_SR_EL1 */
    CK_SR_IPI_RR_LOCAL,  /* S3_5_C15_C0_0 IPI_RR_LOCAL_EL1 */
    CK_SR_IPI_RR_GLOBAL, /* S3_5_C15_C0_1 IPI_RR_GLOBAL_EL1 */
    CK_SR_CNTP_CTL,      /* CNTP_CTL_EL0 */
    CK_SR_CNTV_CTL,      /* CNTV_CTL_EL0 */
    CK_SR_PMCR0,         /* S3_1_C15_C0_0 PMCR0_EL1 */
    CK_SR_UPMCR0,        /* S3_7_C15_C0_4 UPMCR0_EL1 */
    CK_SR_UPMSR,         /* S3_7_C15_C6_4 UPMSR_EL1 */
    CK_SR_MPIDR,         /* MPIDR_EL1 */
    CK_SR_COUNT
};

struct ck_aic_io {
    uint32_t (*rd32)(void *ctx, uint64_t addr);
    void (*wr32)(void *ctx, uint64_t addr, uint32_t v);
    uint64_t (*rdsr)(void *ctx, enum ck_aic_sr r);
    void (*wrsr)(void *ctx, enum ck_aic_sr r, uint64_t v);
    void *ctx;
};

#define CK_AIC_EV_DIE(e) (((e) >> 24) & 0xffu)
#define CK_AIC_EV_TYPE(e) (((e) >> 16) & 0xffu)
#define CK_AIC_EV_NUM(e) ((e) & 0xffffu)
#define CK_AIC_EV_TYPE_IRQ 1u
#define CK_AIC_EV_TYPE_IPI 4u

#define CK_AIC_E_VERSION (-1) /* not 1, 2 or 3 */
#define CK_AIC_E_INFO (-2)    /* INFO registers out of range (0 IRQs, nr > max, dies) */
#define CK_AIC_E_SIZE (-3)    /* register block larger than the tree's reg size */
#define CK_AIC_E_EVENT (-4)   /* AIC2/3 without an event register */
#define CK_AIC_E_IRQ (-5)     /* die/irq outside nr_die/nr_irq */
#define CK_AIC_E_UNSUP (-6)   /* operation this version does not have */

struct ck_aic {
    const struct ck_aic_io *io;
    uint32_t version;
    uint64_t base;           /* core registers */
    uint64_t event;          /* address of the event register */
    uint32_t nr_irq, max_irq, nr_die, max_die;
    uint32_t target_cpu;     /* AIC1 only, else 0 */
    uint32_t irq_cfg;        /* AIC2/3 only, else 0 */
    uint32_t sw_set, sw_clr, mask_set, mask_clr, hw_state, die_stride;
    /* counters for the report */
    uint32_t irqs, ipis, spurious, storms;
};

/* Reads INFO, derives the layout, checks it fits the tree's reg sizes, masks
 * and clears every IRQ of every die, AIC1: targets every IRQ at CPU 0,
 * AIC2/3: sets CONFIG enable. Nothing is touched when a check fails. */
int ck_aic_init(struct ck_aic *a, const struct ck_aic_io *io, uint32_t version, uint64_t base, uint64_t size,
                uint64_t event, uint64_t event_size);

int ck_aic_mask(struct ck_aic *a, uint32_t die, uint32_t irq);
int ck_aic_unmask(struct ck_aic *a, uint32_t die, uint32_t irq);
int ck_aic_sw_set(struct ck_aic *a, uint32_t die, uint32_t irq);
int ck_aic_sw_clr(struct ck_aic *a, uint32_t die, uint32_t irq);
/* AIC1: route IRQ to the CPUs in cpu_mask (TARGET_CPU); others: E_UNSUP. */
int ck_aic_target(struct ck_aic *a, uint32_t irq, uint32_t cpu_mask);

/* IRQ vector: drain the event register (at most `budget` events) and call
 * on_irq for each hardware IRQ. Reading the event masked that IRQ; it is
 * unmasked again only when on_irq returns nonzero (0 keeps it masked).
 * Other event types are counted. Returns the number of events read. */
typedef int (*ck_aic_irq_fn)(void *ctx, uint32_t die, uint32_t irq);
uint32_t ck_aic_handle_irq(struct ck_aic *a, ck_aic_irq_fn on_irq, void *ctx, uint32_t budget);

/* FIQ sources pending in a register snapshot. */
#define CK_FIQ_IPI 1u
#define CK_FIQ_TMR_PHYS 2u
#define CK_FIQ_TMR_VIRT 4u
#define CK_FIQ_PMC 8u
#define CK_FIQ_UPMC 16u
uint32_t ck_aic_fiq_pending(const struct ck_aic_io *io, int apple);

/* FIQ vector: acknowledge the fast IPI first (IPI_SR write 1, before the
 * handler, so an IPI sent during the handler is not lost), call the timer
 * handlers, then turn off PMU FIQs nobody asked for. A timer still firing
 * after its handler is masked (IMASK) and counted as a storm instead of
 * re-entering forever. */
struct ck_fiq_handlers {
    void (*ipi)(void *ctx);
    void (*timer_phys)(void *ctx);
    void (*timer_virt)(void *ctx);
    void *ctx;
};
uint32_t ck_aic_handle_fiq(struct ck_aic *a, const struct ck_aic_io *io, const struct ck_fiq_handlers *h);

/* Fast IPI to the CPU with MPIDR `target`: same cluster (Aff1) as the
 * sender -> IPI_RR_LOCAL with Aff0, else IPI_RR_GLOBAL with Aff0 | Aff1 << 16. */
void ck_aic_ipi_send(const struct ck_aic_io *io, uint64_t target_mpidr);

/* Kernel: live FIQ sources on this CPU. With soc == NULL (not an Apple chip)
 * only the architected timers are read: Apple IMP-DEF registers would fault. */
uint32_t ck_aic_fiq_pending_live(const struct ck_apple_soc *soc);
/* Kernel accessors (real MMIO and system registers). */
extern const struct ck_aic_io ck_aic_io_live;

#endif
