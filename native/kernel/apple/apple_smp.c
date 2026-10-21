/* apple_smp.c -- spin-table secondary CPU start (see apple_smp.h). */
#include "apple_smp.h"

/* TEST-ONLY mutants (apple.mk apple-mutants): host builds only. */
#if (defined(CK_SMP_MUTANT_NO_CLEAN) || defined(CK_SMP_MUTANT_NO_SEV) || defined(CK_SMP_MUTANT_NO_RSV_CHECK)) && \
    !defined(CK_APPLE_HOST_TEST)
#error "CK_SMP_MUTANT_* is a host test mutant"
#endif

#define RSV_MAX 256u /* bound on /memreserve/ entries walked */

const char *ck_smp_strerror(int e)
{
    switch (e) {
    case 0: return "ok";
    case CK_SMP_E_IDX: return "no such cpu node";
    case CK_SMP_E_BOOT: return "boot cpu";
    case CK_SMP_E_METHOD: return "enable-method is not spin-table";
    case CK_SMP_E_RELEASE: return "no aligned cpu-release-addr";
    case CK_SMP_E_RSV: return "release slot outside /memreserve/";
    case CK_SMP_E_IMAGE: return "release slot inside the kernel image";
    case CK_SMP_E_BUSY: return "release slot not zero";
    case CK_SMP_E_ENTRY: return "bad entry address";
    case CK_SMP_E_TIMEOUT: return "no check-in within the bound";
    case CK_SMP_E_CTRR: return "M4-class CTRR range on secondaries not reserved yet";
    default: return "unknown";
    }
}

int ck_smp_rsv_contains(const struct ck_fdt *f, uint64_t pa, uint64_t len)
{
    uint64_t a, s;
    if (len == 0 || pa + len < pa) return 0;
    for (uint32_t i = 0; i < RSV_MAX && ck_fdt_rsv(f, i, &a, &s) == 0; i++)
        if (pa >= a && pa - a <= s && len <= s - (pa - a)) return 1;
    return 0;
}

int ck_smp_spin_release(const struct ck_fdt *f, const struct ck_apple_platform *p, uint32_t idx, uint64_t boot_mpidr,
                        uint64_t entry, uint64_t img_lo, uint64_t img_hi, const struct ck_smp_io *io, uint32_t polls)
{
    if (idx >= p->ncpus || idx >= CK_APPLE_MAX_CPUS) return CK_SMP_E_IDX;
    const struct ck_apple_cpu *c = &p->cpu[idx];
    if ((c->mpidr & CK_SMP_AFF_MASK) == (boot_mpidr & CK_SMP_AFF_MASK)) return CK_SMP_E_BOOT;
    if (!c->spin_table) return CK_SMP_E_METHOD;
    if (p->soc && p->soc->ctrr_hole) return CK_SMP_E_CTRR;
    uint64_t slot = c->release;
    if (slot == 0 || (slot & 7u)) return CK_SMP_E_RELEASE;
#ifdef CK_SMP_MUTANT_NO_RSV_CHECK
    (void)f; /* slot accepted wherever it lies */
#else
    if (!ck_smp_rsv_contains(f, slot, 8)) return CK_SMP_E_RSV;
#endif
    if (slot + 8u > img_lo && slot < img_hi) return CK_SMP_E_IMAGE;
    if (entry == 0 || (entry & 3u)) return CK_SMP_E_ENTRY;
    if (io->rd64(io->ctx, slot) != 0) return CK_SMP_E_BUSY;

    io->wr64(io->ctx, slot, entry);
#ifndef CK_SMP_MUTANT_NO_CLEAN
    io->clean(io->ctx, slot); /* the parked core reads memory, not our cache */
#endif
#ifndef CK_SMP_MUTANT_NO_SEV
    io->sev(io->ctx);
#endif
    for (uint32_t i = 0; i < polls; i++)
        if (io->checkin(io->ctx, idx)) return 0;
    return CK_SMP_E_TIMEOUT;
}
