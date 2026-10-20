# Aienux native baseline on current main, 2026-10-20 (L201)

Scope: host tests, the Spark-shape SMMU case, the IORT decode fixture and the QEMU gates of
`native/kernel/GATES.md`, re-run on one commit. Host builds and QEMU only. No reboot, no boot-order
or ESP change, no firmware load, no real-device access. QEMU qualifies nothing physical.

Commit under test: `40f977bc8acd708fc49f5f0e1bceefc8b3044b43` (branch `feat/spark-native-baseline`,
base `origin/main` 18e276590424593647cd7c39c6d16c2f9f8d6027; the one change on top of main is the
stale-IORT text correction below, no code). Machine: the DGX Spark (aarch64), `qemu-system-aarch64`
with AAVMF. Raw logs and receipts: `~/handoffs/L201/` on the Spark (not in the repo).

Tags: OBSERVED = seen in a log of this run. INFERRED = reasoned from observed data. UNKNOWN = not measured.

## 1. Host tests

| Item | Command | Exit | Result |
|---|---|---|---|
| Whole host suite incl. `test_smmu`, `iort-mutants`, `smmu-l2-mutants` | `make -C native/kernel OUT=$PWD/target/native-kernel-baseline-host -j8 test` | 0 | `CK_CORE_HOST: PASS`; iort-mutants PASS; smmu-l2-mutants PASS (OBSERVED, host-test.log) |
| Spark-shape SMMU case | `test_smmu` from the same build | 0 | `test_smmu: PASS (202 checks)` (OBSERVED, test_smmu.log) |

## 2. IORT decode fixture against `docs/GB10_IORT_DECODE.md`

Input: the saved copy of the Spark's real IORT (3904 bytes, sha256 `5e0096a2be83ce20...`, the same
hash `docs/GB10_IORT_DECODE.md` names), read from a saved file; nothing under `/sys` was read in this run.

| Item | Command | Exit | Result |
|---|---|---|---|
| Kernel parser on the real table | `ck_acpi_scan --iort IORT.bin DSDT.bin SSDT1..5.bin` | 0 | Accepted: 3 SMMUv3 nodes; 15 PCI root-complex mappings to the first SMMUv3 against `CK_IORT_MAX_MAPS=32`; segment 15 rid 0x0..0xffff to stream 0x0.. on SMMUv3 `0x13000000`; RMR node 0xc18, streams 0x0..0x100, 3 ranges (OBSERVED) |
| Independent decoder | `rustc --edition 2021 -O -o iort_decode scripts/iort_decode.rs`, then `iort_decode IORT.bin 15 0x100` | 0, 0 | Root complex node 0x72c to SMMUv3 node 0xa0 (base `0x13000000`, StreamID 0x100) to ITS group DeviceID 0x100 (OBSERVED) |

Agreement with the document: the decode chain and the 3 SMMU nodes match `docs/GB10_IORT_DECODE.md` (OBSERVED).

## 3. Stale text corrected (gap D3)

`native/kernel/README.md`, `native/kernel/GATES.md` row 32b and `tests/fixtures/spark_xhci_acpi_expected.txt`
said the Spark IORT is refused ("IORT malformed", `SmmuNotReady`, 15 mappings against a limit of 8).
`CK_IORT_MAX_MAPS` is 32 (`core/acpi.h`), and the run above accepts the table with 15 mappings.
Corrected in commit 40f977bc. No test reads that fixture (OBSERVED by grep).

## 4. QEMU gates driven by `scripts/ck_gates.sh` (27 gates)

Command shape: `bash scripts/ck_gates.sh --out ~/handoffs/L201/receipt [--only <list>]`. The runner runs each
child script as `bash <script>` and reports the child's exit code. Child exit 3 means NOT_RUN because
another agent's QEMU job held the shared gate lock; it is not a pass. The first full run (17:06Z) ended with
8 PASS and 19 NOT_RUN for exactly that reason. The 19 were re-run with `--only` until each ran; no gate
reported FAIL at any point, and nothing was retried after a FAIL (there was none).

| Gate | Child script | Child exit | Seconds | Run |
|---|---|---|---|---|
| M1 | qemu_ck_boot_test.sh | 0 | 6 | rerun-1 |
| M3 | qemu_ck_boot_test.sh | 0 | (same run as M1) | rerun-1 |
| SMMU | qemu_ck_store_test.sh | 0 | (same run as M4_NVME) | rerun-1 |
| NVME_SHUTDOWN | qemu_ck_store_test.sh | 0 | (same run as M4_NVME) | rerun-1 |
| P2_ARTIFACT | qemu_ck_artifact_test.sh | 0 | 34 | rerun-1 |
| M4_NVME | qemu_ck_store_test.sh | 0 | 120 | rerun-1 |
| M4_STORE | qemu_ck_store_test.sh | 0 | (same run) | rerun-1 |
| M4_STORE_CRASH | qemu_ck_store_crash_test.sh | 0 | 715 | rerun-1 |
| M4_CONTINUITY | qemu_ck_continuity_test.sh | 0 | 191 | rerun-1 |
| M4_RECOVERY | qemu_ck_recovery_test.sh | 0 | 125 | rerun-1 |
| M4_ALLEN | qemu_ck_allen_test.sh | 0 | 215 | rerun-1 |
| ARGUS1_REVOKE | qemu_ck_store_test.sh | 0 | (same run as M4_NVME) | rerun-1 |
| KEYBOARD | qemu_ck_keyboard_test.sh | 0 | 44 | rerun-1 |
| NET | qemu_ck_net_test.sh | 0 | 19 | rerun-1 |
| SMP | qemu_ck_smp_test.sh | 0 | 6 | rerun-1 |
| DISK_LAYOUT | qemu_ck_disk_layout_test.sh | 0 | 58 | rerun-1 |
| FPU | qemu_ck_fpu_test.sh | 0 | 6 | rerun-1 |
| INFER | qemu_ck_infer_test.sh | 0 | 132 | rerun-1 |
| SCREEN | qemu_ck_screen_test.sh | 0 | 17 | rerun-6 (NOT_RUN in rerun-1..5, lock held) |
| CONSOLE | qemu_ck_console_test.sh | 0 | 234 | first run |
| M0_ROLLBACK | qemu_ck_rollback_test.sh | 0 | 168 | first run |
| OSC_UNIT | qemu_ck_osc_unit_test.sh | 0 | 25 | first run |
| OSC_LAUNCH | qemu_ck_osc_launch_test.sh | 0 | 16 | first run |
| OSC_RECEIPT | qemu_ck_osc_receipt_test.sh | 0 | 16 | first run |
| OSH_CORE | qemu_ck_osh_core_test.sh | 0 | 21 | first run |
| OSH_OP | qemu_ck_osh_op_test.sh | 0 | 17 | first run |
| SLOT_LOADER | qemu_ck_slot_loader_test.sh | 0 | 274 | first run |

Result: 27 of 27 PASS, 0 FAIL (OBSERVED, ck-gates.log and rerun-1/6 logs; receipts in `~/handoffs/L201/receipt/`).
NVMe DMA mode reported by the runner: "Confined (QEMU SMMUv3 stage 1, out-of-window DMA faulted); no-SMMU boot
denied (NoSmmu); TEST-ONLY bypass image checked separately" (OBSERVED, rerun-1.log).
The runner's own final line is `SELECTED_GATES_NOT_ALL_PASS` per partial run because each re-run selected a
subset; the table above is the union of the per-gate results, which is how it is stated here. A single
all-27 receipt for this commit does not exist (INFERRED cause: shared gate lock held by other agents; UNKNOWN
whether one uninterrupted run would differ).

## 5. Gates in `GATES.md` that `ck_gates.sh` does not run

Each ran as `quiet-build bash scripts/<script>` from the commit under test; retried only on exit 3 (gate lock held by another agent), never after a PASS or FAIL.

| Gate | Script | Exit | Result |
|---|---|---|---|
| LIFECYCLE | qemu_ck_lifecycle_test.sh | 0 | `AIENOS_CK_LIFECYCLE: PASS (QEMU only; hardware NOT_RUN)` (OBSERVED, extra-lifecycle.log) |
| MEMORY | qemu_ck_memory_test.sh | 0 | `AIENOS_CK_MEMORY: PASS (QEMU only; hardware NOT_RUN)` (OBSERVED, extra-memory.log) |
| SERVICE_DISK | qemu_ck_service_disk_test.sh | 1 | **FAIL**: 99 rows PASS, 1 row FAIL (ROW 206, `M1 checks`), `AIENOS_CK_SERVICE_DISK: FAIL` (OBSERVED, extra-service_disk.log) |

### SERVICE_DISK FAIL: cause

The one failing row is the M1 tick-statistics check inside one boot: `timer_stats: intid=30 freq_hz=62500000
period_us=10000 min_us=31 avg_us=11382 max_us=35787`. The check needs `2*min >= period`; 31 us is far below 5000 us
(OBSERVED). Every functional row of that boot passed (recall, facts, unload, stop order, QEMU exit 0).
The machine was heavily loaded at the time (load average above 30, OBSERVED by `uptime`).

Cause (INFERRED from code, `native/kernel/arch/timer.c` at this commit, and consistent with the numbers): `ck_timer_tick`
re-arms the compare value from the counter read at IRQ entry, then reads the counter again to timestamp the interval.
If the host stalls the QEMU vCPU between those two reads, one interval comes out long and the next one short.
The long one (max 35787 us) and the short one (31 us) are exactly that pair. This is the known flake
AIENOS-CK-QEMU-GATES, already root-caused by the desk lane (ledger L081/L082): fix is PR aienos#438, branch
`fix/timer-window-starvation` (head 54fe5b96), which uses the IRQ-entry counter for both ends of an interval
(18/20 red to 20/20 green in that branch's own evidence). It is not in `origin/main` (18e27659). No new fixer
line was opened: a fix already exists and is in the desk's queue.

This is a FAIL of this baseline, not a waiver. The baseline is not green for SERVICE_DISK on current main.
The other 29 QEMU gates (27 + LIFECYCLE + MEMORY) pass at this commit. Section 6 records the re-run on the fix branch.

### Not run, by design (OBSERVED in `GATES.md`)

- TRUST-1 Gate 4 (`qemu_security_suite.sh`, `qemu_secureboot_signing_test.sh`, GATES.md rows 92-95): NOT_RUN,
  MISSING_IMPLEMENTATION. These boot the retired Rust loader image; no C-kernel version exists.
- The Rust-reference scripts (`qemu_boot_test.sh` and relatives) boot the Rust loader image, not the Aienux kernel;
  not part of this baseline.
- Hardware: every gate here is QEMU only. Nothing physical was touched or qualified.

## 6. Re-run on the timer fix, and the owner-keys check

SERVICE_DISK, `quiet-build bash scripts/qemu_ck_service_disk_test.sh`, run in a throwaway worktree of
`origin/fix/timer-window-starvation` at 54fe5b96 (PR aienos#438): exit 0, 100 rows PASS, 0 FAIL,
`AIENOS_CK_SERVICE_DISK: PASS (QEMU only; hardware NOT_RUN)` (OBSERVED, svcdisk-fix.log; two earlier attempts
returned exit 3, gate lock held by another agent, and were retried on exit 3 only).
One PASS on the fix branch is consistent with the cause in section 5 but does not prove it: the flake needs host
load to show, and this run was a single run (INFERRED; the 18/20 to 20/20 repeat evidence belongs to #438's own record).
Result for main 18e27659 plus this branch's text-only changes: SERVICE_DISK stays FAIL until #438 lands.

Owner-keys check, `quiet-build bash scripts/ck_owner_keys_check.sh`: exit 0, 37 PASS, 0 FAIL,
`CK_OWNER_KEYS_CHECK: PASS` (OBSERVED, owner-keys.log). Hardware staging refuses the TEST Store keys and TEST machine id.

## 7. Baseline verdict

| Item | Result |
|---|---|
| Host `make test`, test_smmu (202 checks), IORT mutants | PASS |
| IORT decode fixture vs `docs/GB10_IORT_DECODE.md` | PASS (15 maps, limit 32) |
| 27 `ck_gates.sh` gates + LIFECYCLE + MEMORY | PASS |
| SERVICE_DISK on current main | **FAIL** (timer flake, fix = #438, PASS once on fix branch) |
| Owner-keys check | PASS |
| TRUST-1 Gate 4, Rust-reference scripts | NOT_RUN by design (section 5) |

The baseline is NOT fully green: one gate fails on current main until #438 merges.
