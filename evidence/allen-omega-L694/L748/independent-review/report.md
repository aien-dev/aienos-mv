# L753 independent review — L748 native Omega provider stream

**Verdict:** No correctness finding in the reviewed provider core or native stdin/stdout entry at `43eae848f0a44604b42378d9ea3170278b5f3f8f`. The focused independent probes passed. This is source/build/host-stream evidence only; it does not qualify a live model, network, clock, persistent storage, device, x86, or iMac path.

## Scope and source closure

- Reviewed exact head `43eae848f0a44604b42378d9ea3170278b5f3f8f` (base `be28deb21721f54fc46b5402076c7e8ac8fde151`), including `native/omega/allen/PROVIDER.md`, `provider-core.om`, `provider.om`, provider tests, and their application, scheduler, memory, context, reply, code, courier, runtime and base callees.
- Exported that commit with `git archive` to `~/handoffs/L753-review/source/`; all 51 tracked `native/omega/allen` paths exactly matched the L748 `source.sha256` membership and every one of its recorded hashes matched `git show <pin>:<path>`. The 51-file manifest is broader than the 43-file L747 review closure recorded in L748 summary.
- Pinned `omc` SHA-256: `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9`. Rebuilt with the exported `build-one.sh`; every binary is a nonempty static AArch64 ELF EXEC with no `INTERP`/`NEEDED` entries.
- Rebuilt provider SHA-256 `c9367d4cbc9261ebc0faaf3a99c2988ee36abda197670d868bd202a3bb19cccd`, exactly matching the L748 author receipt. Provider-core test, output verifier, process verifier, boundary test and custom probe artifact hashes are in `raw/builds-final.sha256` and `raw/builds.sha256`.
- The production worktree was clean at initial inspection. It had unrelated concurrent modifications by final inspection (`README.md`, `gate.sh`, and presentation files); I did not modify or build from that mutable tree. All compilation/replay used the immutable exported pin.

## Findings

None. Header fields are decoded little-endian as full 64-bit values; malformed magic/op/shape refuses before later command processing, while semantic refusal remains a framed status-0 response and permits following records. Restore staging gates BOOT, COPY/NOTE are bounded and duplicate visits refuse, offer output is copied to its own buffer without advancing transport completion, and the actual SENT result remains owned by the caller. Reply and completed-code records remain separately framed.

## Independent execution

All newly written behavioral probes and receipt checks were handwritten seeded Omega (`.om`). Shell was limited to pinned export/build, wiring real processes/descriptors and retaining logs/statuses.

| Probe | Result |
|---|---|
| Rebuilt provider-core test from exported callees and pinned source | 100 assertions; exit 0 |
| Rebuilt native stream entry + existing independent output verifier, normal valid stream | 292 assertions; exit 0 |
| Same valid stream through a real FIFO with Omega producer writes selected to one byte at a time | 292 assertions; producer/consumer exit 0; output byte-identical to regular-file replay (1114 bytes) |
| Independent `.om` parser/shape boundaries | 25 assertions; exit 0; included timestamp/op/argument/body-size bounds, 64-bit high-bit round-trip, invalid high-bit op, ASK 0/1/159/160, RECV 64/65/1200/1201, COPY/NOTE index edges, and zero-timestamp rules |
| Independent real process sequence BOOT, duplicate BOOT refusal, then CANCEL | 38 assertions; process exit 0; exactly three 40-byte replies with statuses 1, 0, 1 |
| Existing Omega process receipt verifier over six generated malformed/truncated inputs | 42 assertions; each process refused with exit 65; output was empty except the expected prior BOOT reply (40 bytes) before the malformed next frame |
| Clean EOF and true partial header/body EOF | Clean EOF exit 0 with empty output; 39/40-byte header and 1/2-byte body actually observed by `strace`, then EOF; each truncated process exit 65 |
| Short output write followed by write error | `RLIMIT_FSIZE`/ignored `SIGXFSZ` caused a real 101/111-byte positive write, followed by `EFBIG`; process exited 74 after a 1024-byte prefix. Independent Omega receipt/frame-prefix checker: 83 assertions, exit 0 |
| `/dev/full` output error | Process exit 74; no stderr |

**Count:** 872 passing Omega assertions across the listed test artifacts (100 + 292 + 292 + 25 + 38 + 42 + 83). This count excludes raw syscall observations and exit-only EOF/`/dev/full` checks.

## Raw evidence and replay inputs

- `raw/pins.txt`, `raw/compiler.sha256`, `raw/focused-inputs.sha256`, `raw/source-membership.diff`, `raw/git-source.paths`, and `raw/release-source.paths`: pin/compiler/source closure.
- `raw/provider.elf.txt`, `raw/builds.sha256`, `raw/builds-final.sha256`: ELF and compiled artifact identities.
- `raw/provider-core.log`, `raw/provider-normal.check.log`, `raw/provider-fragment.check.log`, `raw/provider-semantic-check.log`, `raw/provider-boundary-final.log`, `raw/short-*.receipt`, `raw/short-*.check.log`, and `raw/provider-partial-write-check-final.log`: native Omega assertion output and process receipts.
- `raw/short-3.strace`, `raw/short-4.strace`, `raw/provider-fragment.strace`, `raw/provider-partial-write.strace`, and associated input/output/status files: real syscall evidence.
- `raw/failed-probes/`: retained expected probe-construction failures and their statuses. Missing `main` was a compile/setup failure; the initial boundary count was an assertion-count error; a cancellation fixture with nonzero timestamp was malformed and correctly refused (65, with only the two prior 40-byte replies). They are not product failures and were not counted as kills.
- `source/` is the exact source archive export; `build/` contains focused inputs, Omega probes and outputs. No whole suite was rerun; L748's exact-source 2214 assertion/15 compiled-mutant receipt was inspected rather than duplicated.

## Limits / remaining qualification

- No live model/provider service, network or datagram transport, monotonic hardware clock, physical persistence, device, x86, or iMac execution was exercised. Host stdin/stdout framing is not those interfaces.
- The byte-at-a-time FIFO run alone did **not** prove short `read(2)` calls (the trace showed a complete 40-byte read); separate truncated-header/body EOF cases did prove positive partial reads. Short output write accounting was directly exercised as a positive short write followed by a subsequent error, not as successful retry/recovery.
- EINTR behavior and inherited SIGPIPE disposition were not injected. The entry maps syscall errors to 74; these signal/interruption cases remain separate qualification limits.
- No production files were changed; no deploy/restart/push or ledger action was performed.
