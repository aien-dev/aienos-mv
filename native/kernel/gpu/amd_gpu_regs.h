/* native/kernel/gpu/amd_gpu_regs.h -- AMD GCN register indices for Polaris 11
 * (Radeon Pro 555/560, PCI 1002:67ef, DCE 11.2 / GMC 8.x / GFX 8.0 / OSS 3.0 / SMU 7.1.3).
 *
 * MMIO byte offset = index * 4.  All indices refer to BAR5 (256 KiB register aperture).
 * Indices sourced from MIT-licensed AMD Linux kernel headers; see THIRD_PARTY_NOTICES.
 *
 * Sources (all MIT, Copyright (C) 2014-2016 Advanced Micro Devices, Inc.):
 *   include/asic_reg/dce/dce_11_2_d.h      -- display engine
 *   include/asic_reg/gmc/gmc_8_1_d.h       -- memory controller
 *   include/asic_reg/gca/gfx_8_0_d.h       -- GFX (CP, RLC)
 *   include/asic_reg/oss/oss_3_0_d.h       -- SDMA
 *   include/asic_reg/smu/smu_7_1_3_d.h    -- SMU
 * Repository: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
 *             drivers/gpu/drm/amd/include/asic_reg/ */
#pragma once
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Display engine -- DCE 11.2 (dce_11_2_d.h)
 * CRTC 0 registers; CRTC N adds crtc_offset (0x300 per controller).
 * Byte offset within BAR5 = index * 4.
 * ------------------------------------------------------------------------ */

/* Panel power sequencer (used by panel_x86.c; repeated here for reference) */
#define mmLVTMA_PWRSEQ_CNTL             0x481bu   /* dce_11_2_d.h line ~1200 */

/* GRPH (display pipe) surface address -- for scan-out page flip */
#define mmGRPH_PRIMARY_SURFACE_ADDRESS       0x5045u  /* dce_11_2_d.h line ~1320 */
#define mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH  0x5046u  /* dce_11_2_d.h line ~1321 */
#define mmGRPH_FLIP_CONTROL                  0x5047u  /* dce_11_2_d.h line ~1322 */
#define mmGRPH_UPDATE                        0x5048u  /* dce_11_2_d.h line ~1323 */

/* GRPH_UPDATE bits (dce_11_2_sh_mask.h) */
#define GRPH_UPDATE_LOCK                     0x00000001u  /* GRPH_UPDATE__GRPH_SURFACE_UPDATE_LOCK_MASK */
#define GRPH_SURFACE_UPDATE_PENDING          0x00000002u  /* GRPH_UPDATE__GRPH_SURFACE_UPDATE_PENDING_MASK */

/* GRPH_FLIP_CONTROL bits */
#define GRPH_FLIP_AT_VSYNC                   0x00000001u  /* 1 = flip at next vertical retrace */

/* CRTC status -- for vblank detection */
#define mmCRTC_STATUS                        0x604eu  /* dce_11_2_d.h line ~1560 */
#define CRTC_V_BLANK                         0x00000001u  /* CRTC_STATUS__CRTC_V_BLANK_MASK bit 0 */

/* CRTC master update lock (CRTC 0) */
#define mmCRTC_MASTER_UPDATE_LOCK            0x61d1u  /* dce_11_2_d.h line ~1600 */
#define CRTC_MASTER_UPDATE_LOCK_BIT          0x00000001u

/* -------------------------------------------------------------------------
 * Memory Controller -- GMC 8.1 (gmc_8_1_d.h)
 * ------------------------------------------------------------------------ */
#define mmMC_VM_FB_LOCATION                  0x0009u  /* gmc_8_1_d.h line ~20 */
#define mmMC_VM_FB_OFFSET                    0x000au  /* gmc_8_1_d.h line ~21 */
#define mmMC_VM_AGP_TOP                      0x000bu
#define mmMC_VM_AGP_BOT                      0x000cu
#define mmMC_VM_SYSTEM_APERTURE_LOW_ADDR     0x000eu  /* gmc_8_1_d.h line ~25 */
#define mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR    0x000fu  /* gmc_8_1_d.h line ~26 */
#define mmMC_VM_MX_L1_TLB_CNTL              0x0028u  /* gmc_8_1_d.h line ~45 */

/* CONFIG_MEMSIZE: VRAM size in MiB (upper 16 bits) */
#define mmCONFIG_MEMSIZE                     0x0150u  /* mc indirect -- read via HDP_MEM_COHERENCY or direct */

/* HDP for coherency flush */
#define mmHDP_MEM_COHERENCY_FLUSH_CNTL       0x0122u  /* gmc_8_1_d.h */
#define mmHDP_REG_COHERENCY_FLUSH_CNTL       0x0128u  /* gmc_8_1_d.h */

/* VM context 0 page table base address */
#define mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR   0x0153u  /* gmc_8_1_d.h line ~130 */
#define mmVM_CONTEXT0_CNTL                   0x0144u  /* gmc_8_1_d.h line ~100 */
#define mmVM_L2_CNTL                         0x0137u
#define mmVM_L2_CNTL2                        0x0138u

/* -------------------------------------------------------------------------
 * GFX 8 / CP (gfx_8_0_d.h)
 * ------------------------------------------------------------------------ */
/* GRBM -- Graphics Register Bus Manager */
#define mmGRBM_SOFT_RESET                    0x0817u  /* gfx_8_0_d.h */
#define GRBM_SOFT_RESET_CP                   0x00000001u  /* GRBM_SOFT_RESET__SOFT_RESET_CP_MASK */
#define GRBM_SOFT_RESET_GFX                  0x00010000u  /* GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK */
#define GRBM_SOFT_RESET_RLC                  0x00000004u  /* GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK */

#define mmGRBM_STATUS                        0x0814u  /* gfx_8_0_d.h */
#define GRBM_STATUS_GUI_ACTIVE               (1u << 31) /* GUI_ACTIVE bit */

/* CP PFP microcode upload */
#define mmCP_PFP_UCODE_ADDR                  0x2150u  /* gfx_8_0_d.h */
#define mmCP_PFP_UCODE_DATA                  0x2151u

/* CP ME microcode upload */
#define mmCP_ME_RAM_WADDR                    0x2157u
#define mmCP_ME_RAM_RADDR                    0x2158u
#define mmCP_ME_RAM_DATA                     0x2159u

/* CP CE microcode upload */
#define mmCP_CE_UCODE_ADDR                   0x2180u
#define mmCP_CE_UCODE_DATA                   0x2181u

/* CP MEC (Micro Engine Compute) microcode upload */
#define mmCP_MEC_ME1_UCODE_ADDR              0x21c5u
#define mmCP_MEC_ME1_UCODE_DATA              0x21c6u
#define mmCP_MEC_ME2_UCODE_ADDR              0x21c7u
#define mmCP_MEC_ME2_UCODE_DATA              0x21c8u

/* GFX ring 0 control */
#define mmCP_RB0_BASE                        0x20c5u  /* gfx_8_0_d.h */
#define mmCP_RB0_BASE_HI                     0x20c6u
#define mmCP_RB0_CNTL                        0x20c1u
#define mmCP_RB0_WPTR                        0x20c3u
#define mmCP_RB0_RPTR_ADDR                   0x20bfu
#define mmCP_RB0_RPTR_ADDR_HI                0x20c0u
#define mmCP_RB_VMID                         0x20ccu

/* CP status */
#define mmCP_STAT                            0x00e4u
#define CP_STAT_PFP_BUSY                     0x00000001u

/* CP_RB0_CNTL bits */
#define CP_RB_BLKSZ(n)           ((n) & 0x3fu)  /* log2(ring_entries) - 1, shifted 0 */
#define CP_RB_NO_UPDATE          (1u << 27)      /* CP writes RPTR to address on retire */

/* RLC (RunList Controller) */
#define mmRLC_CNTL                           0x30d0u  /* gfx_8_0_d.h */
#define RLC_ENABLE                           0x00000001u
#define mmRLC_SRM_CNTL                       0x30d4u
#define mmRLC_CGTT_MGCG_OVERRIDE             0x3130u
#define mmRLC_SRM_INDEX_CNTL_ADDR_0          0x3158u
#define mmRLC_SRM_INDEX_CNTL_DATA_0          0x315cu
#define mmRLC_GPM_UCODE_ADDR                 0x3100u  /* gfx_8_0_d.h */
#define mmRLC_GPM_UCODE_DATA                 0x3101u

/* -------------------------------------------------------------------------
 * SDMA (oss_3_0_d.h)
 * SDMA0 registers; SDMA1 adds an offset of 0x200.
 * ------------------------------------------------------------------------ */
#define mmSDMA0_GFX_RB_BASE                  0x3490u  /* oss_3_0_d.h */
#define mmSDMA0_GFX_RB_BASE_HI               0x3491u
#define mmSDMA0_GFX_RB_RPTR                  0x3492u
#define mmSDMA0_GFX_RB_WPTR                  0x3493u
#define mmSDMA0_GFX_RB_RPTR_ADDR_LO          0x3494u
#define mmSDMA0_GFX_RB_RPTR_ADDR_HI          0x3495u
#define mmSDMA0_GFX_RB_CNTL                  0x3496u
#define mmSDMA0_GFX_IB_CNTL                  0x34a2u
#define mmSDMA0_CNTL                         0x3427u
#define mmSDMA0_F32_CNTL                     0x3421u
#define mmSDMA0_STATUS_REG                   0x3428u
#define SDMA0_STATUS_IDLE                    0x00000001u  /* STATUS_REG__IDLE_MASK */

/* SDMA0_GFX_RB_CNTL bits */
#define SDMA_RB_ENABLE                       0x00000001u
#define SDMA_RB_SIZE(n)                      (((n) & 0x1fu) << 1)  /* log2(ring_dwords) */

/* -------------------------------------------------------------------------
 * SMU 7.1.3 (smu_7_1_3_d.h) -- checking firmware state
 * ------------------------------------------------------------------------ */
#define mmSMC_MSG                            0x0178u  /* smu_7_1_3_d.h */
#define mmSMC_MSG_ARG_0                      0x0179u
#define mmSMC_MSG_RESP_0                     0x0180u
#define mmSMC_IND_INDEX_0                    0x016au
#define mmSMC_IND_DATA_0                     0x016bu

/* SMC message IDs (polaris10_smumgr.c) */
#define PPSMC_MSG_GetSMFWVersion             0x0201u

/* SMC firmware header location in VRAM (polaris10_smumgr.c:SMU7_FIRMWARE_HEADER_LOCATION) */
#define SMU7_FIRMWARE_HEADER_LOCATION        0x20000u
#define SMU7_FIRMWARE_HEADER_smc_firmware_version_offset  0x4u  /* uint32_t at +4 */

/* RSMU interrupt / power state */
#define mmSMU_IND_INDEX_1                    0x016cu
#define mmSMU_IND_DATA_1                     0x016du

/* -------------------------------------------------------------------------
 * Compute HQD / MQD (gfx_8_0_d.h)
 * ------------------------------------------------------------------------ */
#define mmCP_HQD_ACTIVE                      0x2120u  /* gfx_8_0_d.h */
#define mmCP_HQD_PQ_BASE                     0x2121u
#define mmCP_HQD_PQ_BASE_HI                  0x2122u
#define mmCP_HQD_PQ_RPTR                     0x2123u
#define mmCP_HQD_PQ_RPTR_REPORT_ADDR         0x2124u
#define mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI      0x2125u
#define mmCP_HQD_PQ_WPTR_POLL_ADDR           0x2126u
#define mmCP_HQD_PQ_WPTR_POLL_ADDR_HI        0x2127u
#define mmCP_HQD_PQ_CNTL                     0x2128u
#define mmCP_HQD_PQ_WPTR                     0x2129u
#define mmCP_HQD_PIPE_PRIORITY               0x2130u
#define mmCP_HQD_QUEUE_PRIORITY              0x2131u
#define mmCP_HQD_QUANTUM                     0x2132u
#define mmCP_HQD_IB_BASE_ADDR                0x2133u
#define mmCP_HQD_IB_BASE_ADDR_HI             0x2134u
#define mmCP_HQD_IB_CONTROL                  0x2135u
#define mmCP_HQD_DEQUEUE_REQUEST             0x2136u
#define mmCP_HQD_EOP_BASE_ADDR               0x2139u
#define mmCP_HQD_EOP_BASE_ADDR_HI            0x213au
#define mmCP_HQD_EOP_CONTROL                 0x213bu

/* GRBM_GFX_INDEX -- select pipe/queue for HQD programming */
#define mmGRBM_GFX_INDEX                     0x0803u
#define GRBM_GFX_INDEX_INSTANCE_INDEX(n)     ((n) & 0xffu)
#define GRBM_GFX_INDEX_SH_INDEX(n)           (((n) & 0xffu) << 8u)
#define GRBM_GFX_INDEX_SE_INDEX(n)           (((n) & 0xffu) << 16u)
#define GRBM_GFX_INDEX_SH_BROADCAST         (1u << 29u)
#define GRBM_GFX_INDEX_SE_BROADCAST         (1u << 31u)

/* -------------------------------------------------------------------------
 * PM4 packet headers (gfx_v8_0.c; GCN3 ISA guide section 3)
 * ------------------------------------------------------------------------ */
/* IT_NOP (opcode 0x10): does nothing; used to pad ring to alignment */
#define PM4_NOP3(n)         (0xc0001000u | ((n) & 0x3fffu))  /* 3+n dwords: header + n dwords */
#define PM4_NOP_HDR         0xffff1000u  /* traditional NOP (all-ones count) */

/* IT_WRITE_DATA (opcode 0x37): write value to memory or register */
#define PM4_WRITE_DATA_HDR  0xc0023700u  /* 4-dword packet: hdr, engine/addr, addr_lo, addr_hi, data */
#define WD_ENGINE_MEM       (2u << 8u)   /* write to memory */
#define WD_WR_CONFIRM       (1u << 20u)  /* wait for write to land */
#define WD_DST_MEM          (0u)
#define PM4_WRITE_DATA_COUNT 4u          /* dwords after header: control, addr_lo, addr_hi, data */

/* IT_FENCE_REG (opcode 0x3a alias): in gfx8 use WRITE_DATA to memory for fence */
/* Fence PM4: header=WRITE_DATA(1 dw) | ADDR_LO | ADDR_HI | SEQ */
