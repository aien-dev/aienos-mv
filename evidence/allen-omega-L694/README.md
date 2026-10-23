# L694 native acceptance (WIP)

All execution is Linux AArch64 host execution, no QEMU/iMac/physical/live claim.
Frozen behavioral source is baseline da55660ea7337f2b6a42095fcd1fb6ddebe1cc7c.
Native product/test sources: native/omega/allen. `accepted/` is replay5 history;
final immutable-source replay is recorded by the later closure receipt.

Current implemented acceptance: runtime28, CODE23, courier24, memory/notes79,
intent8, reply15, code-view13, scene14, paint11, boundary/recovery352,
legacy short fixture20 and long fixture292 =879 checks, zero failed.
Five valid compiled mutants fail original gates: stale-ID3, CRC2, SHA2,
readback7 and session1 failed assertions, all native exits1..99.
Independent full4096-byte memory golden comparison passes. Native render produces
mind-component.ppm with the actual Omega pixel loop, not a fabricated screenshot.
Standalone stdio code exchange outputs JOB and immediate acknowledgement/fixture
completion. Input is canned **test data**, never model output or live attachment.

Exact commands:

```sh
quiet-build native/omega/allen/gate.sh target/allen-omega-L694/new-empty-output
quiet-build env AIENOS_STRICT=1 ./scripts/verify_all.sh
quiet-build cargo clippy --offline --all-targets -- -D warnings
```

Repository strict suite: FAIL exit101. Formatting passed; four existing
allen-courier socket tests hit OS EPERM. verify-all.log preserves full output.
Clippy is separately recorded in clippy.log. No assertion was skipped or waived.
Legacy suite is compatibility evidence only, not an Omega acceptance backend.

Fixtures short.wire/long.wire were emitted by the unchanged existing
`target/release/allen-courier --split native/kernel/svc/tests/fixtures/code/{short,long}.txt`
after `quiet-build cargo build --offline -q --release -p allen-courier --bin allen-courier`.
Its implementation is a fixture oracle, not an execution-closure dependency.
Full code CRC32768 x bytes is independently frozen from GNU gzip trailer:
CRC ba3e1bc1; native boundary test checks acceptance, then CRC corruption refusal.
Memory reference is independently assembled from layout with dd/printf, sealed
using sha256sum and xxd, then compared byte-for-byte. No new C/Rust/Python/JS
program or test backend exists. Shell merely constructs references/builds/runs.

Original failed receipts and exact behavioral RED sources remain here. See
[development-failures.md](development-failures.md). Receipt replay2 is invalid;
only corrected later replays count. Source/compiler/executable hashes and native
per-check outputs are retained. Output binaries live under target/allen-omega-L694
and the permitted handoff tree, not in Git. Target/platform gates and remaining
portable compatibility are listed in native/omega/allen/README.md.
