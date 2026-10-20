/* native/kernel/gpu/gcn_enc.h -- GCN gen-3/4 (gfx803 Polaris 11) instruction encoder.
 *
 * Encodes scalar ALU (SOP1/SOP2/SOPK/SOPC/SOPP), scalar memory (SMEM),
 * vector ALU (VOP1/VOP2/VOP3/VOPC), local data share (DS), untyped buffer
 * memory (MUBUF/MTBUF), and flat memory (FLAT) instructions.
 *
 * API style mirrors omega/src/omega_blackwell_encoder.h: each emit function
 * takes (buf, pos, max, ...) and returns 0 on success, -1 on overflow.
 * Instructions are 32-bit (SOP/VOP) or 64-bit (SMEM/DS/MUBUF/FLAT/VOP3).
 *
 * Source: AMD "Graphics Core Next Architecture, Generation 3" ISA reference
 * (public PDF). Oracle: llvm-mc-18 -arch=amdgcn -mcpu=gfx803 -show-encoding
 * (installed under /usr/bin/llvm-mc-18 on Spark, never in the repo build).
 */
#ifndef GCN_ENC_H
#define GCN_ENC_H

#include <stddef.h>
#include <stdint.h>

/* ---- register operand encoding (src fields) ---- */
/* In SRC0 / SRC1 / SRC2 positions (9-bit fields):
 *   0-101  = SGPRs s0-s101
 *   102    = FLAT_SCRATCH_LO
 *   103    = FLAT_SCRATCH_HI
 *   104    = XNACK_MASK_LO
 *   105    = XNACK_MASK_HI
 *   106    = VCC_LO
 *   107    = VCC_HI
 *   108    = TBA_LO / TMA_LO / ...
 *   ...
 *   124    = M0
 *   126    = EXEC_LO
 *   127    = EXEC_HI
 *   128    = inline const 0
 *   129-192= inline const 1..64
 *   193-208= inline const -1..-16
 *   240    = 0.5
 *   241    = -0.5
 *   242    = 1.0
 *   243    = -1.0
 *   244    = 2.0
 *   245    = -2.0
 *   246    = 4.0
 *   247    = -4.0
 *   248    = 1/(2*PI)
 *   255    = literal constant (next DWORD)
 *   256-511= VGPRs v0-v255  */
#define GCN_SGPR(n)    ((uint32_t)(n))
#define GCN_VGPR(n)    ((uint32_t)(256u + (n)))
#define GCN_VCC_LO     106u
#define GCN_VCC_HI     107u
#define GCN_M0         124u
#define GCN_EXEC_LO    126u
#define GCN_EXEC_HI    127u
#define GCN_IMM(n)     ((uint32_t)(128u + (n)))   /* inline const 0..64 */
#define GCN_NIMM(n)    ((uint32_t)(192u + ((-((int)(n)))-1))) /* -1..-16 */
#define GCN_LITERAL    255u
#define GCN_F0_5       240u
#define GCN_FN0_5      241u
#define GCN_F1         242u
#define GCN_FN1        243u
#define GCN_F2         244u
#define GCN_FN2        245u
#define GCN_F4         246u
#define GCN_FN4        247u

/* SDST / SBASE special registers */
#define GCN_SDST_VCC_LO 106u
#define GCN_SDST_VCC    106u   /* writes VCC_LO for 32-bit SDST */

/* SCC implicit output for SOPC (comparison result) */

/* ---- SOP2: scalar ALU, 2 sources ---- */
/* ENCODING[31:30]=10  OP[29:23]  SDST[22:16]  SSRC1[15:8]  SSRC0[7:0] */
typedef enum {
    GCN_SOP2_S_ADD_U32       = 0,
    GCN_SOP2_S_SUB_U32       = 1,
    GCN_SOP2_S_ADD_I32       = 2,
    GCN_SOP2_S_SUB_I32       = 3,
    GCN_SOP2_S_ADDC_U32      = 4,
    GCN_SOP2_S_SUBB_U32      = 5,
    GCN_SOP2_S_MIN_I32       = 6,
    GCN_SOP2_S_MIN_U32       = 7,
    GCN_SOP2_S_MAX_I32       = 8,
    GCN_SOP2_S_MAX_U32       = 9,
    GCN_SOP2_S_CSELECT_B32   = 10,
    GCN_SOP2_S_CSELECT_B64   = 11,
    GCN_SOP2_S_AND_B32       = 12,
    GCN_SOP2_S_AND_B64       = 13,
    GCN_SOP2_S_OR_B32        = 14,
    GCN_SOP2_S_OR_B64        = 15,
    GCN_SOP2_S_XOR_B32       = 16,
    GCN_SOP2_S_XOR_B64       = 17,
    GCN_SOP2_S_ANDN2_B32     = 18,
    GCN_SOP2_S_ANDN2_B64     = 19,
    GCN_SOP2_S_ORN2_B32      = 20,
    GCN_SOP2_S_ORN2_B64      = 21,
    GCN_SOP2_S_NAND_B32      = 22,
    GCN_SOP2_S_NAND_B64      = 23,
    GCN_SOP2_S_NOR_B32       = 24,
    GCN_SOP2_S_NOR_B64       = 25,
    GCN_SOP2_S_XNOR_B32      = 26,
    GCN_SOP2_S_XNOR_B64      = 27,
    GCN_SOP2_S_LSHL_B32      = 28,
    GCN_SOP2_S_LSHL_B64      = 29,
    GCN_SOP2_S_LSHR_B32      = 30,
    GCN_SOP2_S_LSHR_B64      = 31,
    GCN_SOP2_S_ASHR_I32      = 32,
    GCN_SOP2_S_ASHR_I64      = 33,
    GCN_SOP2_S_BFM_B32       = 34,
    GCN_SOP2_S_BFM_B64       = 35,
    GCN_SOP2_S_MUL_I32       = 36,
    GCN_SOP2_S_BFE_U32       = 37,
    GCN_SOP2_S_BFE_I32       = 38,
    GCN_SOP2_S_BFE_U64       = 39,
    GCN_SOP2_S_BFE_I64       = 40,
    GCN_SOP2_S_ABSDIFF_I32   = 42,
} GcnSop2Op;

/* ---- SOP1: scalar ALU, 1 source ---- */
/* ENCODING[31:23]=101111101  SDST[22:16]  OP[15:8]  SSRC0[7:0] */
typedef enum {
    GCN_SOP1_S_MOV_B32        = 0,
    GCN_SOP1_S_MOV_B64        = 1,
    GCN_SOP1_S_CMOV_B32       = 2,
    GCN_SOP1_S_CMOV_B64       = 3,
    GCN_SOP1_S_NOT_B32        = 4,
    GCN_SOP1_S_NOT_B64        = 5,
    GCN_SOP1_S_WQM_B32        = 6,
    GCN_SOP1_S_WQM_B64        = 7,
    GCN_SOP1_S_BREV_B32       = 8,
    GCN_SOP1_S_BREV_B64       = 9,
    GCN_SOP1_S_BCNT0_I32_B32  = 10,
    GCN_SOP1_S_BCNT0_I32_B64  = 11,
    GCN_SOP1_S_BCNT1_I32_B32  = 12,
    GCN_SOP1_S_BCNT1_I32_B64  = 13,
    GCN_SOP1_S_FF0_I32_B32    = 14,
    GCN_SOP1_S_FF0_I32_B64    = 15,
    GCN_SOP1_S_FF1_I32_B32    = 16,
    GCN_SOP1_S_FF1_I32_B64    = 17,
    GCN_SOP1_S_FLBIT_I32_B32  = 18,
    GCN_SOP1_S_FLBIT_I32_B64  = 19,
    GCN_SOP1_S_FLBIT_I32      = 20,
    GCN_SOP1_S_SEXT_I32_I8    = 21,
    GCN_SOP1_S_SEXT_I32_I16   = 22,
    GCN_SOP1_S_BITSET0_B32    = 23,
    GCN_SOP1_S_BITSET0_B64    = 24,
    GCN_SOP1_S_BITSET1_B32    = 25,
    GCN_SOP1_S_BITSET1_B64    = 26,
    GCN_SOP1_S_GETPC_B64      = 27,
    GCN_SOP1_S_SETPC_B64      = 28,
    GCN_SOP1_S_SWAPPC_B64     = 29,
    GCN_SOP1_S_RFE_B64        = 30,
    GCN_SOP1_S_AND_SAVEEXEC_B64 = 32,
    GCN_SOP1_S_OR_SAVEEXEC_B64  = 33,
    GCN_SOP1_S_XOR_SAVEEXEC_B64 = 34,
    GCN_SOP1_S_ANDN2_SAVEEXEC_B64 = 35,
    GCN_SOP1_S_ORN2_SAVEEXEC_B64  = 36,
    GCN_SOP1_S_NAND_SAVEEXEC_B64  = 37,
    GCN_SOP1_S_NOR_SAVEEXEC_B64   = 38,
    GCN_SOP1_S_XNOR_SAVEEXEC_B64  = 39,
    GCN_SOP1_S_QUADMASK_B32   = 40,
    GCN_SOP1_S_QUADMASK_B64   = 41,
    GCN_SOP1_S_MOVRELS_B32    = 44,
    GCN_SOP1_S_MOVRELS_B64    = 45,
    GCN_SOP1_S_MOVRELD_B32    = 46,
    GCN_SOP1_S_MOVRELD_B64    = 47,
    GCN_SOP1_S_CBRANCH_JOIN   = 48,
    GCN_SOP1_S_ABS_I32        = 48,
    GCN_SOP1_S_MOV_REGRD_B32  = 53,
    GCN_SOP1_S_SET_GPR_IDX_IDX = 54,
} GcnSop1Op;

/* ---- SOPK: scalar with 16-bit immediate ---- */
/* ENCODING[31:28]=1011  OP[27:23]  SDST[22:16]  SIMM16[15:0] */
typedef enum {
    GCN_SOPK_S_MOVK_I32       = 0,
    GCN_SOPK_S_CMOVK_I32      = 1,
    GCN_SOPK_S_CMPK_EQ_I32    = 2,
    GCN_SOPK_S_CMPK_LG_I32    = 3,
    GCN_SOPK_S_CMPK_GT_I32    = 4,
    GCN_SOPK_S_CMPK_GE_I32    = 5,
    GCN_SOPK_S_CMPK_LT_I32    = 6,
    GCN_SOPK_S_CMPK_LE_I32    = 7,
    GCN_SOPK_S_CMPK_EQ_U32    = 8,
    GCN_SOPK_S_CMPK_LG_U32    = 9,
    GCN_SOPK_S_CMPK_GT_U32    = 10,
    GCN_SOPK_S_CMPK_GE_U32    = 11,
    GCN_SOPK_S_CMPK_LT_U32    = 12,
    GCN_SOPK_S_CMPK_LE_U32    = 13,
    GCN_SOPK_S_ADDK_I32       = 14,
    GCN_SOPK_S_MULK_I32       = 15,
    GCN_SOPK_S_GETREG_B32     = 17,
    GCN_SOPK_S_SETREG_B32     = 18,
    GCN_SOPK_S_SETREG_IMM32_B32 = 19,
} GcnSopkOp;

/* ---- SOPC: scalar comparison (result in SCC) ---- */
/* ENCODING[31:23]=101111110  OP[22:16]  SSRC1[15:8]  SSRC0[7:0] */
typedef enum {
    GCN_SOPC_S_CMP_EQ_I32   = 0,
    GCN_SOPC_S_CMP_LG_I32   = 1,
    GCN_SOPC_S_CMP_GT_I32   = 2,
    GCN_SOPC_S_CMP_GE_I32   = 3,
    GCN_SOPC_S_CMP_LT_I32   = 4,
    GCN_SOPC_S_CMP_LE_I32   = 5,
    GCN_SOPC_S_CMP_EQ_U32   = 6,
    GCN_SOPC_S_CMP_LG_U32   = 7,
    GCN_SOPC_S_CMP_GT_U32   = 8,
    GCN_SOPC_S_CMP_GE_U32   = 9,
    GCN_SOPC_S_CMP_LT_U32   = 10,
    GCN_SOPC_S_CMP_LE_U32   = 11,
    GCN_SOPC_S_BITCMP0_B32  = 12,
    GCN_SOPC_S_BITCMP1_B32  = 13,
    GCN_SOPC_S_BITCMP0_B64  = 14,
    GCN_SOPC_S_BITCMP1_B64  = 15,
} GcnSopcOp;

/* ---- SOPP: scalar with 16-bit SIMM / program control ---- */
/* ENCODING[31:23]=101111111  OP[22:16]  SIMM16[15:0] */
typedef enum {
    GCN_SOPP_S_NOP           = 0,
    GCN_SOPP_S_ENDPGM        = 1,
    GCN_SOPP_S_BRANCH        = 2,
    GCN_SOPP_S_CBRANCH_SCC0  = 4,
    GCN_SOPP_S_CBRANCH_SCC1  = 5,
    GCN_SOPP_S_CBRANCH_VCCZ  = 6,
    GCN_SOPP_S_CBRANCH_VCCNZ = 7,
    GCN_SOPP_S_CBRANCH_EXECZ = 8,
    GCN_SOPP_S_CBRANCH_EXECNZ= 9,
    GCN_SOPP_S_BARRIER       = 10,
    GCN_SOPP_S_WAITCNT       = 12,
    GCN_SOPP_S_SETHALT       = 13,
    GCN_SOPP_S_SLEEP         = 14,
    GCN_SOPP_S_SETPRIO       = 15,
    GCN_SOPP_S_SENDMSG       = 16,
    GCN_SOPP_S_SENDMSGHALT   = 17,
    GCN_SOPP_S_TRAP          = 18,
    GCN_SOPP_S_ICACHE_INV    = 19,
    GCN_SOPP_S_INCPERFLEVEL  = 20,
    GCN_SOPP_S_DECPERFLEVEL  = 21,
    GCN_SOPP_S_TTRACEDATA    = 22,
} GcnSoppOp;

/* ---- VOP1: vector ALU, 1 source ---- */
/* ENCODING[31:25]=0111111  VDST[24:17]  OP[16:9]  SRC0[8:0] */
typedef enum {
    GCN_VOP1_V_NOP                = 0,
    GCN_VOP1_V_MOV_B32            = 1,
    GCN_VOP1_V_READFIRSTLANE_B32  = 2,
    GCN_VOP1_V_CVT_I32_F64        = 3,
    GCN_VOP1_V_CVT_F64_I32        = 4,
    GCN_VOP1_V_CVT_F32_I32        = 5,
    GCN_VOP1_V_CVT_F32_U32        = 6,
    GCN_VOP1_V_CVT_U32_F32        = 7,
    GCN_VOP1_V_CVT_I32_F32        = 8,
    GCN_VOP1_V_CVT_F16_F32        = 10,
    GCN_VOP1_V_CVT_F32_F16        = 11,
    GCN_VOP1_V_CVT_F32_F64        = 15,
    GCN_VOP1_V_CVT_F64_F32        = 16,
    GCN_VOP1_V_FRACT_F32          = 27,
    GCN_VOP1_V_TRUNC_F32          = 28,
    GCN_VOP1_V_CEIL_F32           = 29,
    GCN_VOP1_V_RNDNE_F32          = 30,
    GCN_VOP1_V_FLOOR_F32          = 31,
    GCN_VOP1_V_EXP_F32            = 32,
    GCN_VOP1_V_LOG_F32            = 33,
    GCN_VOP1_V_RCP_F32            = 34,
    GCN_VOP1_V_RCP_IFLAG_F32      = 35,
    GCN_VOP1_V_RSQ_F32            = 36,
    GCN_VOP1_V_SQRT_F32           = 39,
    GCN_VOP1_V_SIN_F32            = 41,
    GCN_VOP1_V_COS_F32            = 42,
    GCN_VOP1_V_NOT_B32            = 43,
    GCN_VOP1_V_BFREV_B32          = 44,
    GCN_VOP1_V_FFBH_U32           = 45,
    GCN_VOP1_V_FFBL_B32           = 46,
    GCN_VOP1_V_FFBH_I32           = 47,
    GCN_VOP1_V_FREXP_EXP_I32_F32  = 51,
    GCN_VOP1_V_FREXP_MANT_F32     = 52,
} GcnVop1Op;

/* ---- VOP2: vector ALU, 2 sources ---- */
/* ENCODING[31]=0  OP[30:25]  VDST[24:17]  VSRC1[16:9]  SRC0[8:0] */
typedef enum {
    GCN_VOP2_V_CNDMASK_B32     = 0,
    GCN_VOP2_V_ADD_F32         = 1,
    GCN_VOP2_V_SUB_F32         = 2,
    GCN_VOP2_V_SUBREV_F32      = 3,
    GCN_VOP2_V_MUL_LEGACY_F32  = 4,
    GCN_VOP2_V_MUL_F32         = 5,   /* GFX8 */
    GCN_VOP2_V_MUL_I32_I24     = 6,
    GCN_VOP2_V_MUL_HI_I32_I24  = 7,
    GCN_VOP2_V_MUL_U32_U24     = 8,
    GCN_VOP2_V_MUL_HI_U32_U24  = 9,
    GCN_VOP2_V_MIN_F32         = 10,
    GCN_VOP2_V_MAX_F32         = 11,
    GCN_VOP2_V_MIN_I32         = 12,
    GCN_VOP2_V_MAX_I32         = 13,
    GCN_VOP2_V_MIN_U32         = 14,
    GCN_VOP2_V_MAX_U32         = 15,
    GCN_VOP2_V_LSHRREV_B32     = 16,
    GCN_VOP2_V_ASHRREV_I32     = 17,
    GCN_VOP2_V_LSHLREV_B32     = 18,
    GCN_VOP2_V_AND_B32         = 19,
    GCN_VOP2_V_OR_B32          = 20,
    GCN_VOP2_V_XOR_B32         = 21,
    GCN_VOP2_V_MAC_F32         = 22,
    GCN_VOP2_V_ADD_U32         = 37,   /* GFX8 */
    GCN_VOP2_V_SUB_U32         = 38,   /* GFX8 */
    GCN_VOP2_V_SUBREV_U32      = 39,   /* GFX8 */
    GCN_VOP2_V_ADDC_U32        = 40,   /* GFX8, result in VCC */
    GCN_VOP2_V_SUBB_U32        = 41,   /* GFX8 */
    GCN_VOP2_V_SUBBREV_U32     = 42,   /* GFX8 */
} GcnVop2Op;

/* ---- VOPC: vector comparison (result in VCC or SDST via VOP3) ---- */
/* ENCODING[31:25]=0111110  OP[24:17]  VSRC1[16:9]  SRC0[8:0] */
typedef enum {
    GCN_VOPC_V_CMP_F_F32    = 0x40,
    GCN_VOPC_V_CMP_LT_F32   = 0x41,
    GCN_VOPC_V_CMP_EQ_F32   = 0x42,
    GCN_VOPC_V_CMP_LE_F32   = 0x43,
    GCN_VOPC_V_CMP_GT_F32   = 0x44,
    GCN_VOPC_V_CMP_LG_F32   = 0x45,
    GCN_VOPC_V_CMP_GE_F32   = 0x46,
    GCN_VOPC_V_CMP_O_F32    = 0x47,
    GCN_VOPC_V_CMP_U_F32    = 0x48,
    GCN_VOPC_V_CMP_NGE_F32  = 0x49,
    GCN_VOPC_V_CMP_NLG_F32  = 0x4A,
    GCN_VOPC_V_CMP_NGT_F32  = 0x4B,
    GCN_VOPC_V_CMP_NLE_F32  = 0x4C,
    GCN_VOPC_V_CMP_NEQ_F32  = 0x4D,
    GCN_VOPC_V_CMP_NLT_F32  = 0x4E,
    GCN_VOPC_V_CMP_T_F32    = 0x4F,
    GCN_VOPC_V_CMP_F_I32    = 0xC0,
    GCN_VOPC_V_CMP_LT_I32   = 0xC1,
    GCN_VOPC_V_CMP_EQ_I32   = 0xC2,
    GCN_VOPC_V_CMP_LE_I32   = 0xC3,
    GCN_VOPC_V_CMP_GT_I32   = 0xC4,
    GCN_VOPC_V_CMP_LG_I32   = 0xC5, /* != */
    GCN_VOPC_V_CMP_GE_I32   = 0xC6,
    GCN_VOPC_V_CMP_T_I32    = 0xC7,
    GCN_VOPC_V_CMP_F_U32    = 0xC8,
    GCN_VOPC_V_CMP_LT_U32   = 0xC9,
    GCN_VOPC_V_CMP_EQ_U32   = 0xCA,
    GCN_VOPC_V_CMP_LE_U32   = 0xCB,
    GCN_VOPC_V_CMP_GT_U32   = 0xCC,
    GCN_VOPC_V_CMP_LG_U32   = 0xCD,
    GCN_VOPC_V_CMP_GE_U32   = 0xCE,
    GCN_VOPC_V_CMP_T_U32    = 0xCF,
} GcnVopcOp;

/* ---- VOP3 opcodes (VOP2->VOP3: 0x100+vop2op; VOP1->VOP3: 0x140+vop1op) ---- */

/* ---- SMEM opcodes ---- */
typedef enum {
    GCN_SMEM_S_LOAD_DWORD         = 0x00,
    GCN_SMEM_S_LOAD_DWORDX2       = 0x01,
    GCN_SMEM_S_LOAD_DWORDX4       = 0x02,
    GCN_SMEM_S_LOAD_DWORDX8       = 0x03,
    GCN_SMEM_S_LOAD_DWORDX16      = 0x04,
    GCN_SMEM_S_BUFFER_LOAD_DWORD  = 0x08,
    GCN_SMEM_S_BUFFER_LOAD_DWORDX2= 0x09,
    GCN_SMEM_S_BUFFER_LOAD_DWORDX4= 0x0A,
    GCN_SMEM_S_STORE_DWORD        = 0x10,
    GCN_SMEM_S_STORE_DWORDX2      = 0x11,
    GCN_SMEM_S_STORE_DWORDX4      = 0x12,
} GcnSmemOp;

/* ---- MUBUF opcodes ---- */
typedef enum {
    GCN_MUBUF_BUFFER_LOAD_FORMAT_X  = 0x00,
    GCN_MUBUF_BUFFER_LOAD_UBYTE     = 0x10,
    GCN_MUBUF_BUFFER_LOAD_SBYTE     = 0x11,
    GCN_MUBUF_BUFFER_LOAD_USHORT    = 0x12,
    GCN_MUBUF_BUFFER_LOAD_SSHORT    = 0x13,
    GCN_MUBUF_BUFFER_LOAD_DWORD     = 0x14,
    GCN_MUBUF_BUFFER_LOAD_DWORDX2   = 0x15,
    GCN_MUBUF_BUFFER_LOAD_DWORDX3   = 0x16,
    GCN_MUBUF_BUFFER_LOAD_DWORDX4   = 0x17,
    GCN_MUBUF_BUFFER_STORE_BYTE     = 0x18,
    GCN_MUBUF_BUFFER_STORE_SHORT    = 0x19,
    GCN_MUBUF_BUFFER_STORE_DWORD    = 0x1C,
    GCN_MUBUF_BUFFER_STORE_DWORDX2  = 0x1D,
    GCN_MUBUF_BUFFER_STORE_DWORDX3  = 0x1E,
    GCN_MUBUF_BUFFER_STORE_DWORDX4  = 0x1F,
    GCN_MUBUF_BUFFER_ATOMIC_ADD     = 0x40,
    GCN_MUBUF_BUFFER_ATOMIC_AND     = 0x44,
    GCN_MUBUF_BUFFER_ATOMIC_OR      = 0x45,
    GCN_MUBUF_BUFFER_ATOMIC_XOR     = 0x46,
} GcnMubufOp;

/* ---- DS opcodes ---- */
typedef enum {
    GCN_DS_DS_ADD_U32        = 0x00,
    GCN_DS_DS_SUB_U32        = 0x01,
    GCN_DS_DS_RSUB_U32       = 0x02,
    GCN_DS_DS_INC_U32        = 0x03,
    GCN_DS_DS_DEC_U32        = 0x04,
    GCN_DS_DS_MIN_I32        = 0x05,
    GCN_DS_DS_MAX_I32        = 0x06,
    GCN_DS_DS_MIN_U32        = 0x07,
    GCN_DS_DS_MAX_U32        = 0x08,
    GCN_DS_DS_AND_B32        = 0x09,
    GCN_DS_DS_OR_B32         = 0x0A,
    GCN_DS_DS_XOR_B32        = 0x0B,
    GCN_DS_DS_MSKOR_B32      = 0x0C,
    GCN_DS_DS_WRITE_B32      = 0x0D,
    GCN_DS_DS_WRITE2_B32     = 0x0E,
    GCN_DS_DS_WRITE2ST64_B32 = 0x0F,
    GCN_DS_DS_CMPST_B32      = 0x10,
    GCN_DS_DS_CMPST_F32      = 0x11,
    GCN_DS_DS_MIN_F32        = 0x12,
    GCN_DS_DS_MAX_F32        = 0x13,
    GCN_DS_DS_WRITE_B8       = 0x1E,
    GCN_DS_DS_WRITE_B16      = 0x1F,
    GCN_DS_DS_ADD_RTN_U32    = 0x20,
    GCN_DS_DS_READ_B32       = 0x36,
    GCN_DS_DS_READ2_B32      = 0x37,
    GCN_DS_DS_READ2ST64_B32  = 0x38,
    GCN_DS_DS_READ_I8        = 0x39,
    GCN_DS_DS_READ_U8        = 0x3A,
    GCN_DS_DS_READ_I16       = 0x3B,
    GCN_DS_DS_READ_U16       = 0x3C,
    GCN_DS_DS_SWIZZLE_B32    = 0x3D,
} GcnDsOp;

/* ---- FLAT opcodes ---- */
typedef enum {
    GCN_FLAT_FLAT_LOAD_UBYTE   = 0x10,
    GCN_FLAT_FLAT_LOAD_SBYTE   = 0x11,
    GCN_FLAT_FLAT_LOAD_USHORT  = 0x12,
    GCN_FLAT_FLAT_LOAD_SSHORT  = 0x13,
    GCN_FLAT_FLAT_LOAD_DWORD   = 0x14,
    GCN_FLAT_FLAT_LOAD_DWORDX2 = 0x15,
    GCN_FLAT_FLAT_LOAD_DWORDX4 = 0x17,
    GCN_FLAT_FLAT_STORE_BYTE   = 0x18,
    GCN_FLAT_FLAT_STORE_SHORT  = 0x19,
    GCN_FLAT_FLAT_STORE_DWORD  = 0x1C,
    GCN_FLAT_FLAT_STORE_DWORDX2= 0x1D,
    GCN_FLAT_FLAT_STORE_DWORDX4= 0x1F,
} GcnFlatOp;

/* ---- Emit functions ---- */
/* All return 0 on success, -1 on buffer overflow.
 * pos is updated by the instruction size (4 or 8 bytes). */

/* Generic scalar 2-source */
int gcn_emit_sop2(uint8_t *buf, size_t *pos, size_t max,
                  GcnSop2Op op, uint8_t sdst, uint8_t ssrc1, uint8_t ssrc0);
/* Generic scalar 1-source */
int gcn_emit_sop1(uint8_t *buf, size_t *pos, size_t max,
                  GcnSop1Op op, uint8_t sdst, uint8_t ssrc0);
/* Generic scalar immediate */
int gcn_emit_sopk(uint8_t *buf, size_t *pos, size_t max,
                  GcnSopkOp op, uint8_t sdst, uint16_t simm16);
/* Generic scalar compare */
int gcn_emit_sopc(uint8_t *buf, size_t *pos, size_t max,
                  GcnSopcOp op, uint8_t ssrc1, uint8_t ssrc0);
/* Generic scalar program-flow */
int gcn_emit_sopp(uint8_t *buf, size_t *pos, size_t max,
                  GcnSoppOp op, uint16_t simm16);

/* Scalar memory load/store (64-bit instruction, immediate offset) */
int gcn_emit_smem(uint8_t *buf, size_t *pos, size_t max,
                  GcnSmemOp op, uint8_t sdst, uint8_t sbase_div2,
                  uint32_t offset, int glc);

/* VOP1: 1-src vector */
int gcn_emit_vop1(uint8_t *buf, size_t *pos, size_t max,
                  GcnVop1Op op, uint8_t vdst, uint32_t src0);
/* VOP2: 2-src vector (e32) */
int gcn_emit_vop2(uint8_t *buf, size_t *pos, size_t max,
                  GcnVop2Op op, uint8_t vdst, uint8_t vsrc1, uint32_t src0);
/* VOPC: vector compare, result → VCC */
int gcn_emit_vopc(uint8_t *buf, size_t *pos, size_t max,
                  GcnVopcOp op, uint8_t vsrc1, uint32_t src0);

/* VOP3: 64-bit, 3 sources, optional neg/abs/clamp/omod.
 * opcode: VOP3 opcode (0x100+vop2op or 0x140+vop1op or native VOP3 ops).
 * sdst: destination SGPR for VOPC-in-VOP3 (set to GCN_VCC_LO for VCC).
 * For VOP3A (vector dest) sdst_is_vgpr=1 and sdst = vgpr index. */
int gcn_emit_vop3(uint8_t *buf, size_t *pos, size_t max,
                  uint16_t op, uint8_t vdst, int vdst_is_vgpr,
                  uint32_t src0, uint32_t src1, uint32_t src2,
                  uint8_t neg, uint8_t abs, int clamp, uint8_t omod);

/* DS: local data share (64-bit) */
int gcn_emit_ds(uint8_t *buf, size_t *pos, size_t max,
                GcnDsOp op, uint8_t vdst, uint8_t addr,
                uint8_t data0, uint8_t data1,
                uint8_t offset0, uint8_t offset1, int gds);

/* MUBUF: untyped buffer memory (64-bit) */
int gcn_emit_mubuf(uint8_t *buf, size_t *pos, size_t max,
                   GcnMubufOp op,
                   uint8_t vdst_or_vdata, /* VDST for loads, VDATA for stores */
                   uint8_t vaddr,          /* offset vgpr (OFFEN) or index (IDXEN) */
                   uint8_t srsrc_div4,     /* SRD base SGPR / 4 */
                   uint8_t soffset,        /* soffset SGPR or inline const */
                   uint16_t offset,        /* 12-bit immediate offset */
                   int offen, int idxen, int glc, int slc);

/* FLAT: flat memory (64-bit, addr is 64-bit in VGPR pair vaddr:vaddr+1) */
int gcn_emit_flat(uint8_t *buf, size_t *pos, size_t max,
                  GcnFlatOp op,
                  uint8_t vdst,   /* destination VGPR (loads) or 0 (stores) */
                  uint8_t vdata,  /* source VGPR (stores) or 0 (loads) */
                  uint8_t vaddr,  /* low VGPR of 64-bit address pair */
                  int glc, int slc);

/* ---- Convenience wrappers ---- */
#define gcn_s_endpgm(b,p,m)     gcn_emit_sopp(b,p,m, GCN_SOPP_S_ENDPGM, 0)
#define gcn_s_waitcnt(b,p,m,v)  gcn_emit_sopp(b,p,m, GCN_SOPP_S_WAITCNT, v)
#define gcn_s_barrier(b,p,m)    gcn_emit_sopp(b,p,m, GCN_SOPP_S_BARRIER, 0)
#define gcn_s_mov_b32(b,p,m,d,s) gcn_emit_sop1(b,p,m, GCN_SOP1_S_MOV_B32, d, s)
#define gcn_s_mov_b64(b,p,m,d,s) gcn_emit_sop1(b,p,m, GCN_SOP1_S_MOV_B64, d, s)
#define gcn_v_mov_b32(b,p,m,d,s) gcn_emit_vop1(b,p,m, GCN_VOP1_V_MOV_B32, d, s)

/* Instruction size: 4 bytes for most, 8 for SMEM/DS/MUBUF/FLAT/VOP3 */
#define GCN_INSN_SIZE32  4
#define GCN_INSN_SIZE64  8

#endif /* GCN_ENC_H */
