/* native/kernel/gpu/pm4.c -- PM4 type-3 compute packet implementation.
 * See pm4.h for register attribution.
 */
#include "pm4.h"

/* ---- Internal helpers ---- */
static int pm4_hdr(pm4_ring_t *r, uint32_t op, uint32_t ndw)
{
    return pm4_write(r, PM4_TYPE3_HDR(op, ndw));
}

/* ---- SET_SH_REG ---- */
/* Packet: header, start_reg_offset, data[0..n-1]
 * Total: 2 + count DWORDs. */
int pm4_set_sh_reg(pm4_ring_t *r, uint32_t mm_reg,
                   const uint32_t *data, uint32_t count)
{
    if (count == 0)
        return 0;
    if (pm4_hdr(r, PM4_IT_SET_SH_REG, 1 + count) != 0)
        return -1;
    /* OFFSET = (mm_reg - SH_REG_BASE) */
    if (pm4_write(r, (mm_reg - GCN_SH_REG_BASE) & 0xffffu) != 0)
        return -1;
    for (uint32_t i = 0; i < count; i++)
        if (pm4_write(r, data[i]) != 0)
            return -1;
    return 0;
}

int pm4_set_sh_reg1(pm4_ring_t *r, uint32_t mm_reg, uint32_t val)
{
    return pm4_set_sh_reg(r, mm_reg, &val, 1);
}

/* ---- DISPATCH_DIRECT ---- */
/* Packet: header, dim_x, dim_y, dim_z, dispatch_initiator  (5 DWORDs total) */
int pm4_dispatch_direct(pm4_ring_t *r,
                        uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                        uint32_t dispatch_initiator)
{
    if (pm4_hdr(r, PM4_IT_DISPATCH_DIRECT, 4) != 0)
        return -1;
    if (pm4_write(r, dim_x) != 0)            return -1;
    if (pm4_write(r, dim_y) != 0)            return -1;
    if (pm4_write(r, dim_z) != 0)            return -1;
    if (pm4_write(r, dispatch_initiator) != 0) return -1;
    return 0;
}

/* ---- ACQUIRE_MEM ---- */
/* Packet: header, CP_COHER_CNTL, CP_COHER_SIZE_LO, CP_COHER_SIZE_HI,
 *         CP_COHER_BASE_LO, CP_COHER_BASE_HI, POLL_INTERVAL (7 DWORDs).
 *
 * coher_cntl bit fields (from vi_structs.h):
 *   bit 25 = TC_ACTION_ENA (L2 cache flush/invalidate)
 *   bit 26 = TC_WB_ACTION_ENA (L2 write-back)
 *   bit 29 = SH_ICACHE_ACTION_ENA (instruction cache invalidate)
 *   bit 30 = SH_KCACHE_ACTION_ENA (scalar cache invalidate)
 *
 * Full barrier (compute L2 + shader caches): 0x00600000 = bits 25+26.
 * Instruction cache only: 0x60000000 = bits 29+30. */
int pm4_acquire_mem(pm4_ring_t *r, uint32_t coher_cntl)
{
    if (pm4_hdr(r, PM4_IT_ACQUIRE_MEM, 6) != 0)
        return -1;
    if (pm4_write(r, coher_cntl) != 0)     return -1; /* CP_COHER_CNTL */
    if (pm4_write(r, 0xFFFFFFFFu) != 0)    return -1; /* CP_COHER_SIZE lo = all */
    if (pm4_write(r, 0x000FFFFFu) != 0)    return -1; /* CP_COHER_SIZE hi = all */
    if (pm4_write(r, 0u) != 0)             return -1; /* CP_COHER_BASE lo */
    if (pm4_write(r, 0u) != 0)             return -1; /* CP_COHER_BASE hi */
    if (pm4_write(r, 0x000Au) != 0)        return -1; /* POLL_INTERVAL = 10 */
    return 0;
}

/* ---- RELEASE_MEM / EVENT_WRITE_EOP ---- */
/* Packet for signalling a fence after GPU work completes.
 * Packet body (8 DWORDs after header):
 *   event_cntl: event_type | INDEX(4) | DATA_SEL(1=32b, 2=64b value, 4=timestamp)
 *   event_type2: unused
 *   addr_lo: fence address low 32b
 *   addr_hi: fence address high 32b
 *   data_lo: value low 32b
 *   data_hi: value high 32b
 *   INT_SEL: 0=no interrupt
 *   unused
 */
int pm4_release_mem_fence(pm4_ring_t *r,
                          uint32_t event_type,
                          uint64_t addr,
                          uint64_t value)
{
    /* Use RELEASE_MEM (0x49) with INDEX=4 (EOP), DATA_SEL=1 (64b) */
    uint32_t event_cntl = (event_type & 0x3fu)          /* bits[5:0] */
                        | (4u << 8)                      /* INDEX=4: EOP */
                        | (1u << 16)                     /* DATA_SEL=1: 32b/64b write */
                        | (0u << 24);                    /* INT_SEL=0: no interrupt */
    if (pm4_hdr(r, PM4_IT_RELEASE_MEM, 6) != 0)
        return -1;
    if (pm4_write(r, event_cntl) != 0)                       return -1;
    if (pm4_write(r, 0u) != 0)                                return -1;
    if (pm4_write(r, (uint32_t)(addr)) != 0)                  return -1;
    if (pm4_write(r, (uint32_t)(addr >> 32)) != 0)            return -1;
    if (pm4_write(r, (uint32_t)(value)) != 0)                 return -1;
    if (pm4_write(r, (uint32_t)(value >> 32)) != 0)           return -1;
    return 0;
}

/* ---- WRITE_DATA ---- */
/* Packet: header, engine/dst_sel, addr_lo, addr_hi, data[0..n-1] (4+n DWORDs) */
int pm4_write_data(pm4_ring_t *r, uint64_t dst_addr,
                   const uint32_t *data, uint32_t count)
{
    if (count == 0)
        return 0;
    /* DST_SEL=5 (memory mapped register space) or 2 (GPU memory) */
    uint32_t wr_ctrl = (2u << 8) /* DST_SEL=2: TC_L2 (GPU memory) */
                     | (1u << 28); /* WR_CONFIRM=1 */
    if (pm4_hdr(r, PM4_IT_WRITE_DATA, 3 + count) != 0)
        return -1;
    if (pm4_write(r, wr_ctrl) != 0)                           return -1;
    if (pm4_write(r, (uint32_t)(dst_addr)) != 0)              return -1;
    if (pm4_write(r, (uint32_t)(dst_addr >> 32)) != 0)        return -1;
    for (uint32_t i = 0; i < count; i++)
        if (pm4_write(r, data[i]) != 0)
            return -1;
    return 0;
}

/* ---- NOP ---- */
int pm4_nop(pm4_ring_t *r, uint32_t count)
{
    if (count == 0)
        return 0;
    if (pm4_hdr(r, PM4_IT_NOP, count) != 0)
        return -1;
    for (uint32_t i = 1; i < count; i++)
        if (pm4_write(r, 0u) != 0)
            return -1;
    return 0;
}
