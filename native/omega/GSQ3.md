# GSQ3: seeded sequencer with attached register policy

This candidate connects the previously separate native sequencer and register policy **inside one Omega executable**. `regcore.om` is the shared source for `gsq3.om` and the existing RGP1 `regpolicy.om` adapter. Source files are concatenated byte-for-byte with one LF between them, core first, and passed to the pinned seeded compiler. There is no C translation, linked foreign runtime, Python subprocess or RGP1 protocol bridge inside the executable. Python/shell remain external build/test orchestration; Linux supplies stdin/stdout syscalls.

GSQ3 replaces the eight numeric register slots with actual pinned BAR0 offsets mapped to14 simulated registers. The first13 slots follow the C shim's read-list order; slot13 is write-only SEC2 EMEMD0 (0x00840ac4). Both native consumers use the same policy functions and lists; the duplicate policy implementation was removed from the RGP1 adapter. GSQ2 remains unchanged for historical compatibility and regression, not as a hardware backend.

## Contract

Input: bytes `GSQ3`, then LE32 `nwords, end, pollBudget, coreMode, delayEnabled, allow_boot, initial_doorbells`, then14 initial register values,8 save values, then nwords instruction words. Maximum1024 words,4216 input bytes; exact length required. Budget<=1024, coreMode0..2, delayEnabled/allow_boot0..1. Initial latch accepts any u32. Execution covers the word prefix before end; end must be less than nwords, retaining GSQ2's sentinel/range convention.

Output:28 LE32 values (112bytes): `status, reads, writes, delayTotal, coreCalls, final_doorbells, regs[14], save[8]`. Process status equals output status:0 success,1 structural/runtime sequencer failure,3 register-policy refusal. Invalid envelope/I/O exits2; malformed input produces no successful state output, and output I/O failure can leave a prefix. EOF is required before processing; no implicit multi-frame session.

1. Validate the entire selected range structurally, including save indices and required core callback presence.
2. Validate explicit register accesses in program order, using a shadow doorbell latch. Modify requires both read and write permission; poll/store require read; write requires write. Unknown/misaligned/not-listed addresses are refused. Two SEC2 doorbell writes are refused before either executes. Structural refusal takes precedence over permission refusal.
3. Restore the original latch regardless of policy verdict. On structural/policy preflight failure, registers/save/latch remain unchanged and operation counters remain zero.
4. Execute only a permitted stream, checking permissions again at every explicit register read/write. Commit the simulated latch only when the corresponding write executes. A poll timeout or simulated core failure can retain earlier effects; these are not rolled back. A future write after a timeout does not consume the latch merely because preflight saw it.

Permissions apply to explicit script register operations. Core callbacks remain abstract simulated success/failure counters, with no real internal register operations. This does NOT establish safety of real reset/start callbacks or firmware command streams. Register values are ordinary storage: no W1C behavior, asynchronous progress, read side effects or real device ordering is emulated. Shared policy globals support this single-process/single-frame adapter, not concurrent device instances.

## Gates and provenance

Original inputs are frozen at aienux f3a00467438e92bc6f5e5e2059ccbdad22859932. Shared policy function bodies are reused unchanged from native `regpolicy.om`. Sequencer size/poll/step/structural-validation bodies are reused unchanged from native `gspseq.om`; `gsq3-reuse.json` records their identities. No vendor C was converted into source. The pinned C shim supplies the external oracle's allowed address lists; the test independently models sequencing, policy precedence, shadow-latch planning and effects in Python. That is not execution of the kernel module or a formal proof of all inputs.

193 integration cases pass, including all allowed register mappings, write-only modify refusal, read/write/poll/store sequences, zero-effect late denial, repeated doorbell, permission switches, consumed/max latch, timeout before/after doorbell, structural precedence, all core opcodes, poll budgets, capacity and framing. The original190-case stub run had184 failures; three interaction fixtures were subsequently added without replacing any original case. Five compiled integration mutants are caught. Shared-core RGP1 regression retains32frames/3332requests and four compiled mutants. Both new consumers have three real I/O refusal controls with valid-frame positive controls. Wolfram independently checks framing, arithmetic and four native state/counter cases.

An initial build attempt failed at `fn inc slot` because the new function named `slot` conflicted with that parameter; it was renamed `locate`. This compiler refusal was not counted as a caught mutant or a behavioral RED.

```sh
python3 native/omega/tests/gsq3.py "$PWD" "$OMEGA_ROOT"
python3 native/omega/tests/gsq3-mutants.py "$PWD" "$OMEGA_ROOT"
python3 native/omega/tests/gsq3.py "$PWD" "$OMEGA_ROOT"
python3 native/omega/tests/gsq3-io.py "$PWD"
python3 native/omega/tests/gsq3-wolfram.py evidence/omega-sequencer
```

Compiler pin:01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6. Harnesses require successful compilation and a static AArch64 ELF before execution. Mutation runs are serial and restore both sources; source/binary identities are recorded. Historical receipts are retained, and new shared-source receipts use `shared-policy-*` names.

## Still unqualified

This is address-aware simulation, **not MMIO or a kernel module**. The live C shim and NVIDIA modules remain untouched. Real device mapping, kernel ABI/loading, locking, callback implementations, firmware behavior, rollback and hardware qualification remain required before replacement. Source review is separate from the author-run gates and fresh reconstruction. No deployment authorization follows from these receipts.
