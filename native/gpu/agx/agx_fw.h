/*
 * Apple GPU (AGX) firmware interface constants for a first compute job:
 * G14 (M2) with GPU firmware 13.5. Shared by the AienOS driver side
 * (agx_job.c) and the simulated device (agx_sim.c), so both read and write
 * the same bytes at the same offsets.
 *
 * Every offset and value here is INFERRED from drm/asahi (GPL-2.0-only OR MIT,
 * used under MIT) at 94fb23346d52, through the generated layout tables in
 * docs/AGX_FW_LAYOUT_G14_V13_5.md and docs/AGX_FIRMWARE_INTERFACE.md
 * sections 5, 8 and 10. Command-stream and USC word encodings are INFERRED
 * from Mesa src/asahi/genxml/cmdbuf.xml (MIT) at ba3dacb9. The trace in
 * docs/MACBOOK_AGX_TRACE_PROCEDURE.md is the experiment that confirms them on
 * this machine (rows U2 to U4).
 */
#ifndef AIENOS_AGX_FW_H
#define AIENOS_AGX_FW_H

#include <stdint.h>

/* GPU MMU (UAT): drm/asahi pgtable.rs, mmu.rs. */
#define AGX_UAT_PGBIT 14
#define AGX_UAT_PGSZ (1ull << AGX_UAT_PGBIT)
#define AGX_UAT_LVBIT 11 /* 2048 eight-byte entries per 16 KiB table */
#define AGX_UAT_LEVELS 3
#define AGX_UAT_IAS 39
#define AGX_UAT_NUM_CTX 64
#define AGX_UAT_TTBR1_BASE 0xffffff8000000000ull /* VAs at or above use ttb1 */
#define AGX_UAT_USER_BASE AGX_UAT_PGSZ
#define AGX_UAT_USER_TOP (1ull << AGX_UAT_IAS)
#define AGX_UAT_KERN_BASE 0xffffffa000000000ull
#define AGX_UAT_KERN_TOP 0xffffffb000000000ull

#define AGX_TTBR_VALID 1ull
#define AGX_TTBR_ASID_SHIFT 48
#define AGX_TTBR_ADDR_MASK 0x0000ffffffffc000ull /* below the ASID; PTE addresses reach bit 51 */
#define AGX_TTBAT_SLOT_BYTES 16 /* SlotTTBS { ttb0 u64, ttb1 u64 } */

#define AGX_PTE_TYPE_MASK 3ull
#define AGX_PTE_TYPE_TABLE_LEAF 3ull /* tables and leaf pages both use 3 */
#define AGX_PTE_MEMATTR_SHIFT 2
#define AGX_PTE_AP_SHIFT 6
#define AGX_PTE_AF (1ull << 10)
#define AGX_PTE_NON_GLOBAL (1ull << 11)
#define AGX_PTE_HIGH_SHIFT 52
#define AGX_PTE_HIGH_PXN 0x2u
#define AGX_PTE_HIGH_UXN 0x4u
#define AGX_PTE_HIGH_GPU_ACCESS 0x8u
#define AGX_PTE_ADDR_MASK 0x000fffffffffc000ull

/* AP values (drm/asahi AP_FW_GPU, AP_FW, AP_GPU) and memory attributes. */
#define AGX_AP_FW_GPU 0
#define AGX_AP_FW 1
#define AGX_AP_GPU 2
#define AGX_MEMATTR_CACHED 0
#define AGX_MEMATTR_DEV 1
#define AGX_MEMATTR_UNCACHED 2

/* Work queue command (fw/workqueue.rs CommandType). */
#define AGX_CMD_RUN_COMPUTE 3

/* compute::RunCompute, 0x31c bytes. */
#define AGX_RC_SIZE 0x31c
#define AGX_RC_TAG 0x000
#define AGX_RC_COUNTER 0x004
#define AGX_RC_VM_SLOT 0x010
#define AGX_RC_NOTIFIER 0x014
#define AGX_RC_JOB_PARAMS1 0x070
#define AGX_RC_MICROSEQ 0x1f0
#define AGX_RC_MICROSEQ_SIZE 0x1f8
#define AGX_RC_JOB_PARAMS2 0x1fc
#define AGX_RC_ENCODER_PARAMS 0x25c
#define AGX_RC_META 0x284
#define AGX_RC_CLIENT_SEQ 0x2d8

/* compute::JobParameters1 (0x160) and JobParameters2 (0x60), relative. */
#define AGX_JP1_CDM_BASE 0x008
#define AGX_JP1_USC_EXEC_BASE 0x030
#define AGX_JP1_UNK_38 0x038
#define AGX_JP1_UNK_58 0x058
#define AGX_JP1_UNK_38_VALUE 0x8c60u /* drm/asahi queue/compute.rs */
#define AGX_JP1_UNK_58_VALUE 1u
#define AGX_JP2_CDM_END 0x030

/* job::EncoderParams (0x28) and job::JobMeta (0x2c), relative. */
#define AGX_EP_ENCODER_ID 0x0c
#define AGX_META_STAMP 0x04
#define AGX_META_FW_STAMP 0x0c
#define AGX_META_STAMP_VALUE 0x14
#define AGX_META_STAMP_SLOT 0x18
#define AGX_META_EVENT_SEQ 0x28

/* Microsequence ops: header words and sizes (fw/microseq.rs). */
#define AGX_MS_START_COMPUTE 0x29u
#define AGX_MS_START_COMPUTE_SIZE 0x16c
#define AGX_MS_SC_JOB_PARAMS1 0x00c
#define AGX_MS_SC_WORK_QUEUE 0x01c
#define AGX_MS_SC_VM_SLOT 0x024
#define AGX_MS_SC_EVENT_SEQ 0x030
#define AGX_MS_SC_JOB_PARAMS2 0x03c
#define AGX_MS_TIMESTAMP 0x19u /* bit 31 set: start */
#define AGX_MS_TIMESTAMP_SIZE 0x3c
#define AGX_MS_WAIT_FOR_IDLE_COMPUTE (0x01u | (0x8000u << 8))
#define AGX_MS_WAIT_FOR_IDLE_SIZE 0x4
#define AGX_MS_FINALIZE_COMPUTE 0x2au
#define AGX_MS_FINALIZE_COMPUTE_SIZE 0x7c
#define AGX_MS_FC_WORK_QUEUE 0x00c
#define AGX_MS_FC_VM_SLOT 0x014
#define AGX_MS_FC_JOB_PARAMS2 0x018
#define AGX_MS_FC_FW_STAMP 0x028
#define AGX_MS_FC_STAMP_VALUE 0x030
#define AGX_MS_FC_RESTART_BRANCH 0x058
#define AGX_MS_RETIRE_STAMP (0x18u | 0x40000000u)
#define AGX_MS_RETIRE_STAMP_SIZE 0x4

/* workqueue::QueueInfo (0xb8) and RingState (0x70). */
#define AGX_QI_SIZE 0xb8
#define AGX_QI_STATE 0x000
#define AGX_QI_RING 0x008
#define AGX_QI_GPU_RPTR1 0x020
#define AGX_QI_ERROR_COUNT 0x08c
#define AGX_RS_SIZE 0x70
#define AGX_RS_GPU_DONEPTR 0x000
#define AGX_RS_GPU_RPTR 0x030
#define AGX_RS_CPU_WPTR 0x040
#define AGX_RS_RB_SIZE 0x050

/* channels::RunWorkQueueMsg: 0x40 bytes on G14 with firmware >= 13.2. */
#define AGX_RWQ_SIZE 0x40
#define AGX_RWQ_PIPE_TYPE 0x00
#define AGX_RWQ_WORK_QUEUE 0x04
#define AGX_RWQ_WPTR 0x0c
#define AGX_RWQ_EVENT_SLOT 0x10
#define AGX_RWQ_IS_NEW 0x14
#define AGX_PIPE_COMPUTE 2u

/* CDM control stream blocks (bits 29..31 of the first word). */
#define AGX_CDM_LAUNCH 0u
#define AGX_CDM_STREAM_LINK 1u
#define AGX_CDM_STREAM_TERMINATE 2u
#define AGX_CDM_BARRIER 3u
#define AGX_CDM_STREAM_RETURN 4u

/* USC control word tags (low byte). */
#define AGX_USC_UNIFORM 0x1du
#define AGX_USC_UNIFORM_HIGH 0x3du
#define AGX_USC_SHARED 0x4du
#define AGX_USC_SHADER 0x0du
#define AGX_USC_REGISTERS 0x8du
#define AGX_USC_SAMPLER 0x9du
#define AGX_USC_TEXTURE 0xddu
#define AGX_USC_NO_PRESHADER 0x88u
#define AGX_USC_PRESHADER 0x38u
#define AGX_USC_SHARED_LAYOUT_COMPUTE 0x24u

/* Little-endian accessors over plain byte buffers (freestanding). */
static inline uint32_t agx_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t agx_rd64(const uint8_t *p)
{
	return (uint64_t)agx_rd32(p) | (uint64_t)agx_rd32(p + 4) << 32;
}

static inline void agx_wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void agx_wr64(uint8_t *p, uint64_t v)
{
	agx_wr32(p, (uint32_t)v);
	agx_wr32(p + 4, (uint32_t)(v >> 32));
}

#endif
