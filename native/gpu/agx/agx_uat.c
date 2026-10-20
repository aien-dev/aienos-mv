/*
 * UAT page tables. Freestanding. Format and rights: see agx_uat.h.
 */
#include "agx_uat.h"

#define PGMSK (AGX_UAT_PGSZ - 1)
#define LVMSK ((1ull << AGX_UAT_LVBIT) - 1)
#define IASMSK ((1ull << AGX_UAT_IAS) - 1)

static void zero(uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		p[i] = 0;
}

int agx_pages_init(struct agx_pages *pg, const struct agx_mem *mem, uint64_t pa_start, uint64_t pa_end)
{
	if ((pa_start & PGMSK) || (pa_end & PGMSK) || pa_end < pa_start)
		return AGX_UAT_E_ALIGN;
	if (pa_end > pa_start && !agx_mem_ptr(mem, pa_start, pa_end - pa_start))
		return AGX_UAT_E_RANGE;
	pg->mem = mem;
	pg->next = pa_start;
	pg->end = pa_end;
	return AGX_UAT_OK;
}

int agx_pages_alloc(struct agx_pages *pg, uint64_t npages, uint64_t *pa)
{
	if (npages == 0 || npages > (pg->end - pg->next) >> AGX_UAT_PGBIT)
		return AGX_UAT_E_NOMEM;
	uint64_t bytes = npages << AGX_UAT_PGBIT;
	*pa = pg->next;
	pg->next += bytes;
	zero(agx_mem_ptr(pg->mem, *pa, bytes), bytes);
	return AGX_UAT_OK;
}

unsigned agx_pte_rights(uint64_t pte)
{
	/* Indexed [AP][(UXN << 1) | PXN], from drm/asahi's Prot constant names. */
	static const uint8_t gpu[4][4] = {
		{ 0, AGX_ACC_GPU_R, AGX_ACC_GPU_W, AGX_ACC_GPU_R | AGX_ACC_GPU_W }, /* FW_GPU_{NA,RO,WO,RW} */
		{ 0, 0, 0, AGX_ACC_GPU_R },                                         /* FW_RO, FW_NA, FW_RW, FW_RW_GPU_RO */
		{ AGX_ACC_GPU_R, AGX_ACC_GPU_W, AGX_ACC_GPU_R | AGX_ACC_GPU_W, 0 }, /* GPU_{RO,WO,RW,NA} */
		{ 0, 0, 0, 0 },
	};
	static const uint8_t fw[4][4] = {
		{ AGX_ACC_FW_R | AGX_ACC_FW_W, AGX_ACC_FW_R | AGX_ACC_FW_W, AGX_ACC_FW_R | AGX_ACC_FW_W,
		  AGX_ACC_FW_R | AGX_ACC_FW_W },
		{ AGX_ACC_FW_R, 0, AGX_ACC_FW_R | AGX_ACC_FW_W, AGX_ACC_FW_R | AGX_ACC_FW_W },
		{ 0, 0, 0, 0 },
		{ 0, 0, 0, 0 },
	};
	unsigned ap = (unsigned)(pte >> AGX_PTE_AP_SHIFT) & 3;
	unsigned high = (unsigned)(pte >> AGX_PTE_HIGH_SHIFT);
	unsigned idx = ((high & AGX_PTE_HIGH_UXN) ? 2u : 0u) | ((high & AGX_PTE_HIGH_PXN) ? 1u : 0u);
	unsigned r = fw[ap][idx];
	if (high & AGX_PTE_HIGH_GPU_ACCESS)
		r |= gpu[ap][idx];
	return r;
}

int agx_uat_init(struct agx_uat *t, struct agx_pages *pg, int kernel)
{
	t->kernel = (uint8_t)(kernel != 0);
	return agx_pages_alloc(pg, 1, &t->ttb_pa);
}

static unsigned lv_index(uint64_t va, unsigned level)
{
	return (unsigned)((va >> (AGX_UAT_PGBIT + level * AGX_UAT_LVBIT)) & LVMSK);
}

static int va_in_half(const struct agx_uat *t, uint64_t va, uint64_t size)
{
	if (t->kernel)
		return va >= AGX_UAT_KERN_BASE && va < AGX_UAT_KERN_TOP && size <= AGX_UAT_KERN_TOP - va;
	return va >= AGX_UAT_USER_BASE && va < AGX_UAT_USER_TOP && size <= AGX_UAT_USER_TOP - va;
}

/* Leaf descriptor slot for va; creates tables when pg != NULL. */
static uint8_t *leaf_slot(struct agx_uat *t, const struct agx_mem *mem, struct agx_pages *pg, uint64_t va, int *err)
{
	uint64_t table = t->ttb_pa;
	for (int level = AGX_UAT_LEVELS - 1; level > 0; level--) {
		uint8_t *e = agx_mem_ptr(mem, table + 8ull * lv_index(va & IASMSK, (unsigned)level), 8);
		if (!e) {
			*err = AGX_UAT_E_RANGE;
			return NULL;
		}
		uint64_t d = agx_rd64(e);
		if ((d & AGX_PTE_TYPE_MASK) != AGX_PTE_TYPE_TABLE_LEAF) {
			if (!pg) {
				*err = AGX_UAT_E_UNMAPPED;
				return NULL;
			}
			uint64_t npa;
			if ((*err = agx_pages_alloc(pg, 1, &npa)) != AGX_UAT_OK)
				return NULL;
			d = npa | AGX_PTE_TYPE_TABLE_LEAF;
			agx_wr64(e, d);
		}
		table = d & AGX_PTE_ADDR_MASK;
	}
	uint8_t *leaf = agx_mem_ptr(mem, table + 8ull * lv_index(va & IASMSK, 0), 8);
	if (!leaf)
		*err = AGX_UAT_E_RANGE;
	return leaf;
}

int agx_uat_map(struct agx_uat *t, struct agx_pages *pg, uint64_t va, uint64_t pa, uint64_t size, uint64_t prot)
{
	if ((va | pa | size) & PGMSK || size == 0)
		return AGX_UAT_E_ALIGN;
	if (!va_in_half(t, va, size) || !agx_mem_ptr(pg->mem, pa, size))
		return AGX_UAT_E_RANGE;
	/* Refuse before changing anything if any page is already mapped. */
	for (uint64_t off = 0; off < size; off += AGX_UAT_PGSZ) {
		int err = AGX_UAT_OK;
		uint8_t *e = leaf_slot(t, pg->mem, NULL, va + off, &err);
		if (e && (agx_rd64(e) & AGX_PTE_TYPE_MASK))
			return AGX_UAT_E_BUSY;
	}
	uint64_t bits = AGX_PTE_TYPE_TABLE_LEAF | (t->kernel ? 0 : AGX_PTE_NON_GLOBAL) | prot;
	for (uint64_t off = 0; off < size; off += AGX_UAT_PGSZ) {
		int err = AGX_UAT_OK;
		uint8_t *e = leaf_slot(t, pg->mem, pg, va + off, &err);
		if (!e) {
			if (off)
				agx_uat_unmap(t, pg->mem, va, off);
			return err;
		}
		agx_wr64(e, (pa + off) | bits);
	}
	return AGX_UAT_OK;
}

int agx_uat_unmap(struct agx_uat *t, const struct agx_mem *mem, uint64_t va, uint64_t size)
{
	if ((va | size) & PGMSK || size == 0)
		return AGX_UAT_E_ALIGN;
	if (!va_in_half(t, va, size))
		return AGX_UAT_E_RANGE;
	for (uint64_t off = 0; off < size; off += AGX_UAT_PGSZ) {
		int err = AGX_UAT_OK;
		uint8_t *e = leaf_slot(t, mem, NULL, va + off, &err);
		if (e)
			agx_wr64(e, 0);
	}
	return AGX_UAT_OK;
}

int agx_uat_translate(const struct agx_mem *mem, uint64_t ttbat_pa, unsigned slot, uint64_t va, uint64_t len,
		      unsigned access, uint64_t *pa)
{
	if (slot >= AGX_UAT_NUM_CTX || len == 0 || len > AGX_UAT_PGSZ - (va & PGMSK))
		return AGX_UAT_E_RANGE;
	int upper = va >= AGX_UAT_TTBR1_BASE;
	if (!upper && va >= AGX_UAT_USER_TOP)
		return AGX_UAT_E_UNMAPPED;
	uint8_t *te = agx_mem_ptr(mem, ttbat_pa + (uint64_t)slot * AGX_TTBAT_SLOT_BYTES + (upper ? 8 : 0), 8);
	if (!te)
		return AGX_UAT_E_RANGE;
	uint64_t ttbr = agx_rd64(te);
	if (!(ttbr & AGX_TTBR_VALID))
		return AGX_UAT_E_SLOT;
	struct agx_uat t = { .ttb_pa = ttbr & AGX_TTBR_ADDR_MASK, .kernel = (uint8_t)upper };
	int err = AGX_UAT_OK;
	uint8_t *e = leaf_slot(&t, mem, NULL, va, &err);
	if (!e)
		return err;
	uint64_t d = agx_rd64(e);
	if ((d & AGX_PTE_TYPE_MASK) != AGX_PTE_TYPE_TABLE_LEAF || !(d & AGX_PTE_AF))
		return AGX_UAT_E_UNMAPPED;
	if ((agx_pte_rights(d) & access) != access)
		return AGX_UAT_E_PERM;
	*pa = (d & AGX_PTE_ADDR_MASK) | (va & PGMSK);
	if (!agx_mem_ptr(mem, *pa, len))
		return AGX_UAT_E_RANGE;
	return AGX_UAT_OK;
}
