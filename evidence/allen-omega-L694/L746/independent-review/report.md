# L747 independent review — L746 restored-context / JOB binding

## Verdict

**No findings** in the scoped native Omega implementation at `be28deb21721f54fc46b5402076c7e8ac8fde151`, compared with parent `a65b70eb99a4d132c8c1f657f5cbb2a2fc1cd81d`.

Scope reviewed: `native/omega/allen/context.om`, its `tests/context.om` integration coverage, `application.om`'s `appask -> jobbuild -> ctxappend -> qsubmit` path, and the related gate/README changes. This review does not repeat L737/L744 or the full 1479-check suite and makes no disk, live-model, service, target, or hardware qualification claim.

## Independent source/build/probe receipts

- Source repository: Spark `~/workspace/aienos-allen-omega-L694`; exact commit resolved to `be28deb21721f54fc46b5402076c7e8ac8fde151` (repository HEAD matched); working tree was clean at inspection.
- Private exact-source export: `~/handoffs/L747-review/native/omega/allen/` (Git archive of the pinned directory; 43 files). Export/member and SHA receipts: `logs/export-members.txt`, `logs/scoped-source.sha256`, `logs/pin-source.sha256`.
- Compiler: `~/handoffs/allen-omega-L694/omc`, SHA-256 `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9` (`logs/compiler.sha256`). The exported `build-one.sh` checked this identity before build.
- Independent Omega probe: `probes/context-independent.om`; assembled exact-source input: `probes/context-independent.full.om`; build command used the exported `native/omega/allen/build-one.sh` into fresh `build/context-independent`. The compile returned success and produced a nonempty static AArch64 ELF64 EXEC, SHA-256 `4a53ec429504131fd48a6f2931a7244053ab9013291d46841dc69545addcabd6` (`logs/elf.txt`, `logs/probe.sha256`). No `INTERP`/`NEEDED` entries.
- Execution: `build/context-independent > logs/context-independent-v2.log`; exit status 0; 150 emitted checks, all `0` (all passed). This runs the Omega context and application test body against the freshly built native executable.
- The probe adds direct refusal-state snapshots around `appask` with an incomplete restored catalog. It asserts R/history, Q/session and sequence, pending `T8` job length and `JobOut` bytes, and outgoing `Outb`/length/error remain unchanged. The inherited context integration also runs real `appask`/`qsubmit`, checks the appended context at the tail of the queued `JobOut`, verifies deleted/damaged notes are omitted, and exercises whole-job overflow refusal.
- Source diff hygiene: `git diff --check <parent> <pin> -- <scoped files>` returned 0 (`logs/diff-check.log`).

## Probe iteration note

The first probe revision (`logs/context-independent.failed-v1.log`, `probes/context-independent.failed-v1.full.om`) reported one failed assertion: it incorrectly expected `Q[0] == 0` after `appinit`; `qstart` correctly initializes `Q[0]` to run/session `123`. This was a probe expectation error, not a product failure. The corrected probe snapshots both Q fields before `appask` and compares against that state. Its rebuilt run passed all 150 checks. No production source was edited.

## Boundary disposition

Inspected the context catalog lifecycle (declared-count cap, loading/ready/visited/damaged state, duplicate/out-of-range refusal, deleted/damaged-note omission, and transaction exclusion), metadata staging/field validation/4096-byte append boundary, and the provider-facing `appask` ordering. The implementation validates restored context before variable-layout identity reads; stages metadata privately; appends before `qsubmit`; and rejects an incomplete catalog or oversized complete job before queue/history admission. No scoped correctness defect was reproduced.

SHA integrity remains unkeyed and does not establish authentication or freshness. This is source review plus host-executed native Omega boundary evidence only.

## Raw receipts

All retained under `~/handoffs/L747-review/` on Spark:

- `logs/scoped.diff`, `logs/diff-check.log`
- `logs/build-v2.log`, `logs/context-independent-v2.log`, `logs/probe.sha256`, `logs/elf.txt`
- failed probe revision/log: `probes/context-independent.failed-v1.full.om`, `logs/context-independent.failed-v1.log`
- corrected probe: `probes/context-independent.om`, `probes/context-independent.full.om`
