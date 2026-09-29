# ARGUS-0 exit gates

Branch `feat/argus-0`. Code under test: commit `1a7d761` (integrator round 3, on top of
the ABI v1.1 merge `03b9f06` and the round-3 WIP `be2814d`). The only later commit adds
this file. Host: DGX Spark (gcc, aarch64, Linux). Cross-check: MacBook (Apple clang 17.0.0,
macOS 26.6.2), `make test` exit 0, same gate results.

All commands run from `native/argus/`. `make test` exits non-zero if any gate or any
expected-defended hostile test fails.

| # | Gate | Result | Command | Where measured |
|---|---|---|---|---|
| 1 | ARGUS_EVENT_ABI_PASS | PASS | `make test` | Spark, `1a7d761` |
| 2 | ARGUS_EVENT_SECRET_NEGATIVE_PASS | PASS | `make test` | Spark, `1a7d761` |
| 3 | ARGUS_EVENT_TRANSPORT_PASS | PASS | `make test` | Spark, `1a7d761` |
| 4 | ARGUS_TRANSPORT_SATURATION_PASS | PASS | `make test` | Spark, `1a7d761` |
| 5 | ARGUS_CORE_DETERMINISM_PASS | PASS | `make test` | Spark, `1a7d761` |
| 6 | ARGUS_HARD_INVARIANTS_PASS | PASS | `make test` | Spark, `1a7d761` |
| 7 | ARGUS_FALSE_POSITIVE_BASELINE_PASS | PASS | `make test` | Spark, `1a7d761` |
| 8 | ARGUS_RUNTIME_INTEGRATION_PASS | BLOCKED (awaiting lane H2 runtime numbers) | see below | omega `feat/argus-producer` |
| 9 | ARGUS_PERFORMANCE_GATE_PASS | PARTIAL (substrate numbers below; runtime numbers pending) | `make bench` + see below | Spark, `1a7d761`; omega `feat/argus-producer` |
| 10 | ARGUS_HOSTILE_REVIEW_PASS | PASS with documented limits (ABI v1) | `make test` (hostile part) | Spark + Mac, `1a7d761` |

Also on `1a7d761`: `make sanitize` exit 0 on the Spark (integration under ASan+UBSan and
TSan, hostile under ASan+UBSan, ring under TSan, detectors under ASan+UBSan; the lanes'
detector test also runs under UBSan inside `make test`). `GATE ARGUS_MEMORY PASS` is an
extra gate (not one of the ten) and is listed at the end.

## Evidence lines (verbatim, Spark, `make test` at `1a7d761`)

1. ARGUS_EVENT_ABI_PASS

```
GATE ARGUS_EVENT_ABI_PASS PASS known-answer 4/4 (event 1be29450.., chain 09ea598e.., chain2 21b19c9b.., finding b56e133c..); round trip encode->decode->re-encode byte-identical + digest==sha256(bytes): benign 5000/5000, hostile 5000/5000, bad 0; v2 byte rejected=1
```

Also: `fuzz_argus_event: 1000000 iterations, 441525 accepted (all re-encoded byte-identical), 558475 rejected, every result matched the byte oracle, 0 violations`.

2. ARGUS_EVENT_SECRET_NEGATIVE_PASS

```
GATE ARGUS_EVENT_SECRET_NEGATIVE_PASS PASS 29 files in 3 dirs under native/argus (recursive incl. docs/ tests/, out/ excluded), 0 hits, scan errors 0; static: 15 event fields != 32 bytes, only machine_id/evidence_digest are 32, field sum == 128
```

(After this file is added the scan covers 30 files; see the final check at the end.)

3. ARGUS_EVENT_TRANSPORT_PASS

```
GATE ARGUS_EVENT_TRANSPORT_PASS PASS hostile seed 1, ring 1024. fast consumer (producer keeps depth < 256): attempts 5000 = pushed 5000 + refused 0 (SEC 0 AUD 0 INF 0, crit_overflow 0); popped 5000; drop events 0 reporting 0 lost; TELEMETRY_LOSS findings 0/0 expected; nothing refused=1. burst (unthrottled producer): attempts 5000 = pushed 1046 + refused 3954 (SEC 1252 AUD 2058 INF 0, crit_overflow 644); popped 1046; drop events 6 reporting 3954 lost; TELEMETRY_LOSS findings 4/4 expected; identity=1
```

(The burst counts depend on thread timing and vary run to run; the identities checked do not.)

4. ARGUS_TRANSPORT_SATURATION_PASS

```
GATE ARGUS_TRANSPORT_SATURATION_PASS PASS 200000 attempts (5 INF 0x7FFF : 2 SEC CAPABILITY_DENIED : 1 CRIT LEASE_REVOKED), ring 1024, consumer stalled to 100% then 8/20us; limits INF 512 SEC 922 CRIT 1024; max depth seen 1024; refused INF 124680 SEC 49598 CRIT(overflow) 24514; first refusal idx INF 513 SEC 1614 CRIT 2424; refused below limit INF/SEC/CRIT 0/0/0; SEC refused while INF accepting 0; CRIT refused below 100% 0; drop events 72 (CRIT-class 24 SEC-class 24 INF-class 24) -> TELEMETRY_LOSS 48/48 expected, wrong sev 0; INF (non-v1 kind) delivered -> MALFORMED_EVENT 320/320 expected; ring malformed refusals 0; other findings 0 (flood pre-check 0)
```

5. ARGUS_CORE_DETERMINISM_PASS

```
GATE ARGUS_CORE_DETERMINISM_PASS PASS hostile seed 1 (5000 events): direct vs ring path (0 refused=1): findings 100 vs 100 byte-identical=1, state digest equal=1, chain equal=1; third fresh direct core identical=1; state 70e0106d197df3f0.. chain b0297a7f57d1ac7b.. findings-bytes sha256 216396f985fa7a29..
```

Also: `test_argus_determinism: 13 passed, 0 failed` (lockstep, interleaved, byte-copy split, 1-slot runs).

6. ARGUS_HARD_INVARIANTS_PASS

```
GATE ARGUS_HARD_INVARIANTS_PASS PASS real core + real detectors, hostile seed 1: code detected/expected [1:15/15 2:8/8 3:5/5 4:4/4 5:9/9 6:12/12 7:12/12 8:8/8 9:4/4 10:8/8 13:0/0 14:8/8 15:7/7 16:0/0]; all 100 injected found=1; unexpected hard (1..16 except 11/12) 0; code 12 findings 0 (corpus sequences strictly increasing per (machine_id,stream,consumer) stream: yes, 23 streams); code 11 findings 0; ingest rc FULL 0 OVERFLOW 0
```

Also (detector unit corpus): `hostile v1.1 injections: summaries -> code 1:1 2:4 3:2; cap 0 wrong subject -> 14:4`.

7. ARGUS_FALSE_POSITIVE_BASELINE_PASS

```
GATE ARGUS_FALSE_POSITIVE_BASELINE_PASS PASS benign seeds 1..9, 45000 events through real core: findings per seed [0,0,0,0,0,0,0,0,0], total 0 (code 11: 0, code 12: 0; sequences strictly increasing per stream: yes); ingest rc FULL 0
```

8. ARGUS_RUNTIME_INTEGRATION_PASS: BLOCKED (awaiting lane H2 numbers)

Measured in omega `feat/argus-producer` (lane H/H2), see `~/workspace/argus-runtime-streams`
and the handoff; numbers to be pasted by the orchestrator.

| Item | Value | Commit | Command |
|---|---|---|---|
| Producer commit (omega) | _to be pasted_ | | |
| ARGUS substrate commit consumed | _to be pasted_ | | |
| Real runtime streams replayed (R7, R8) | _to be pasted_ | | |
| Findings on benign runtime streams | _to be pasted_ | | |
| Verdict | _to be pasted_ | | |

9. ARGUS_PERFORMANCE_GATE_PASS: PARTIAL

Substrate numbers, Spark, `make bench` at `1a7d761` (under `flock ~/workspace/.argus-bench.lock`,
`.spark-quiet` absent):

```
push, single thread (SECURITY)           p50   4.00 ns  p99   5.50 ns  p99.9    6.50 ns  (per push, 32-push batches)
push+pop_batch, single thread bulk: 17.35 ns per event (20000000 events)
push, live consumer thread (CRITICAL)    p50  29.00 ns  p99  36.00 ns  p99.9   47.50 ns  (per push, 32-push batches)
two threads bulk: 60.48 ns per push attempt, 16.5 M pops/s, accepted 26388544, refused (full) 11456, popped total 26388544
INGEST ns/event (benign seed 1, 5000 events x 200 fresh cores, real core + detectors + SHA-256): p50 1056 p99 1232 p99.9 2016 max 32368; mean without per-event timers 1056.6; timer overhead p50 16 ns (included in p50/p99)
```

Ingest is bound by SHA-256 (event digest + chain). The runtime side (producer cost on
the hot path, R8 wall-clock overhead) is measured in omega `feat/argus-producer`
(lane H/H2), see `~/workspace/argus-runtime-streams` and the handoff; numbers to be
pasted by the orchestrator.

| Item | Value | Commit | Command |
|---|---|---|---|
| Producer hot-path cost per validate | _to be pasted_ | | |
| R8 wall clock, ARGUS off vs on | _to be pasted_ | | |
| Budget from the brief | _to be pasted_ | | |
| Verdict | _to be pasted_ | | |

10. ARGUS_HOSTILE_REVIEW_PASS: PASS with documented limits (ABI v1)

There is no single GATE line; the evidence is the hostile test summary (Spark, `make test`
and `make sanitize` under ASan+UBSan; Mac `make test`, all identical):

```
test_argus_hostile: 58 defended, 3 expected-fail (open/weak), 2 N/A-v1 (documented, exempt), 0 xpass, 0 unexpected failures
```

Section verdicts (`docs/HOSTILE_REVIEW.md`, round 3, 26 sections): DEFENDED 20, WEAK 4,
OPEN 1, N/A-v1 1.

Remaining open list (all ABI v1 limits; details in HOSTILE_REVIEW.md "Remaining open
after round 3"):
- G-5 (HIGH, OPEN): no producer identity; any code that can push can announce a grant
  that launders its own later use (EXPECTED-FAIL `unattributed_grant_masks_forged_use`).
  Producer attestation is ARGUS-3.
- G-21 under G-5 (MEDIUM): a byte replay re-keyed with another machine_id escapes the
  replay check (EXPECTED-FAIL `replay_evades_by_rekeying_stream`, ruling c).
- G-9 (LOW, WEAK): a quarantined provider under a new digest is clean
  (EXPECTED-FAIL `provider_substitution_escapes_quarantine`).
- G-21/G-8/G-20 (LOW, WEAK, bounded): full tables stop mirroring new entries; loud once
  per table, then counted (ruling a).
- G-12 (LOW, WEAK): two opaque 32-byte slots could carry a secret; producer-side guard.
- G-17 (LOW, WEAK): the ring is single-producer by contract only.
- G-6 (N/A-v1): sequence gaps are not findings (`sequence_gap_unflagged`, ruling c).
- G-7 (N/A-v1): a producer may self-label low-value kinds CRITICAL
  (`critical_self_label_starves_security`).

Integrator judgement: **PASS with documented limits**, because every attack ARGUS-0 can
defend within ABI v1 is defended with zero unexpected failures, and everything still open
comes from one root (ABI v1 trusts the producers inside the process: no producer or
provider identity), so ARGUS-0 must not be described as defending against a malicious
producer inside the trusted boundary.

## Extra gate

```
GATE ARGUS_MEMORY PASS argus_core_footprint 60296 B; ring(1024) footprint 131456 B (128 B/event slot); 5 objects checked with nm -u, forbidden imports 0 (allowed: argus_/sha256_ cross-calls, mem*, stack protector, aarch64 outline atomics); imports seen: sha256_final sha256_hash sha256_init sha256_update __stack_chk_fail __stack_chk_guard __aarch64_ldadd8_relax __aarch64_swp8_relax argus_event_min_class argus_chain_extend argus_detect_run argus_event_digest argus_event_validate memcmp memcpy memmove memset
```

## Final check after adding this file

`make test` on the Spark at the commit that adds this file: see the commit message
(secret-negative scan covers 30 files).
