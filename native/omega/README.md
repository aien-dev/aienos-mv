# Seed-native Omega GSP CPU sequencer — offline milestone

`gspseq.om` is a personally handwritten port of the CPU-side GSP sequencer in `native/kernel/gpu/gsp_seq.c`. It is compiled directly by the qualified seed-built Omega compiler into static AArch64 ELF; no C compiler, transpiler, generated C, C linker or C runtime constructs or executes that program.

**This is an offline register-backend implementation, not a loaded driver or live NVIDIA replacement.** The existing boot tool still calls the C sequencer. Its migration requires a native MMIO/backend attachment and whole-boot qualification; this source does not satisfy those gates. No live module/device/firmware/service changes occur in this milestone.

## Contract

One little-endian u32 frame on stdin, in order:

1. `nwords`, `cmdIndex`, `pollBudget`, `coreMode`, `delayEnabled`;
2. eight initial register values, eight initial saved values;
3. exactly `nwords` command words.

Bounds: 1024 command words, poll budget <=1024, coreMode 0..2, delayEnabled 0..1. Maximum input 4180 bytes; one extra byte detects overflow. `cmdIndex` must be strictly below `nwords`, even for an empty execution. Payload sizes/opcodes follow the pinned reference: WRITE=0/2 words, MODIFY=1/3, POLL=2/5, DELAY=3/1, STORE=4/2, CORE_RESET/START/WAIT/RESUME=5..8/0. Unknown/short instructions and save indices >=8 refuse.

The backend is deliberately only an eight-register array: out-of-map reads return zero and writes are ignored, as in the reference unit-test model; accesses still increment their counters. **Those addresses are simulation slots, not a hardware register allowlist.** Core mode 0 is absent/refused, 1 simulates successful callbacks, 2 simulates failed callbacks. Core instructions never reset/start/wait/resume hardware. Delays are accumulated modulo 2^32, not slept. Initial poll read plus max(budget,1) retries matches the actual reference, including its zero-budget behavior. Read values are static unless a prior WRITE/MODIFY changed the simulated register; no asynchronous hardware transition is modeled.

Output is exactly 21 little-endian u32 words (84 bytes): result status, read count, write count, delay total, core callback count, registers[8], saved[8]. Exit 0 succeeds, 1 reports sequencer refusal and still exposes prior simulated effects, matching the nontransactional C core. Exit 2 rejects malformed envelope, capacity or I/O. Input is read to EOF before execution; read/capacity failure emits no output. Output error can leave a prefix. EINTR refuses; successful transient retries are not qualified. Each process handles one frame; repeated in-process calls are not an exposed API.

This bounded value-based test interface avoids foreign function-pointer callbacks. It is not a proposed final hardware ABI. Agent-facing improvements are authorized, but changed external semantics need their own discriminating gates; byte/state parity here is the baseline, not a requirement to preserve every C design forever.

## Build and qualification

From the repository root on AArch64 Linux, with the qualified Omega checkout:

```sh
python3 native/omega/tests/check.py "$PWD" "$OMEGA_ROOT"
python3 native/omega/tests/mutants.py "$PWD" "$OMEGA_ROOT"
python3 native/omega/tests/check.py "$PWD" "$OMEGA_ROOT" # restore/rebuild the original
python3 native/omega/tests/io.py "$PWD"
```

`check.py` checks compiler identity and static AArch64 ELF shape, compiles the handwritten source once, and compares all result bytes/statuses against a separately ASan/UBSan-instrumented **external C oracle**. Neither the Python harness nor the C oracle is linked/called by the native program. Build output: `build/omega-sequencer/gspseq`.

- Compiler SHA256: `01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6`.
- Compiler reconstruction source: omega-mv `9b072eb492e106d23f7eed984c5645251601adee`, `seed/omega-from-seed.sh`.
- C reference origin: aienos `2b47925d45171c2399e2617b62b85e6033e7747a`.
- Exact C/header hashes are checked before oracle compilation; source/binary identities are recorded in JSON receipts.

214 executed differential cases cover each opcode and core mode, truncations, save bounds, unknown opcodes, post-write refusal, disabled/wrapping delays, zero/max polling budgets, out-of-map simulation slots, capacity and 150 deterministic random sequences. Stub RED: 208 failures. Initial implementation failed 155 cases because byte serialization narrowed unmasked u32 values; Omega trapped instead of C-style truncation. The fix explicitly masks each byte. That failure receipt is retained. Final GREEN: zero failures.

Four compiled mutants are caught: accept invalid save indices (7 cases), wrong modify mask (26), accept disabled core (30), zero poll budget drops retry (5). Wolfram independently validates modify/delay values, poll read counts and frame dimensions against the actual native results. Three real-kernel I/O controls cover closed input, EAGAIN after a complete frame without EOF, and `/dev/full` output refusal.

Evidence is under `evidence/omega-sequencer/`. The existing Linux shim/module/tools and both plain/sanitized boot simulations are regression checks of the unchanged C path, **not native Omega boot integration**. Clean-source replay reconstructs the compiler and reruns the frozen port separately. Independent source review of this new port is not yet claimed.
