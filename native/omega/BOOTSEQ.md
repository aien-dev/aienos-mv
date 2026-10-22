# Seed-native GSP boot-control port (BTP1)

> **Offline only. This does not boot the GPU or replace a loaded NVIDIA module.**
> `bootseq.om` hand-ports the control logic of `ck_gsp_mmio_boot` from
> `native/kernel/gpu/gsp_mmio.c`: channel-idle polling, packet upload, EMEM pointer
> verification, queue submission, SEC2 completion, privilege-mask/lockdown polling,
> FMC error check, application-version write and RISC-V-active check.
> Register reads come from an explicit bounded tape; writes are recorded, not sent
> to hardware. Responses are test inputs, not observed device behavior. The tape
> is not a model that derives device responses from preceding writes.

## Construction and scope

Compile `regcore.om`, one LF, then `bootseq.om` with the pinned handwritten-seed
compiler. The existing native register policy checks every modeled access, with
boot permission enabled and an initially unused SEC2 doorbell. The resulting
static AArch64 executable has no C translation, linked foreign runtime or hidden
Python bridge. Linux supplies stdin/stdout syscalls. Shell/Python and the C
sanitizer oracle are external qualification infrastructure only.

The external oracle calls the unchanged C boot function against the same read
tape and records its ordered access trace. Tests compare the **entire output**,
including every packet word, write order, read value, stage, poll count and pause
count, not only final registers. C source/header and callback-ABI header are
fingerprinted in `tests/bootseq.py`. This ports one real driver control routine;
it does not retire its C callers or the C MMIO shim. The separate C notify routine
is not part of this port. Live MMIO, firmware admission, kernel attachment,
service parity and recovery remain unqualified under L332.

## Binary contract

All scalar words are little-endian u32. Input: magic `BTP1`, then ten words:
packet byte count, read-pair count, argument-PA low/high, application version,
four polling budgets (channel/SEC2/mask/lockdown), and pause-callback-enabled.
The raw packet immediately follows, with no padding; then `(offset,value)` read
pairs. Exact envelope length is required. Limits: packet envelope 2048 bytes,
1024 read pairs, budgets 0..128, pause flag 0..1. Packet lengths 0 and 1025..2048
are valid envelopes that produce the legacy argument-failure stage; accepted
boot packets are 1..1024 bytes. Unconsumed read pairs may remain after an early
return, just as a device can have future values that the routine never reads.
The input capacity is 10284 bytes plus a one-byte excess detector.

Output: 17 header words followed by `(operation,offset,value)` events; operation
0 means read and 1 means write. Header: stage, four poll counts, uploaded dwords,
EMEMC-after, SEC2 mailbox0/1, HWCFG2, GSP mailbox0/1, CPUCTL, committed SEC2
latch, consumed-read count, event count, and pause count. Capacity: 1600 events,
19268 output bytes. Pause counts model callback invocations, not elapsed time.

Stages 0..8 are the magnitude of the original C return code: modeled success,
argument, channel busy, EMEM, SEC2 timeout, mask timeout, lockdown timeout, FMC
error, and inactive core. Stage9 refuses exhausted/mismatched read tapes before
recording that access. Stages10/11 are defensive policy/event-capacity refusals;
these are not reached by the pinned legal-address/bounded corpus and have no
claimed direct runtime coverage. Earlier recorded operations remain visible on
failure; no rollback or whole-sequence transaction is promised. Process exit0
means a valid result frame, **not** stage0. Framing/capacity/I/O errors exit2.

The channel check precedes all writes. Packet words are zero-padded at the end
and serialized little-endian. EMEM pointer advancement is checked before either
queue write. Queue tail precedes the one-shot head write. Budgets retain the C
inclusive last probe (`budget+1` observations), with no pause after a terminal
failure. Both halves of the argument PA participate in the lockdown condition.
The original pointer-wrap refusal is preserved, not silently redesigned.

## Executed evidence

- Initial compilable stub: 118 cases, 114 failures (`boot-red.json`).
- Expanded final suite: 125 passing cases, including 115 byte-exact C
  differentials covering stages0..8, two read-tape refusals and eight framing
  refusals. Includes nonzero/wrapping EMEM pointers, partial words, maximum
  packet and polling bounds, zero/high address halves and delayed success.
- Six compiled semantic mutants are caught: reversed doorbell ordering, omitted
  EMEM validation, big-endian packing, extra poll, ignored high PA and ignored
  inactive-core result. Compiler failures do not count as killed mutants.
- Four I/O failures: closed input, EAGAIN after input progress before EOF,
  `/dev/full`, and an actual 4096-byte short write followed by EAGAIN while
  emitting a 12524-byte result. Earlier output bytes exactly match the reference.
- Wolfram independently checked magic, masks and capacities; it is not a build
  or runtime dependency. Source composition and ELF hashes are in receipts.

Run `python3 native/omega/tests/bootseq.py "$PWD" /path/to/pinned/omc`, then
`bootseq-mutants.py` with those same arguments and `bootseq-io.py "$PWD"`.
The I/O harness consumes fixtures created by the passing primary suite.
`evidence/omega-sequencer/boot-replay.sh` reconstructs the compiler from the seed
in a clean committed export and repeats the primary suite, I/O and existing
normal/sanitized boot-simulator gates. This remains host-only qualification.

## Required driver simulation entry

`make -C native/linux/aien-gpu sim-test` now requires the phony
`omega-boot-test` prerequisite: the native boot differential suite and I/O
checks must succeed before this target can pass. It runs Python in isolated
mode so `PYTHONOPTIMIZE` cannot erase the qualification assertions. A cached
binary or a file named `omega-boot-test` cannot satisfy this prerequisite.

`tests/bootseq-caller.py` preserves the old-target RED: a broken native boot
stub still passed both legacy simulations. GREEN checks the qualified native
core, the broken stub, the same stub under optimization, a successful cached
run followed by broken source (with a colliding target file), and a changed
compiler under optimization. Each case runs in its own exported source tree.
This attaches native boot-control verification to a real test caller; it does
not switch the production boot engine to Omega or make read tapes into a
causal device model.
