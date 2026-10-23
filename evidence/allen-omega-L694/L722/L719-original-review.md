# L719 independent review — L694 native Allen milestone

**Reviewed source:** `8049f5ddbfb3284498e54ed582768a399dcc99d5` (baseline `da55660ea7337f2b6a42095fcd1fb6ddebe1cc7c`). Exported the source/docs and committed evidence into private `~/handoffs/L719-review`; production/source/worktree and ledgers were not modified. Pinned compiler SHA-256 verified as `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9`.

## Finding

**Medium — oversized stdio command lines are silently discarded instead of refused.** `native/omega/allen/main.om:135-140` sets `Lstate[1]` when the 8192-byte line buffer fills, but on newline `main.om:123-133` takes the `else` branch, clears the overflow flag and line length, and continues without reporting an error or terminating. This contradicts the stated whole-operation overflow refusal (`native/omega/allen/README.md:66-70`) at the actual standalone integration boundary.

**Reproduction:** compiled the unmodified concatenated Omega core + `main.om` using the pinned compiler via `build-one.sh`; nonempty static AArch64 ELF check passed. Fed `ASK ` + 8189 `x` bytes (8193-byte line, one byte beyond the 8192-byte line buffer) + newline, then `ASK hi\n`. The process exited `0`, stderr was empty, and stdout was 473 bytes containing a `JOB 1 chat ...` packet ending with `the person said something in passing.\thi\n`. Thus the oversized first line was silently skipped and the next command was processed; the caller receives no refusal/error for the dropped command. Probe ELF SHA-256: `c22923a20f4bd9f30a3bc8e51544b10533a7c0a20bc65856a107a163362312e8`; stdout SHA-256: `52777ab25b070876095242a799846ece3098d472751e98e33c0eac18ffaa9477`; stderr SHA-256: `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`. Probe artifacts are under `~/handoffs/L719-review/probe-output/`.

## Evidence checked / limits

- `evidence/allen-omega-L694/CLOSURE.md` says the exact immutable replay compiled 13 Omega test/render/fixture programs plus standalone Allen, ran 879 checks, compared the 4096-byte memory golden, and exercised five compiled behavioral mutants. The committed per-test logs have the claimed line counts (runtime 28, CODE 23, courier 24, memory 79, edges 352, short 20, long 292); `final-replay.log` records the same mutant exits and `target=NOT_RUN; live=NOT_RUN`.
- The gate does not test stdio overflow: `gate.sh:33-37` exercises only the canned `stdio-input.txt` and compares its two golden outputs. `main.om` is separately compiled and run in that path, but has no direct oversized-line case in the committed test sources.
- Known missing iMac/provider/UI/scene parity and non-qualification are explicitly disclosed in the README/design/platform gates and are not reported as newly discovered defects. I did not rerun the parent-owned broad gate or alter any production files.

**Disposition:** one concrete boundary defect reproduced; no conclusion of full Allen qualification. Target, live attachment, iMac/platform parity, and the broader strict suite remain outside this review's verified scope (the saved closure separately records four sandbox-denied socket tests in the strict Rust suite).
