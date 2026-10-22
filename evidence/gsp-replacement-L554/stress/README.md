# Expanded offline GSP reconstruction qualification

> [!WARNING]
> **UNQUALIFIED FOR REAL HARDWARE. OFFLINE RESEARCH ONLY.**
>
> This is a partial behavioral reconstruction, not a complete or bootable GSP
> firmware replacement. Do not install or flash it, replace the working GPU
> firmware, or use it to reset or reconfigure a live GPU.
>
> Unqualified hardware experiments can cause GPU failure, system hangs, loss of
> display, data loss, or an unbootable system. Recovery is not guaranteed;
> failures involving persistent boot firmware or security state may require
> vendor repair or replacement.
>
> Boot admission, full GPU functionality, power/thermal safety, and hardware
> recovery/rollback have **not** been qualified. Passing host, emulator, symbolic,
> or code-review checks does not establish hardware safety. Live hardware testing
> on the development Spark is not authorized.

**PASS for the named host-only scope. Not replacement firmware or a GPU cutover.**

Tested source: `c5192f25cab953dcdf04657c4a555d9f4d712fc7`.
The source-hash manifest in `stress-result.json` binds every consumed research
source; `base_source_pin` identifies the earlier milestone, not this revision.
The existing `offline/` receipts are historical. `legacy-green.json` is the fresh
rerun against this revision, including direct-core tests and its original mutants.

## Results actually executed

| Check | Result |
|---|---|
| Original RV64 bytes under QEMU, original bytes under Unicorn, native Omega | **1,015,952 records per engine, zero mismatches** against the independent exact-integer contract |
| Distinct input tuples | 1,007,332; repeated/metamorphic executions are not claimed as distinct inputs |
| Corpus coverage | Exhaustive low/high/mixed small domains; all bit-position boundaries; empty, overflow, adjacency, overlap, disjoint, contained, identical and full-width cases |
| Metamorphic relations | 15,952 symmetry/translation pairs checked |
| Symbolic routine check | Seven returning paths; coverage and disagreement queries UNSAT for the correct contract; three deliberately wrong contracts SAT with replayed counterexamples |
| Unspecified-register poisoning | 10,561 calls; all seven paths observed; callee-saved registers plus SP/GP/TP preserved |
| Directed ELF parser checks | 2,616: 2,583 expected refusals and 33 expected acceptances |
| Random header mutations | 10,000: 7,052 refused, 2,948 structurally accepted and checked for load/entry invariants |
| Emulator stop controls | Ten: SYSTEM/CSR, unmapped read/fetch, execution budget, mapped page padding and compressed break |
| Transport controls | 101: all short-record lengths at three completed-record boundaries, fragmented input and read/write errors |
| Long-stream/capacity controls | Seven, including 999,999 / 1,000,000 / 1,000,001 records, capacity−1, exact capacity and two over-capacity inputs |
| Compiled behavioral mutants | Eight compile successfully, run normally, and produce actual mismatches; all detected |
| Binding / external-oracle controls | Seven fail-closed pin checks, plus 13 independently evaluated Wolfram boundaries replayed in each of QEMU and Omega |
| Original verifier rerun | 10,561 comparisons, twelve direct-core cases, original transport checks and four original compiled mutants pass |

All three large-corpus output hashes are:
`ca0f72d81d5529026701d2dc9eb108648984bcad74a55ce8d2ba1d957c0c65d0`.

Random corruption is not synonymous with invalid ELF: unused padding, advisory
section-table fields and some changed load addresses can remain structurally
valid. The parser is not a signature verifier. Those acceptances are not hidden
failures or claimed rejections. The outer firmware pin is tested separately.

## A real defect the larger run found and fixed

The earlier Omega host adapter used `while true`. The pinned seed compiler bounds
that loop to 1,000,000 iterations. The adapter returned **104 after exactly
1,000,000 successful records**, even though the next tuple `(0,0,0,0)` passed in
isolation. QEMU had completed the full corpus. The failing receipt is retained in
`omega-long-stream-failure.json` and the full stage record in
`stress-omega-failure.json`.

The fix is explicit bounded batching: **4096 batches × 512 records = 2,097,152
complete records per process**. Compiler limits were not changed or bypassed;
the corpus was not shortened or split into smaller processes. The pure `ranges`
function is unchanged. Contract:

- Complete records before a later refusal keep their outputs.
- Clean EOF through exact capacity: exit 0.
- Incomplete final record before capacity: exit 2.
- Read/write syscall error: exit 3 (OS signal termination is not normalized).
- Any extra input beyond capacity: exit 4, no output for the excess input.

The last two capacity cases retain exactly 16,777,216 output bytes and exit 4.
A process accepts at most 67,108,864 bytes of complete records. Capacity arithmetic
was independently checked with Wolfram; see `wolfram-capacity.json`.

## Test-harness corrections retained, not counted as firmware bugs

1. The first QEMU adapter link relaxed an output address into a GP-relative access,
   but a bare `_start` has no CRT to initialize GP. It faulted before producing output.
   `.option norelax` now preserves PC-relative address formation. Linked leaf bytes
   are verified unchanged, and adapter smoke tests run before the full corpus.
2. The first signed-order mutant used Omega checked unsigned-to-signed conversions.
   It exited 105 on a high-bit input rather than testing signed comparison. A
   standalone conversion probe reproduced that. The corrected mutant uses XOR
   with the sign bit; it compiles, exits 0, and yields 1,193 wrong results.
3. Random-parser invariant assertions are outside the exception handler that counts
   parser refusals, so an invalid accepted structure fails the test rather than
   being mislabeled a rejection.

Historical failure records are retained alongside current PASS records.

## What the symbolic result does and does not establish

`prove_leaf.py` decodes the pinned 44 bytes with Capstone and symbolically models
only their explicit RV64 instructions. Z3 compares all four-u64 argument
combinations against a separate 65-bit endpoint contract. Unsupported instructions,
uninitialized-register dependencies, uncovered paths and solver UNKNOWN fail.
Three bad contracts must yield concrete counterexamples, included in the execution
corpus. This is **not** a proof of Capstone, Z3, QEMU, Unicorn, the Omega compiler,
GPU hardware or all firmware. The Omega implementation is qualified by execution,
not by this symbolic lift. Other GSP functions are not covered.

## Reproduce

Use the private snapshot and seed compiler described in
`../../../spikes/gsp-L554-offline/README.md`. Test-only Python dependencies:
`unicorn==2.1.4`, `capstone==5.0.6`, `pyelftools==0.32`, `z3-solver==4.13.0.0`.
QEMU and RISC-V GNU assembler/linker were extracted from Ubuntu packages into
`$LAB/stress/tools`, not installed system-wide. Their executable hashes/versions
and build arguments are in `stress-result.json`. The compiler hash is pinned.

```sh
LAB="$HOME/handoffs/gsp-L554"
CASE="$LAB/snapshot-20261022T212950Z"
SRC="$HOME/workspace/aienux-gsp-replacement-L554/spikes/gsp-L554-offline"
PY="$LAB/emulator-venv/bin/python"
mkdir -p "$LAB/stress"
# Put the retained wolfram-boundaries.json in $LAB/stress first.
"$PY" "$SRC/prove_leaf.py" "$CASE" "$LAB/stress"
"$PY" "$SRC/stress_ranges.py" "$SRC" "$LAB"
"$PY" "$SRC/adversarial.py" "$SRC" "$LAB"
"$PY" "$SRC/final_controls.py" "$SRC" "$LAB"
"$PY" "$SRC/verify_ranges.py" "$LAB" "$CASE" "$SRC/ranges.om"
```

Run with normal Python, not `-O`: assertions are intentional fail-closed gates.
The scripts write only their task-owned lab outputs. No live GPU access, installed
firmware writes, module changes, resets or service restarts are part of the suite.
The installed firmware's final hash still equals the original snapshot:
`32cb7cf3e77f97e92f6e92135939ccb3ba382748144569c0dfccf3338d43d963`.
This does not establish service uptime or an inference regression result.

Vendor firmware, extracted binary bytes, full disassemblies, generated executables
and large corpora remain local. Published material is source, identities and
bounded receipts. Full GSP replacement and its hardware-admission, functionality,
lifecycle and recovery qualification remain open under L554 / aienux#443.
