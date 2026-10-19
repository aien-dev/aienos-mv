# KTAP blocks in receipts

The C kernel prints its own verdicts on its in-kernel self tests as one KTAP
version 1 block at the end of every boot that reaches the final report
(`native/kernel/core/ktap.h`; format: https://docs.kernel.org/dev-tools/ktap.html,
idea borrowed from Linux KUnit, no code copied). The same block comes out of
QEMU, the Pi 5 and the Spark serial consoles, and one reader judges all of
them: `scripts/ktap_summary.sh`.

## What a receipt embeds

1. The block itself, cleaned of console noise:
   `bash scripts/ktap_summary.sh --block <serial log>`.
   Paste its output verbatim in a fenced block. Do not retype or reorder it.
2. The verdict line: `bash scripts/ktap_summary.sh <serial log>`, last line,
   `KTAP_SUMMARY: PASS|FAIL total=N pass=P fail=F skip=S [(reason)]`,
   and its exit status (0 PASS, 1 FAIL).
3. Where the log came from: machine (QEMU, Pi 5, Spark, iMac), image commit
   (the `aienos_commit:` line of the same log), and the sha256 of the full raw
   serial log, so a reader can check the block was cut from that log.
4. Which rows of `ASSUMPTIONS.md` the run relied on. A QEMU run always relies
   on rows E, F and G.

Example shape:

    machine: qemu (qemu-system-aarch64 -cpu max, TCG)
    image commit: <40-hex>
    serial log sha256: <64-hex>
    assumptions relied on: A1 B E F G
    ```ktap
    KTAP version 1
    1..5
      KTAP version 1
      # Subtest: mm
      1..4
      ok 1 mmu_enabled
      ...
    ok 5 stages
    ```
    KTAP_SUMMARY: PASS total=16 pass=13 fail=0 skip=3 (exit 0)

## Rules

- A receipt with no block, a cut block or a FAIL verdict never counts as a
  pass, whatever the other report lines say. A kernel that panics or faults
  before the final report prints no block, and the reader says so.
- SKIP is not PASS. A skipped case (for example a stage that is not linked
  into the image) must be named as skipped wherever the receipt is quoted.
- The block lists only what the kernel checked about itself. Gate scripts
  still check report lines the kernel cannot judge (for example the exact
  commit, or the expected GIC version for the QEMU machine). Quote both.
- QEMU qualifies nothing physical. A QEMU block is emulator evidence only.

## Not done yet

- The block goes out on the serial console with the other report lines.
  LAVA's advice (`lava-signal: kmsg`) is to send test markers through the
  kernel log path so lines from other printers cannot split them; the kernel
  log (`core/klog.c`) is linked only into the Pi 5 images today, so this is
  left for a later change. The reader tolerates whole noise lines between
  KTAP lines, but a line split in the middle by another printer fails the
  block (fail closed).
- The x86 kernel (`native/kernel/x86`) does not print a block yet.
