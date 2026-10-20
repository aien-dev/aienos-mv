# scripts/gpu_bench -- bench scripts for the ck_gpu_bench console

The console (`ck_gpu_bench`, docs/IMAC_GPU_BENCH.md on feat/imac-gpu-bench) reads these.
Check them before a run:

    bash scripts/gpu_bench/check_bench.sh          # exit 0 clean, 1 grammar error, 3 allowlist gaps

Grammar is checked against the console's commands and argument counts. An allowlist gap
(exit 3) is not a grammar error: the console refuses that write with BENCH_ERR_RANGE until
the entry below is added to its allowlist. The console lives on the bench branch, so this
branch cannot add the entries itself.

## Write allowlist entries STEP1-3 need (the required ones)

The console's current allowlist is DWORD 0x4800-0x4900 and 0x4A00-0x4C00 (BAR5).

| Step  | Writes (BAR5 DWORD index)                | Entry to add                       |
|-------|------------------------------------------|------------------------------------|
| STEP1 | 0x5045-0x5048 (GRPH primary surface, flip, update) | 0x5040-0x5050 (DCE 11.2 GRPH, controller 0) |
| STEP3 | 0x178, 0x179, 0x180 (SMC message, arg, response)   | 0x0170-0x0180 (SMU message registers) |

## Entries STEP4-6 need (the same console change, for those runs)

| Step  | Writes (BAR5 DWORD index)                        | Entry to add |
|-------|--------------------------------------------------|--------------|
| STEP4 | 0x817, 0x2150, 0x3100, 0x30d0, 0x20c1, 0x20c3    | 0x0800-0x0820, 0x2150, 0x3100, 0x30d0, 0x20c0-0x20c6 |
| STEP5 | 0x3492, 0x3493, 0x3496                           | 0x3490-0x3496 |
| STEP6 | 0x803, 0x2120, 0x2123, 0x2128, 0x213b           | 0x0803, 0x2120-0x213b |

## Changes from the first version of these files

- Commands were in an older form (`r32`, `w32`, `poll`) with byte offsets. Now they use
  the console's commands (`bar5_read`, `bar5_write`, `bar5_poll`) and DWORD indices
  (byte offset / 4, given in each comment).
- STEP1: CRTC_STATUS was byte 0x1213C; the register is 0x604e, byte 0x18138.
- STEP2: AGP_TOP and AGP_BOT were both read at byte 0x2C; now 0xb (byte 0x2C) and 0xc (byte 0x30).
- STEP3: the "wait for a non-zero response" poll cannot be written with the console's
  equal-to poll. It now waits for PPSMC_Result_OK (0x01).
- STEP4 and STEP6: a poll line had no arguments; fixed.
- Inline comments moved to their own lines (the console doc says comments start with `#`).

## Not verified (do not run blind)

- SMU message 0x0201 (GetSMFWVersion): not in smu7_ppsmc.h (Linux v6.6,
  drivers/gpu/drm/amd/pm/powerplay/inc). PPSMC_Result_OK = 0x01 is verified (same file, line 83).
- STEP4 GRBM_SOFT_RESET bit values (0x00010005, 0x0): not checked against gfx_v8_0.c.
- The checker assumes the console parses 0x-prefixed hex. Confirm in the console source.
- STEP5's verify read at BAR0 + 0x2400000 (DWORD 0x900000) is outside the console's BAR0
  window (DWORD 0-65535). It needs a wider window or a readback by the driver.
- Ring, PQ, EOP and fence addresses are physical addresses from the driver's boot log.
  The scripts leave those lines commented.

Register indices are from native/kernel/gpu/amd_gpu_regs.h. Every STEP index was checked
against that header on 2026-10-19; the STEP2/3/4/5/6 byte offsets match except where noted.
