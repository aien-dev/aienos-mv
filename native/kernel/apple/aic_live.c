/* aic_live.c -- real MMIO and system register accessors for aic.c (kernel
 * image only; the host tests use chip models instead). Apple IMP-DEF
 * registers are only read when the caller says the CPU is an Apple chip. */
#include "aic.h"

static uint32_t live_rd32(void *ctx, uint64_t addr)
{
    (void)ctx;
    uint32_t v = *(volatile uint32_t *)(uintptr_t)addr;
    __asm__ volatile("dmb oshld" ::: "memory"); /* later reads (DMA data) ordered after the event read */
    return v;
}

static void live_wr32(void *ctx, uint64_t addr, uint32_t v)
{
    (void)ctx;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)addr = v;
}

#define MRS(name) ({ uint64_t v_; __asm__ volatile("mrs %0, " name : "=r"(v_)); v_; })
#define MSR(name, v) __asm__ volatile("msr " name ", %0\n\tisb" ::"r"((uint64_t)(v)) : "memory")

static uint64_t live_rdsr(void *ctx, enum ck_aic_sr r)
{
    (void)ctx;
    switch (r) {
    case CK_SR_IPI_SR: return MRS("S3_5_C15_C1_1");
    case CK_SR_CNTP_CTL: return MRS("cntp_ctl_el0");
    case CK_SR_CNTV_CTL: return MRS("cntv_ctl_el0");
    case CK_SR_PMCR0: return MRS("S3_1_C15_C0_0");
    case CK_SR_UPMCR0: return MRS("S3_7_C15_C0_4");
    case CK_SR_UPMSR: return MRS("S3_7_C15_C6_4");
    case CK_SR_MPIDR: return MRS("mpidr_el1");
    default: return 0; /* IPI_RR_* are write-only here */
    }
}

static void live_wrsr(void *ctx, enum ck_aic_sr r, uint64_t v)
{
    (void)ctx;
    switch (r) {
    case CK_SR_IPI_SR: MSR("S3_5_C15_C1_1", v); break;
    case CK_SR_IPI_RR_LOCAL: MSR("S3_5_C15_C0_0", v); break;
    case CK_SR_IPI_RR_GLOBAL: MSR("S3_5_C15_C0_1", v); break;
    case CK_SR_CNTP_CTL: MSR("cntp_ctl_el0", v); break;
    case CK_SR_CNTV_CTL: MSR("cntv_ctl_el0", v); break;
    case CK_SR_PMCR0: MSR("S3_1_C15_C0_0", v); break;
    case CK_SR_UPMCR0: MSR("S3_7_C15_C0_4", v); break;
    default: break;
    }
}

const struct ck_aic_io ck_aic_io_live = {live_rd32, live_wr32, live_rdsr, live_wrsr, 0};

uint32_t ck_aic_fiq_pending_live(const struct ck_apple_soc *soc)
{
    return ck_aic_fiq_pending(&ck_aic_io_live, soc != 0);
}
