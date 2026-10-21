/* apple_smp.h -- secondary CPU start through the spin table (Apple chips
 * have no EL3 and no PSCI; m1n1 parks every secondary core polling its
 * cpu-release-addr).
 *
 * Protocol (Linux Documentation/arch/arm64/booting.rst, "spin-table"
 * enable-method; read-only reference, no code copied): cpu-release-addr is a
 * naturally aligned, zero-initialised 64-bit location contained in a
 * /memreserve/ region; the parked CPU (possibly in wfe) jumps to the value
 * once it reads non-zero; the value is one 64-bit little-endian store; the
 * primary issues sev. The parked CPU may read memory with its caches off, so
 * the primary cleans the line to the point of coherency before the sev
 * (Linux smp_spin_table.c does the same; INFERRED necessary on Apple cores,
 * docs/APPLE_SILICON_PLATFORM.md).
 *
 * Every access goes through ck_smp_io: the kernel passes real accessors,
 * the host test (tests/test_apple_smp.c) a model whose parked cores only see
 * memory, not the primary's cache. Every wait is bounded. */
#ifndef AIENOS_CK_APPLE_SMP_H
#define AIENOS_CK_APPLE_SMP_H
#include <stdint.h>

#include "apple_family.h"
#include "fdt.h"

struct ck_smp_io {
    uint64_t (*rd64)(void *ctx, uint64_t pa);
    void (*wr64)(void *ctx, uint64_t pa, uint64_t v); /* one 64-bit store */
    void (*clean)(void *ctx, uint64_t pa);            /* clean+invalidate the line to PoC, then dsb */
    void (*sev)(void *ctx);
    uint32_t (*checkin)(void *ctx, uint32_t idx);     /* nonzero once CPU idx runs our code; one poll */
    void *ctx;
};

#define CK_SMP_E_IDX (-1)     /* no such cpu node */
#define CK_SMP_E_BOOT (-2)    /* that is the CPU we run on */
#define CK_SMP_E_METHOD (-3)  /* enable-method is not "spin-table" */
#define CK_SMP_E_RELEASE (-4) /* cpu-release-addr absent (0) or not 8-byte aligned */
#define CK_SMP_E_RSV (-5)     /* release slot outside every /memreserve/ region */
#define CK_SMP_E_IMAGE (-6)   /* release slot inside our own image */
#define CK_SMP_E_BUSY (-7)    /* release slot not zero: already released or not a parked CPU */
#define CK_SMP_E_ENTRY (-8)   /* entry address 0 or not 4-byte aligned */
#define CK_SMP_E_TIMEOUT (-9) /* released, never checked in within the bound */
#define CK_SMP_E_CTRR (-10)   /* M4-class: CTRR range on secondaries not reserved yet */

const char *ck_smp_strerror(int e);

/* MPIDR affinity fields (Aff3, Aff2, Aff1, Aff0): the CPU identity. */
#define CK_SMP_AFF_MASK 0xff00ffffffull

/* 1 when [pa, pa+len) lies inside one /memreserve/ entry of the tree. */
int ck_smp_rsv_contains(const struct ck_fdt *f, uint64_t pa, uint64_t len);

/* M4-class rows (ctrr_hole): the secondaries keep a CTRR range enabled that
 * faults when written; the range comes from CTRR_C_LWR/UPR_EL1 (L372 family
 * map section 5). Until Aienux reads and reserves it, those secondaries are
 * refused (CK_SMP_E_CTRR), never started. */
/* Checks cpu idx of the probed platform against every rule above, then
 * writes `entry` to its release slot, cleans the line, sev, and polls
 * io->checkin at most `polls` times. Nothing is written when a check fails.
 * [img_lo, img_hi) is our own image (the slot must not lie in it). */
int ck_smp_spin_release(const struct ck_fdt *f, const struct ck_apple_platform *p, uint32_t idx, uint64_t boot_mpidr,
                        uint64_t entry, uint64_t img_lo, uint64_t img_hi, const struct ck_smp_io *io, uint32_t polls);

#endif
