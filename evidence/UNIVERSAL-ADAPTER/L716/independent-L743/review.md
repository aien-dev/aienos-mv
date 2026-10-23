# L743 independent review — frozen hosted Omega services

## Verdict

No actionable defect found in the frozen native Omega `services.om` or `math-stream.om` at source commit `f91754159061b387b85328a570fd8c1b1bd32043` (review base `8c9e5730a21c4a1dbfbf962c086da7b7f65a6422`). Reviewed the exact committed blobs, not the subsequently edited worktree. Findings: **none** in the bounded review scope. This is not integration, hardware, merge, or whole-issue approval.

## Exact pins and closure

- Repository: `/home/drakestapleton/workspace/hermes-desk/ua-services-L716`, repo `aien-dev/aienux`.
- Source: `f91754159061b387b85328a570fd8c1b1bd32043`; parent `8c9e5730a21c4a1dbfbf962c086da7b7f65a6422`.
- Evidence pin supplied by the brief: `78c7e7385b50460c231347fc061289e4529c517a`.
- Omega compiler `/home/drakestapleton/handoffs/universal-adapter-L707/shared/omc`: SHA-256 `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9`.
- Public math core `.../L716/source/omega/seed/math/core.om`: SHA-256 `b70391a7a38464cd63ff796748565baf3dc2f73afaed90cbc2b94774dba02db6`.
- Public math CLI `.../L716/source/omega/seed/math/cli.om`: SHA-256 `145db7e5184efbc5076ce27e93ddc7822e3f509c793f04f2413e0fd29f3343d0`.
- Frozen `services.om` SHA-256 `92282ea925fc2f06280e2704a58df8de31ebd1d189fcced138b5f43c0ea6b94e`; `math-stream.om` SHA-256 `ebf453cd7250261f29bf91c173c0c7553a6d9ad3bdb439ca491c85d6ab6c58f5`.
- Audited the 16-entry source tree against evidence manifest SHA-256 `03da469c54deaf1f76aa5997870b43fd17e5e19e1f678e293ac468dd22915983`; every listed file's bytes matched the source commit. Raw manifest: `L743-review/raw/pinned-source-tree.sha256`.

## Independent probes and receipts

All newly authored test logic was Omega (`L743-review/sources/offset-refusal.om`, SHA-256 `e489a7f1e8568e2adcbec3bec269e96981e68cc6653961f684cd1aef37896cc8`). No product source was changed.

1. **Facade nonzero offsets and refusal side effects.** Compiled the exact pinned `services.om` concatenated with the Omega probe using the pinned compiler; compile exit 0, execution exit 0, AArch64 ELF64 EXEC. Input `ABC`; expected and observed stdout `YZ`. The probe checked wrong-handle refusal left the buffer unchanged and did not consume stdin, read at offsets 2 then 1 into the requested locations, invalid zero-length range refusal, and exact write offset selection. Binary SHA-256 `a07a14f2c092d6be7dbc7cc7bdd47d3eab026d43a7c06fc685092ceacd264543`; stdout SHA-256 `b7198772da96fcef4ced89a98012153597aa661d40e8cb2d38982f57af7b299b`. Full receipts/inputs: `L743-review/raw/offset-run/`.

2. **Frozen application behavioral replay.** Built exact public CLI reference (`core.om + cli.om`) and the frozen driver (`core.om + services.om + math-stream.om`) with the pinned compiler. Both compile exits 0. Driver binary SHA-256 `01d1f5be0c653f7f281d3fce7831ebc122f0caaf30db82c7e1baf9c3696871a0`, matching the independently supplied hosted replay receipt; reference binary SHA-256 `aa7906f534ffb63575f5b59be15e3ed93a02fe56d103aa35298beed5a35d7ac6`.
   - CRLF input, malformed arithmetic, negative rational and partial final frame without LF: both exit 0 and byte-identical output (30 bytes; SHA-256 `7fa81cdb3d61f80d6a51d7ad1e01d7d83818a2b425da095c09ad2e5e81b7f00e`).
   - Exact 4096-byte request followed by a valid request: both exit 0 and output byte-identical `REFUSED 4\nOK 4\n` (15 bytes; SHA-256 `340292da10a6198dab15474f4858af28fddc6d0fb1948039e17fdb5f80d092d4`). This confirms refusal recovery at the core capacity boundary.
   - 65,536-byte unterminated transport input: driver exits 1 and emits 15 bytes before bounded transport refusal, as specified by the frozen driver status/transport bound.
   - Raw source files, commands' intermediate inputs/outputs, statuses and hashes: `L743-review/raw/math-stream-run/`.

Existing evidence at pin `78c7e...` also reports exact-source Spark and Pi5 hosted replay exit 0, unchanged math suites and two compiled behavioral mutants; I did not repeat those broad suites. Evidence also states native Aienux `NOT_RUN` and no timing run.

## Limitations / declined-to-judge

- The frozen source tree includes a shell runner and Python short-write descriptor helper; the brief assigns their removal to the parent concurrently. I did not assess or modify these parent-owned harness files as product findings. Current worktree showed parent-owned documentation edits and staged deletions of those helpers after the source pin; all review/build inputs above were taken from Git objects.
- I did not force a read error after partial progress (for example nonblocking-pipe EAGAIN), EINTR, successful partial-write retry, or signal behavior. Existing receipts cover short-write progress then failure, closed-input errors, EOF, and `/dev/full`; more extensive syscall-fault behavior remains unqualified here.
- No claim about native Aienux, Pi5 independent execution by this reviewer, FFI/installation/integration, live services, performance, GPU, kernel, or whole-issue acceptance.
