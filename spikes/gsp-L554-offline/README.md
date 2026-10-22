# GSP disk negative, instruction print and emulation spike

## Verdict: PARTIAL, with an executed host-native Omega reconstruction

The requested negative is a byte-for-byte snapshot of the installed GSP blob,
SHA-256 `32cb7cf3e77f97e92f6e92135939ccb3ba382748144569c0dfccf3338d43d963`,
75,012,080 bytes. Its source hash was unchanged before/after copying. It remains
under the task-owned Spark case directory; no vendor blob is committed here.

The print is an ELF load-segment map plus instruction listings generated from
those exact bytes. Eleven packed RISC-V executable payloads have valid program
headers and file-backed entry points. Their original section tables were omitted
from the packed image: following the stale section offsets consumes the next
payload. One additional ELF-magic occurrence is incidental data, not an ELF.
The remaining image bytes are NOT exhaustively classified by this inventory.

`print_segments.py` therefore uses bounded PT_LOAD records and validates entry,
segment extents/alignment and nonoverlap. It wraps unchanged executable bytes in
analysis-only .text sections for LLVM, independently checks the wrappers with
readelf/pyelftools, and produces complete gzip instruction listings locally.
Executable segments may contain data/padding; a linear sweep is not proof every
printed instruction is reachable. This is binary recovery, not original source.

## Actual emulation and first handwritten Omega behavior

`emulate_firmware.py` executes the copied RISC-V routine at raw .fwimage offset
0x28 using Unicorn 2.1.4. Its inferred calling convention takes four unsigned
64-bit values: start/length of two half-open ranges. Empty ranges return zero;
endpoint overflow returns one; otherwise it reports intersection. No hardware
register is used by this routine.

The original binary agreed with an independent unbounded-integer predicate on
10,561 executed inputs: 6,561 boundary combinations, 2,000 full-width randomized
inputs, and 2,000 small-range randomized inputs. Exact corpus/output hashes and
counts are in the emulation receipt. Tests are examples, not universal proof.

`ranges.om` is handwritten seeded Omega, not transpiled assembly or generated C.
It implements that observed behavior using overflow-safe endpoint checks. It
compiles to a static AArch64 host executable with the qualified seed-born compiler
`01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6`,
from Omega source pin `413f87c84e9b4233e4a56487dc6643b2139b5d0c`.
There is no C compiler/runtime in this artifact's construction/computation chain.
Linux, shell, file utilities and the external emulator remain lab infrastructure.

Initial milestone results (historical; the current-source rerun is retained in
`evidence/gsp-replacement-L554/stress/legacy-green.json`):
- Compilable constant-zero RED: 6,341 mismatches, normal compile/run exits.
- Handwritten Omega GREEN: 10,561/10,561 match emulated original bytes.
- Twelve additional direct-core calls, without the streaming adapter: PASS.
- Empty input, 31 incomplete-record lengths, completed records then incomplete
  final record: PASS with the stated stream status/output contract.
- Four successfully compiled mutants detected: omitted empty-range guard (648
  mismatches), first overflow (2,988), second overflow (1,696), inclusive adjacency
  (132). The unchanged original was rerun afterward.

This is a host-native mirror of ONE small firmware behavior. It is NOT RISC-V
replacement firmware, whole-GSP equivalence, a driver cutover, or a GPU feature
parity result. No compiler backend or another owner's worktree was changed.

## Expanded stress qualification

The [expanded test report](../../evidence/gsp-replacement-L554/stress/README.md)
records 1,015,952 comparisons per engine (QEMU, Unicorn and native Omega),
symbolic path checking, adversarial parser/transport controls and eight compiled
mutants. It also retains the long-stream defect the larger tests found: the old
adapter hit the compiler loop budget after 1,000,000 records. The current adapter
uses explicit bounded batches, accepts at most 2,097,152 complete records per
process, and refuses excess input with exit 4 while retaining prior outputs.
This transport correction does not change the pure `ranges` function or compiler.

## Startup trace and next concrete dependency

A separate raw-prefix experiment assumes .fwimage offset zero, load base
0x10000000, zero initial integer registers, and only copied-image memory.
Those are explicit experiment assumptions, not recovered GB10 reset state.
It observes 40 instructions, stopping on a four-byte read at
`0x2000000000000a00` (instruction at raw offset 0x1f08).
The emulator does not invent a read value or continue past the missing device.
All SYSTEM/CSR instructions are also stop boundaries, not pretend implementations.

The public NVIDIA header at pinned source
`20e4e6e19cc26ba47b5cbe23130a396be100c427` defines offset 0xa00 as
`NV_PMC_BOOT_42`, with chip/revision fields. That is a register identification
candidate; the GSP address-window translation and actual reset value still need
independent confirmation. The next device model must derive those semantics from
source/recorded evidence and retain refusal on unknown reads. No live MMIO was
read to fill this gap.

## Reproduce on AArch64 Linux (Spark)

Use a private lab directory, never the installed firmware directory. Dependencies
are isolated test tools: Python, Unicorn 2.1.4, Capstone 5.0.6, pyelftools 0.32,
LLVM objdump 18 and readelf. The scripts do not download dependencies themselves.

```sh
LAB="$HOME/handoffs/gsp-L554"
CASE="$LAB/snapshot-20261022T212950Z"
SRC="$HOME/workspace/aienux-gsp-replacement-L554/spikes/gsp-L554-offline"
"$LAB/emulator-venv/bin/python" "$SRC/print_segments.py" "$CASE"
"$LAB/emulator-venv/bin/python" "$SRC/emulate_firmware.py" "$CASE"
python3 "$SRC/verify_ranges.py" "$LAB" "$CASE" "$SRC/ranges.om"
```

The initial snapshot is a copy of `/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin`;
the tool refuses any different SHA-256. The native verifier requires a local
`$LAB/seed-omc` with the exact compiler hash above. Its source reconstruction and
seed regression log are in the accompanying evidence directory. The original
vendor blob, extracted payloads, full listings, corpus binaries and compiled test
executables remain local. Published evidence contains their identities/results.

## Investigation corrections retained

- Initial generic-ELF extraction followed discarded section tables, accepting six
  misleading extents and rejecting five genuine payloads. PT_LOAD recovery exposed
  the exact cause and now recovers all eleven, with no overlapping payloads.
- Unicorn initially raised a fetch error on a function's unmapped return sentinel
  after correctly executing `ret`. A separate mapped sentinel page resolves this;
  no sentinel instruction or device reply is executed.
- A report-only Path indexing typo was fixed; the complete printer was rerun.

No firmware replacement, secure-boot admission, cold/warm restart, fault recovery,
full repository integration, independent hardware test, or live retirement is
claimed. The original L554 full-function goal remains open. The signing boundary
does not prevent this offline behavioral-reconstruction work.
