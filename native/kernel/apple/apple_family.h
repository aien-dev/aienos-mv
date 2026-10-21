/* apple_family.h -- the Apple Silicon family table and the platform probe.
 *
 * One table row per Apple SoC we hold a pinned device tree for: the root
 * compatible ("apple,t8112") picks the row, and the row carries everything
 * the platform layer must decide per chip (interrupt controller version,
 * idle policy, CPU start method, DART page size). ck_apple_probe reads the
 * device tree, picks the row and cross-checks the row against the tree (the
 * interrupt controller node must say the version the row says). A root
 * compatible that is not in the table is REFUSED with one plain report line
 * and no Apple driver starts: the layer never guesses a chip.
 *
 * Facts: SOURCED from the AsahiLinux/linux device trees at 94fb2334
 * (arch/arm64/boot/dts/apple, "GPL-2.0+ OR MIT", read and compiled as test
 * fixtures only, never into the kernel), except the idle policy (see
 * CK_APPLE_IDLE_*), which docs/APPLE_SILICON_PLATFORM.md marks per row. */
#ifndef AIENOS_CK_APPLE_FAMILY_H
#define AIENOS_CK_APPLE_FAMILY_H
#include <stddef.h>
#include <stdint.h>
#include "fdt.h"

#define CK_APPLE_AIC_V1 1u
#define CK_APPLE_AIC_V2 2u
#define CK_APPLE_AIC_V3 3u

/* Idle policy. WFI_RETAINED: WFI is allowed once our own state-retention
 * setup has run (M1 to M3). SPIN: WFI loses core state because the boot
 * firmware locks the controls that keep it (M4 and later), so idle spins
 * until a state-saving idle path exists. */
#define CK_APPLE_IDLE_WFI_RETAINED 1u
#define CK_APPLE_IDLE_SPIN 2u

/* CPU start: Apple chips have no EL3 and no PSCI. Secondaries are released
 * through the spin table the loader (m1n1) fills in each cpu node
 * (cpu-release-addr), after it set each core's reset vector (RVBAR). */
#define CK_APPLE_START_SPIN_TABLE 1u

/* USB-C port controller, read from the tree (it is a per-board choice: the
 * M3 MacBook Air uses ACE3 over SPMI, the M3 MacBook Pro 14 CD3217 over I2C). */
#define CK_APPLE_USBC_NONE 0u        /* no controller node in the tree */
#define CK_APPLE_USBC_CD3217_I2C 1u  /* compatible "apple,cd321x" */
#define CK_APPLE_USBC_ACE3_SPMI 2u   /* compatible "apple,sn201202x" */

struct ck_apple_soc {
    const char *compat;     /* root compatible, "apple,t8112" */
    uint32_t soc_id;        /* 0x8112 */
    const char *name;       /* "M2" */
    uint8_t gen;            /* 1 = M1 ... */
    uint8_t aic;            /* CK_APPLE_AIC_V* */
    uint8_t idle;           /* CK_APPLE_IDLE_* */
    uint8_t start;          /* CK_APPLE_START_* */
    uint8_t dies;           /* AIC dies (2 on Ultra parts) */
    uint8_t usbc_seen;      /* bits (1 << CK_APPLE_USBC_*) seen in the pinned boards; 0 = none described yet */
    uint16_t max_cpus;      /* cpu nodes in the pinned tree */
    uint32_t dart_page;     /* DART translation granule, bytes */
    const char *aic_compat; /* compatible the interrupt controller node must carry */
};

/* NULL if `compat` is not a table row. */
const struct ck_apple_soc *ck_apple_soc_by_compat(const char *compat);
/* Row i of the table, NULL past the end. */
const struct ck_apple_soc *ck_apple_soc_at(uint32_t i);

#define CK_APPLE_MAX_CPUS 64u

struct ck_apple_cpu {
    uint64_t mpidr;        /* cpu node reg */
    uint64_t release;      /* cpu-release-addr, 0 = not filled by the loader */
    uint8_t spin_table;    /* enable-method is "spin-table" */
};

#define CK_APPLE_OK 0
#define CK_APPLE_NOT_APPLE 1    /* root compatible lacks "apple,arm-platform" */
#define CK_APPLE_UNKNOWN_SOC 2  /* Apple platform, SoC not in the table */
#define CK_APPLE_MISMATCH 3     /* known SoC, tree disagrees with the row */

struct ck_apple_platform {
    int verdict;                    /* CK_APPLE_* */
    const struct ck_apple_soc *soc; /* NULL unless verdict is OK or MISMATCH */
    char why[160];                  /* plain reason when not OK */
    const char *model;              /* root "model" or "" */
    uint64_t aic_base, aic_size;    /* "core" registers (reg 0) */
    uint64_t aic_event, aic_event_size; /* AIC2/3 "event" reg; AIC1: aic_base, 0 */
    uint32_t ncpus;
    struct ck_apple_cpu cpu[CK_APPLE_MAX_CPUS];
    uint8_t usbc;                   /* CK_APPLE_USBC_* from the tree */
};

/* Reads the tree into *p. Returns p->verdict. */
int ck_apple_probe(const struct ck_fdt *f, struct ck_apple_platform *p);

/* One report line (no newline) into buf: "apple: soc=t8112 ..." or
 * "apple: REFUSED ...; Apple drivers not started". Returns ck_snprintf's count. */
int ck_apple_report(const struct ck_apple_platform *p, char *buf, size_t n);

const char *ck_apple_idle_name(uint8_t idle);
const char *ck_apple_usbc_name(uint8_t usbc);

#endif
