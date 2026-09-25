# System Store v1 host crash/fault harness (2026-09-25)

Lane: `feat/p3-store-crash-harness` (test infrastructure and characterization only).
Base: merge-equivalent state of `aienos/main` `3f8ebf3` (#132 NVMe write/flush)
plus PR #131 `d401662` (System Store v1 engine). Merge tree `a32c2a59`.

**This is host-only. It does not emit `STORE_V1_QEMU: PASS` or
`P3_STORE_QEMU: PASS`.** No production Store, recovery, ADR 0015, or NVMe code
was changed.

## Fault device semantics (`tests/common/mod.rs`)

`FaultStoreDevice` distinguishes volatile writes from durable bytes:
writes modify volatile state; `flush()` copies volatile to durable; simulated
power loss discards unflushed state; reopen sees only durable bytes. A torn write
(`tear = (nth_write, offset)`) writes only the prefix straight to durable media
and is not repairable by a later flush. `fail_write`/`fail_flush` return I/O
errors at a deterministic index. This is a test-only `StoreDevice`; plain
persistent RAM is not used as a stand-in for flush durability.

## Task 2 — checkpoint crash matrix (PASS)

Abort with power loss at each canonical checkpoint, discard volatile state,
reopen from durable bytes.

| Checkpoint | Recovered generation | Mount state |
|---|---|---|
| before_first_write | 2 (old) | Valid |
| after_payloads | 2 (old) | Valid |
| after_catalog | 2 (old) | Valid |
| after_commit_record | 2 (old) | Valid |
| after_first_flush | 2 (old) | Valid |
| after_superblock_write | 2 (old) | Valid |
| after_final_flush | 3 (new) | Valid |

Recovered generation is always N or N+1 and the root is self-consistent.

## Task 3 — torn-write matrix

Append units (payload 11 B, catalog 144 B, commit record 200 B): a tear that cuts
the semantic bytes leaves the previous committed generation recoverable
(generation N); a tear beyond the semantic bytes leaves the write effectively
complete and commits N+1. No mixed generation was observed.

Inactive Superblock characterization (NOT a pass; OLD-or-NEW not claimed):

| Tear offset | Outcome |
|---:|---|
| 1, 50 | generation 2 (byte prefix identical to old slot) |
| 100, 167, 168, 171 | **CorruptRecoveryRequired** (hazard) |
| 172, 200, 2048, 4095 | generation 3 (fields intact, trailing zeros) |

A torn inactive Superblock is CRC-invalid, and ADR 0015 classifies it
`Corrupt/RecoveryRequired` even though the active slot holds a valid previous
root. This makes the physical-atomicity dependency explicit. Recovery semantics
were not altered.

## Task 4 — atomicity precondition model (test-side, no production NVMe code)

`tests/store_atomicity_precondition.rs` models 0-based `AWUPF`/`NAWUPF`,
`NABSPF`/`NABO`, and a `write_is_atomic(start_lba, blocks)` predicate
(size <= effective unit AND no atomic-boundary crossing). Cases: 512-byte LBA
with insufficient unit rejects; 512-byte LBA with an 8-block-or-greater decoded
guarantee and safe boundary is eligible; sufficient size crossing a boundary
rejects; 4096-byte LBA with a one-block guarantee is eligible; namespace
guarantee overrides the controller; fallback to the controller when the
namespace guarantee does not apply.

## Tasks 5–7

- Task 5: a failed write, failed flush, or failed Superblock write poisons the
  live Store (`NeedsReopen`); the pending generation is never activated; reopen
  recovers the last commit.
- Task 6: `NoSpace` and `TransactionLimit` produce zero writes and zero flushes.
  (`CatalogFull` and `GenerationExhausted` zero-write cases are already asserted
  by the engine unit tests `store_catalogfull_zero_writes` and
  `store_generation_overflow_zero_writes`.)
- Task 7: after an uncommitted tail exists on media, reopen recovers the last
  committed generation; the tail is reused for a fresh commit without corrupting
  committed history.

## Gate

`cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets -D warnings`,
and `cargo test --workspace` all pass (`GATE: PASS`). 19 new tests.
