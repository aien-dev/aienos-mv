/* native/kernel/gpu/pm4.h -- PM4 type-3 compute command packets for AMD GFX8.
 *
 * Register offsets from Linux amdgpu (MIT license):
 *   drivers/gpu/drm/amd/include/asic_reg/gca/gfx_8_0_d.h
 *   drivers/gpu/drm/amd/include/asic_reg/gca/gfx_8_0_sh_mask.h
 * Cited and attributed per THIRD_PARTY_NOTICES.
 *
 * Ring writer callback: until the sibling core driver
 *   (native/kernel/gpu/amd_ring.h in branch feat/imac-gpu-core) lands, we
 *   define a minimal pm4_ring_t with a write callback so callers can build
 *   packets into any buffer.
 *
 * No floats in kernel C; no third-party code in the build.
 * Oracle: Linux amdgpu driver and AMD PPT documentation.
 */
#ifndef PM4_H
#define PM4_H

#include <stddef.h>
#include <stdint.h>

/* ---- Register offsets from Linux gfx_8_0_d.h (cited, MIT) ---- */
/* SH (shader) registers: base 0x2C00, accessed via SET_SH_REG */
#define GCN_SH_REG_BASE             0x2C00u

/* Compute pipeline registers (SET_SH_REG addressing = reg - SH_REG_BASE) */
#define mmCOMPUTE_PGM_LO            0x2E0Cu  /* low 32 bits of shader start addr >> 8 */
#define mmCOMPUTE_PGM_HI            0x2E0Du  /* high 32 bits of shader start addr >> 8 */
#define mmCOMPUTE_PGM_RSRC1         0x2E12u  /* PGM resource 1 */
#define mmCOMPUTE_PGM_RSRC2         0x2E13u  /* PGM resource 2 (LDS, waves, etc.) */
#define mmCOMPUTE_USER_DATA_0       0x2E40u  /* user data SGPRs 0-15 at +0..+15 */
#define mmCOMPUTE_TMPRING_SIZE      0x2E18u
#define mmCOMPUTE_RESTART_X         0x2E14u
#define mmCOMPUTE_RESTART_Y         0x2E15u
#define mmCOMPUTE_RESTART_Z         0x2E16u
#define mmCOMPUTE_THREAD_TRACE_ENABLE 0x2E1Bu
#define mmCOMPUTE_NUM_THREAD_X      0x2E07u  /* threads per group, X */
#define mmCOMPUTE_NUM_THREAD_Y      0x2E08u  /* threads per group, Y */
#define mmCOMPUTE_NUM_THREAD_Z      0x2E09u  /* threads per group, Z */
#define mmCOMPUTE_START_X           0x2E04u
#define mmCOMPUTE_START_Y           0x2E05u
#define mmCOMPUTE_START_Z           0x2E06u

/* ---- PM4 packet header macros ---- */
/* Type-3 header (from Linux amdgpu PACKET3 macro):
 * [31:30]=11  [29:16]=count-1  [15:8]=op  [7:0]=0x3 */
#define PM4_TYPE3_HDR(op, count) \
    ((3u << 30) | (((uint32_t)((count)-1) & 0x3fffu) << 16) | ((uint32_t)(op) << 8) | 3u)

/* PM4 IT_OPCODE values for compute (from Linux amdgpu oss/amdgpu_ring.h) */
#define PM4_IT_SET_SH_REG           0x76u
#define PM4_IT_DISPATCH_DIRECT      0x15u
#define PM4_IT_ACQUIRE_MEM          0x58u
#define PM4_IT_RELEASE_MEM          0x49u
#define PM4_IT_WRITE_DATA           0x37u
#define PM4_IT_WAIT_REG_MEM         0x3Cu
#define PM4_IT_EVENT_WRITE          0x46u
#define PM4_IT_EVENT_WRITE_EOP      0x47u
#define PM4_IT_NOP                  0x10u

/* ---- Ring writer abstraction ---- */
/* Replace with amd_ring.h callback when feat/imac-gpu-core lands. */
typedef struct {
    uint32_t *buf;      /* ring buffer (caller-managed) */
    uint32_t  size;     /* capacity in DWORDs */
    uint32_t  wptr;     /* write pointer (DWORD index) */
} pm4_ring_t;

/* Write one DWORD to the ring; returns 0 on success, -1 on overflow. */
static inline int pm4_write(pm4_ring_t *r, uint32_t dw)
{
    if (r->wptr >= r->size)
        return -1;
    r->buf[r->wptr++] = dw;
    return 0;
}

/* ---- PM4 packet builders ---- */
/* All return 0 on success, -1 on ring overflow.
 * count is the number of DWORDs actually written (header + payload). */

/* SET_SH_REG: write N contiguous SH registers starting at mm_reg.
 * mm_reg is the mmXXX constant (absolute address).
 * data[] has N entries. */
int pm4_set_sh_reg(pm4_ring_t *r, uint32_t mm_reg,
                   const uint32_t *data, uint32_t count);

/* Convenience: set one SH register */
int pm4_set_sh_reg1(pm4_ring_t *r, uint32_t mm_reg, uint32_t val);

/* DISPATCH_DIRECT: launch a compute shader.
 * dim_{x,y,z}: number of thread groups in each dimension.
 * dispatch_initiator: typically 0x00000001 (COMPUTE_SHADER_EN). */
int pm4_dispatch_direct(pm4_ring_t *r,
                        uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                        uint32_t dispatch_initiator);

/* ACQUIRE_MEM: cache flush / memory barrier.
 * coher_cntl: coherency control bits (see ACQUIRE_MEM_COHER_CNTL in vi_structs.h).
 * Typical full-barrier: coher_cntl=0x00600000 (SH+TC invalidate). */
int pm4_acquire_mem(pm4_ring_t *r, uint32_t coher_cntl);

/* RELEASE_MEM / EVENT_WRITE_EOP: write a fence value to memory after a GPU event.
 * event_type: e.g. BOTTOM_OF_PIPE_TS=0x14, CS_DONE=0x07.
 * addr: 64-bit GPU-visible address of the fence location.
 * value: 64-bit value to write. */
int pm4_release_mem_fence(pm4_ring_t *r,
                          uint32_t event_type,
                          uint64_t addr,
                          uint64_t value);

/* WRITE_DATA: write N DWORDs to a GPU address at dispatch time.
 * dst_addr: 64-bit destination address.
 * data: DWORDs to write.
 * count: number of DWORDs. */
int pm4_write_data(pm4_ring_t *r, uint64_t dst_addr,
                   const uint32_t *data, uint32_t count);

/* NOP padding: write count DWORDs of NOP (for alignment or debug). */
int pm4_nop(pm4_ring_t *r, uint32_t count);

/* ---- Helper: compute DISPATCH_INITIATOR ---- */
/* Standard compute shader enable */
#define PM4_DISPATCH_INITIATOR_CS_EN   0x00000001u
/* Force start (no waiting for previous shader end) */
#define PM4_DISPATCH_INITIATOR_FORCE   0x00000002u

/* ---- COMPUTE_PGM_RSRC1 field encoding ---- */
/* SGPRS in units of 8, minus 1. VGPRS in units of 4, minus 1. */
static inline uint32_t pm4_pgm_rsrc1(uint32_t sgprs, uint32_t vgprs)
{
    /* SGPRs: bits[3:0] = (ceil(sgprs/8)-1), max 15 (=104 SGPRs) */
    /* VGPRs: bits[9:6] = (ceil(vgprs/4)-1), max 63 (=256 VGPRs) */
    uint32_t sgpr_field = (sgprs + 7) / 8 - 1;
    uint32_t vgpr_field = (vgprs + 3) / 4 - 1;
    return (vgpr_field << 6) | sgpr_field;
}

/* COMPUTE_PGM_RSRC2 field encoding */
/* scratch_en=0, user_sgprs=N (number of user data SGPRs), tgid_x/y/z,
 * excp_en=0, lds_size in 512-byte units */
static inline uint32_t pm4_pgm_rsrc2(uint32_t user_sgprs,
                                      int tgid_x, int tgid_y, int tgid_z,
                                      uint32_t lds_size_dw)
{
    /* bits[1:0]=scratch_en=0 & user_sgprs bits[5:1] in register */
    /* bits[10:6]=user_sgpr_count (0-16) */
    /* bit 11 = tgid_x_en, bit 12 = tgid_y_en, bit 13 = tgid_z_en */
    /* bits[24:15] = lds_size (units of 512 bytes = 128 DWORDs) */
    uint32_t lds_units = (lds_size_dw + 127) / 128;  /* round up to 512-byte units */
    return ((user_sgprs & 0x3fu) << 1)
         | ((tgid_x ? 1u : 0u) << 11)
         | ((tgid_y ? 1u : 0u) << 12)
         | ((tgid_z ? 1u : 0u) << 13)
         | ((lds_units & 0x1ffu) << 15);
}

#endif /* PM4_H */
