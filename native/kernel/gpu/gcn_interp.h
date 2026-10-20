/* native/kernel/gpu/gcn_interp.h -- Host-side GCN interpreter for testing kernels.
 *
 * Simulates one wavefront (64 lanes) executing GCN gfx803 instructions.
 * Supported: scalar ALU (SOP1/SOP2/SOPK/SOPC/SOPP), vector ALU (VOP1/VOP2/VOPC),
 * buffer loads/stores (MUBUF), local data share (DS), flat memory (FLAT).
 * Used to check kernels pixel-for-pixel against reference C without hardware.
 *
 * No floats in the interpreter infrastructure (lane data uses uint32_t;
 * float arithmetic is done by the GPU instructions themselves via float bit
 * manipulation that stays within uint32_t).
 */
#ifndef GCN_INTERP_H
#define GCN_INTERP_H

#include <stddef.h>
#include <stdint.h>

/* ---- Wavefront state ---- */
#define GCN_WAVE_LANES  64   /* always 64 for gfx803 */
#define GCN_NUM_SGPR    104  /* user SGPRs, s0-s103 */
#define GCN_NUM_VGPR    256  /* VGPRs per lane */

typedef struct {
    /* Scalar registers */
    uint32_t sgpr[GCN_NUM_SGPR]; /* s0-s103 */
    uint32_t vcc_lo, vcc_hi;     /* VCC (64-bit, lo=lanes 0-31) */
    uint32_t exec_lo, exec_hi;   /* EXEC mask */
    uint32_t scc;                /* scalar condition code (1 bit) */
    uint32_t m0;                 /* M0 register */
    uint32_t pc;                 /* program counter (DWORD index) */

    /* Vector registers: vgpr[lane][reg] */
    uint32_t vgpr[GCN_WAVE_LANES][GCN_NUM_VGPR];

    /* Memory: host-side byte-addressable buffer (simulates VRAM/system memory) */
    uint8_t  *mem;
    uint64_t  mem_base; /* GPU address of mem[0] */
    size_t    mem_size; /* bytes */

    /* Local data share (LDS): up to 64 KB per workgroup */
    uint8_t   lds[65536];

    /* Instruction count (for profiling) */
    uint64_t  insn_count;

    /* Step limit: stop after this many instructions (0 = no limit) */
    uint64_t  step_limit;

    /* Error string set on illegal instruction or illegal memory access */
    const char *error;

    /* Thread group IDs (set before launch) */
    uint32_t tgid_x, tgid_y, tgid_z;

    /* Thread IDs within group (set before launch, per-lane) */
    uint32_t tid_x[GCN_WAVE_LANES];
    uint32_t tid_y[GCN_WAVE_LANES];
    uint32_t tid_z[GCN_WAVE_LANES];
} gcn_wave_t;

/* ---- Interpreter API ---- */

/* Initialize a wavefront: clear all registers, set EXEC=all-ones, PC=0. */
void gcn_wave_init(gcn_wave_t *w);

/* Attach host memory: mem[0..size-1] is GPU-visible at base_gpu_addr. */
void gcn_wave_attach_mem(gcn_wave_t *w, void *mem, size_t size,
                         uint64_t base_gpu_addr);

/* Execute one instruction at w->pc; return 0 on success, -1 on error/endpgm. */
int gcn_wave_step(gcn_wave_t *w, const uint32_t *code, size_t code_dwords);

/* Execute until s_endpgm or step_limit; return instruction count. */
uint64_t gcn_wave_run(gcn_wave_t *w, const uint32_t *code, size_t code_dwords);

/* Set up thread IDs for a flat 1D workgroup of 'nthreads' threads
 * (nthreads <= 64), thread 0..nthreads-1 active, rest masked off. */
void gcn_wave_set_threads_1d(gcn_wave_t *w, uint32_t nthreads,
                              uint32_t tgid_x, uint32_t base_tid_x);

/* Set up thread IDs for a 2D workgroup (tx*ty <= 64 threads). */
void gcn_wave_set_threads_2d(gcn_wave_t *w,
                              uint32_t tx, uint32_t ty,
                              uint32_t tgid_x, uint32_t tgid_y,
                              uint32_t base_x, uint32_t base_y);

/* ---- SRC0 helper: read value from 9-bit src field ---- */
/* 0-127=SGPR, 128=inline 0, 129-192=inline 1-64, 193-208=inline -1 to -16,
 * 240-247=float constants, 255=literal (next dword), 256-511=VGPR lane N. */
uint32_t gcn_src_read(const gcn_wave_t *w, uint32_t src, uint32_t lane,
                      const uint32_t *literal_ptr);

#endif /* GCN_INTERP_H */
