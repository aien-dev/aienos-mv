# L737 independent review — L734 native Omega application binding

## Verdict

**No substantive findings** in the reviewed native application/runtime/courier integration at `8d0d11babf7cc3b57ee000a265aeda9fe531ebf0` (base `0db871fdc1fabefa99012cbd3f6e8cd83fdfcf2e`). Scope is the host-executed, transport-independent Omega core only. This does not qualify a socket/provider binding, live service, iMac, EFI/device path, or full Allen replacement.

## Source and compiler receipts

- Fresh `git archive` export: `~/handoffs/L737-review/source`; all 39 files under `native/omega/allen` matched the exact pinned Git tree byte-for-byte; no missing or extra files. Manifest: `source-closure.sha256`.
- Application source SHA-256: `68f02db782e9c5a506e64972123129c9cc761c78ce6f39e40d2eb7c73394bcfe`.
- Parent application-test SHA-256: `f4e6bb5571a0700febdecfc6209ae1c088eab50a82bce4d8a10179408e3abb5f`.
- Pinned compiler SHA-256: `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9`.
- Both freshly built test artifacts passed `build-one.sh` checks for nonempty static AArch64 `EXEC`; `readelf` showed no dynamic section for the independent probe.

## Executed results

1. Rebuilt and ran exact-source `tests/application.om`: **exit 0; 182/182 checks passed**. ELF SHA-256 `ba8dd2746d9612599cc96b2d98c0b2e70a9230bfa0dbb3ec003fd08bdd756fec`; log `build/app-baseline.log`.
2. Authored and ran `probes/L737-interleave.om` (Omega only): **exit 0; 50/50 checks passed**. Binary SHA-256 `e792b6ad96152cf123b2caa46ce46bf501e5c5f2d8f036cf078e1b0eb1a64c32`; log `build/probe-final-50/probe.log`.

The independent probe exercised a received WAIT followed by a distinct LINE while its GOT was pending, verified transport backpressure leaves the first receipt pending, retried the LINE after GOT send, decoded the resulting GOT sequence, replayed an acknowledged LINE and verified duplicate receipt, cancelled an offered JOB and confirmed both counters were retained, then forced wire-ID exhaustion and checked request ID/transcript count remained unchanged. All expected state/output assertions passed.

## Files

- `report.md` — this review.
- `source/` and `source-closure.sha256` — exact source export and per-file closure manifest.
- `probes/L737-interleave.om` — independent Omega probe source.
- `build/` — assembled Omega inputs, logs and native artifacts, including the application rebuild and successful 50-check probe.

No production or compiler files were modified. No whole Allen suite, socket, service, agent, hardware or deployment work was run.
