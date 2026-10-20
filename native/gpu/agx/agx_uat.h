/*
 * Apple GPU MMU (UAT) page tables: physical memory window, page allocator,
 * mapping (driver side) and translation (device side, used by the
 * simulator). Freestanding: no heap, no clock, no I/O.
 *
 * Format INFERRED from drm/asahi pgtable.rs and mmu.rs (GPL-2.0-only OR MIT,
 * used under MIT) at 94fb23346d52: 16 KiB pages, 3 levels of 2048 entries,
 * 39-bit input addresses, ARM64-like descriptors (type bits 0..1 = 3 for
 * tables and pages), AP in bits 6..7, PXN/UXN/GPU-access in bits 52 and up.
 * A TTBAT holds {ttb0, ttb1} per context; VAs at or above 0xffffff8000000000
 * walk ttb1.
 *
 * Access rights per (AP, UXN, PXN) are INFERRED from the names drm/asahi gives
 * its Prot constants (PROT_GPU_RO = AP_GPU,0,0 and so on). Whether the
 * hardware enforces exactly that split is UNKNOWN; the trace (row U2) and a
 * deliberate write to a GPU_RO buffer on the real chip settle it.
 */
#ifndef AIENOS_AGX_UAT_H
#define AIENOS_AGX_UAT_H

#include <stddef.h>
#include <stdint.h>

#include "agx_fw.h"

/* A window of physical memory the CPU can reach (all of the simulator's
 * DRAM, or a carveout on hardware). */
struct agx_mem {
	uint8_t *cpu;
	uint64_t pa_base;
	uint64_t size;
};

/* CPU pointer for [pa, pa + len), or NULL when any byte lies outside. */
static inline uint8_t *agx_mem_ptr(const struct agx_mem *m, uint64_t pa, uint64_t len)
{
	if (pa < m->pa_base || len > m->size || pa - m->pa_base > m->size - len)
		return NULL;
	return m->cpu + (pa - m->pa_base);
}

/* Bump allocator of zeroed 16 KiB pages over [next, end). */
struct agx_pages {
	const struct agx_mem *mem;
	uint64_t next;
	uint64_t end;
};

enum agx_uat_err {
	AGX_UAT_OK = 0,
	AGX_UAT_E_NOMEM = -1,   /* page allocator exhausted */
	AGX_UAT_E_ALIGN = -2,   /* VA, PA or size not 16 KiB aligned */
	AGX_UAT_E_RANGE = -3,   /* VA outside the table's half, or PA outside memory */
	AGX_UAT_E_BUSY = -4,    /* map over an existing mapping */
	AGX_UAT_E_UNMAPPED = -5, /* translate: no valid descriptor */
	AGX_UAT_E_PERM = -6,    /* translate: access not allowed */
	AGX_UAT_E_SLOT = -7,    /* translate: context slot not bound */
};

int agx_pages_init(struct agx_pages *pg, const struct agx_mem *mem, uint64_t pa_start, uint64_t pa_end);
int agx_pages_alloc(struct agx_pages *pg, uint64_t npages, uint64_t *pa);

/* PTE protection bits (without type and address). */
static inline uint64_t agx_prot(unsigned ap, unsigned uxn, unsigned pxn, unsigned memattr)
{
	uint64_t high = AGX_PTE_HIGH_GPU_ACCESS | (pxn ? AGX_PTE_HIGH_PXN : 0) | (uxn ? AGX_PTE_HIGH_UXN : 0);
	return (uint64_t)ap << AGX_PTE_AP_SHIFT | high << AGX_PTE_HIGH_SHIFT |
	       (uint64_t)memattr << AGX_PTE_MEMATTR_SHIFT | AGX_PTE_AF;
}

/* The protections the compute path uses, as drm/asahi names them. */
#define AGX_PROT_GPU_SHARED_RW agx_prot(AGX_AP_GPU, 1, 0, AGX_MEMATTR_UNCACHED)
#define AGX_PROT_GPU_SHARED_RO agx_prot(AGX_AP_GPU, 0, 0, AGX_MEMATTR_UNCACHED)
#define AGX_PROT_FW_SHARED_RW agx_prot(AGX_AP_FW, 1, 0, AGX_MEMATTR_UNCACHED)
#define AGX_PROT_GPU_RO_FW_PRIV_RW agx_prot(AGX_AP_FW, 1, 1, AGX_MEMATTR_CACHED)
#define AGX_PROT_GPU_FW_SHARED_RW agx_prot(AGX_AP_FW_GPU, 1, 1, AGX_MEMATTR_UNCACHED)

enum agx_access {
	AGX_ACC_GPU_R = 1, /* shader, CDM and USC fetches */
	AGX_ACC_GPU_W = 2,
	AGX_ACC_FW_R = 4,  /* the GPU's firmware coprocessor */
	AGX_ACC_FW_W = 8,
};

/* Which of enum agx_access a leaf descriptor allows. */
unsigned agx_pte_rights(uint64_t pte);

/* One page table tree. kernel = 1: upper half (ttb1), global entries;
 * kernel = 0: user half (ttb0), non-global entries. */
struct agx_uat {
	uint64_t ttb_pa;
	uint8_t kernel;
};

int agx_uat_init(struct agx_uat *t, struct agx_pages *pg, int kernel);
int agx_uat_map(struct agx_uat *t, struct agx_pages *pg, uint64_t va, uint64_t pa, uint64_t size, uint64_t prot);
int agx_uat_unmap(struct agx_uat *t, const struct agx_mem *mem, uint64_t va, uint64_t size);

/* Value for a TTBAT entry. */
static inline uint64_t agx_ttbr(const struct agx_uat *t, unsigned slot)
{
	return t->ttb_pa | AGX_TTBR_VALID | (uint64_t)slot << AGX_TTBR_ASID_SHIFT;
}

/* Device side: translate va in context slot through the TTBAT at ttbat_pa.
 * Returns AGX_UAT_OK with *pa set, or an error. The translation must cover
 * len bytes inside one page. */
int agx_uat_translate(const struct agx_mem *mem, uint64_t ttbat_pa, unsigned slot, uint64_t va, uint64_t len,
		      unsigned access, uint64_t *pa);

#endif
