# Seed-native Omega GSP CPU sequencer, version 2

`gspseq.om` is a personally handwritten seed-native implementation of the CPU-side GSP sequencer. It compiles directly to static AArch64 ELF without a C compiler, generated C, linker or C runtime in its construction/execution path.

**Offline only:** its register backend is an eight-slot array, not MMIO. The boot caller still uses `native/kernel/gpu/gsp_seq.c`. No module loading, firmware loading/reset, real register operations, live service changes or NVIDIA-driver retirement are authorized or claimed by these receipts.

## Safety improvement

An effect-free pass validates the entire instruction range before execution: nonempty buffer, cmdIndex strictly inside it, known opcodes, complete payloads, valid save indices and presence of a core callback mode when required. Structural refusal performs **zero register reads/writes, delay accumulation or core operations**, returning original registers/save values. Execution retains its own defensive checks.

This intentionally changes the C core's behavior, which can execute a valid prefix before discovering a malformed command. A timed-out poll or failed simulated callback in a structurally valid program still exposes prior effects. Preflight does not promise transactional hardware execution or rollback.

## Versioned envelope

Input starts with literal bytes `GSQ2`, then little-endian u32 fields:
1. `nwords`, `cmdIndex`, `pollBudget`, `coreMode`, `delayEnabled`;
2. eight initial register values, eight initial saved values;
3. exactly `nwords` command words.

Bounds: 1024 command words, pollBudget <=1024, coreMode 0..2, delayEnabled 0..1. Maximum input 4184 bytes; an extra byte detects overflow. Previous unversioned frames and unknown versions refuse. This is a test interface, not a final hardware ABI.

Opcode/payload sizes: WRITE=0/2 words, MODIFY=1/3, POLL=2/5, DELAY=3/1, STORE=4/2, CORE_RESET/START/WAIT/RESUME=5..8/0. Save slots are 0..7. cmdIndex must be below nwords, including empty execution. Words beyond cmdIndex are ignored as in the reference.

Simulation slots are **not a hardware allowlist**: out-of-map reads return zero and writes are ignored, with operations counted. Core mode 0 refuses, mode 1 simulates success, mode 2 simulates callback failure. No actual reset/start/wait/resume occurs. Delays accumulate modulo 2^32 without sleeping. Polling performs an initial read plus at most max(budget,1) retries; asynchronous device responses are not modeled.

Output remains 84 bytes: 21 little-endian u32 values containing status, read count, write count, delay total, core callback count, registers[8], save[8]. Exit 0 succeeds; 1 is a sequencer refusal with state output; 2 is envelope/capacity/I/O refusal. Reading to EOF precedes execution. Input errors emit nothing; output errors can leave a prefix. EINTR refuses. One process handles one frame; repeated in-process calls are not an exposed API.

## Qualification

From repository root on AArch64 Linux:
```sh
python3 native/omega/tests/check.py "$PWD" "$OMEGA_ROOT"
python3 native/omega/tests/mutants.py "$PWD" "$OMEGA_ROOT"
python3 native/omega/tests/check.py "$PWD" "$OMEGA_ROOT" # restore original artifact
python3 native/omega/tests/io.py "$PWD"
```
Output: `build/omega-sequencer/gspseq`. Python and the separately ASan/UBSan-instrumented C oracle are external qualification infrastructure, not linked into or called by the native program. Structurally valid streams compare against the unchanged C core. Structural refusals use an independent Python decoder and original-state/zero-operation expectations; legacy results are retained alongside them.

Pins:
- Compiler SHA256 `01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6`.
- Compiler reconstruction: omega-mv `9b072eb492e106d23f7eed984c5645251601adee`, `seed/omega-from-seed.sh`.
- C reference: aienux `2b47925d45171c2399e2617b62b85e6033e7747a`; exact C/header hashes checked before oracle compilation.

Version-2 receipts: `evidence/omega-sequencer/v2-*`.
- 222 executed cases, including 122 structural refusals, all pass. V1 fails 215 cases against the versioned contract.
- 56 cases demonstrate the change from legacy partial effects to preflight refusal with no operations. Five named prefix-effect tests explicitly assert that the legacy result differs from the untouched-state expectation.
- Five compiled mutants caught: skip preflight (56), omit preflight save check (2), omit preflight core check (9), wrong MODIFY mask (16), remove zero-budget poll retry (5). Source restored/retested afterward.
- Three real-kernel I/O refusal tests have a positive current-version frame control, preventing malformed old input from falsely satisfying an output-failure test.
- Wolfram independently verifies magic, dimensions, arithmetic, polling counts and five preflight output states.
- Clean-source reconstruction and existing plain/sanitized boot simulations have separate receipts. Boot simulations still exercise the unchanged C path, not native MMIO integration.

Qualification corrections: the compiler rejected a hexadecimal source literal with error6; the version magic now uses its independently calculated decimal value. The first skip-preflight mutant used untyped zero and failed compilation with error11; `int 0` corrected it before any mutant was counted. Neither compiler failure is a semantic success.

Historical V1 evidence remains unchanged, including its 214-case gate and the retained serialization failure. It qualifies the historical source, not V2. Independent source review and hardware/production integration remain separate gates. Agent-oriented redesign is authorized; this version explicitly names and tests its intentional behavioral change.
