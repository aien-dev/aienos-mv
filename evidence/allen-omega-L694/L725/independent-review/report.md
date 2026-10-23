# L730 independent review — native Omega courier lifecycle

## Scope and provenance

- Reviewed exact candidate `ae16b168e3c135a4770b4400f4cb9a18dcb289f4` against parent `13d02ab4b7145579f6d3faa53442167bfcdcd7bc` in `~/workspace/aienos-allen-omega-L694`.
- Exported candidate source to `~/handoffs/L730-review/source` and rebuilt from that immutable export; local git worktree was clean at review start.
- Reviewed `native/omega/allen/scheduler.om`, its regression suite and API documentation, the reused Omega courier core, and legacy `native/kernel/x86/courier_udp.h` / `courier_udp_x86.c` wire behavior.
- Compiler SHA-256: `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9`.

## Results

**No findings** in the reviewed serialized lifecycle and tested boundary sequences.

Executed from the exact export with the pinned `build-one.sh` path:

1. Rebuilt and ran the narrow scheduler suite: compile exit 0, static AArch64 ELF64 EXEC with no `INTERP`/`NEEDED`, native exit 0, `213` assertions and `0` failures. Combined source SHA-256 `f92c69dd168d6f95984c9150af0544f11a9b265b4aa350e15dc3e3b7bf1de947`; executable SHA-256 `6a815fd66fdb6886af1a8dc857d6951268b43340bfd0f5dcf5ed7e677d384357`.
2. Independently authored and ran `build/lifecycle-review.om` as Omega against the candidate scheduler. Compile exit 0, native exit 0, `58` assertions, `0` failures. Sequences covered failed JOB send/retry, ACK suppressing retries, failed GOT retained through duplicate/retry, `qcancel` discarding pending receipt, post-cancel sequence advancement and stale ACK refusal, strict HI expiry and 2s/10s HELLO timing, and backward-clock LINE refusal without state movement. Full-probe source SHA-256 `46096dd4e85af3c5338088a9b5842cc53286f1eca7a09baf2b5eefe7ed164074`; executable SHA-256 `0e0ecd1704cc4bb5b8187e1b2fd0899b353f1acf78f5ab39b92dc90cfb14aaff`.
3. Built and executed a separate minimal Omega program producing the first HELLO, then compared the resulting bytes with the literal legacy header form using `cmp`: **PASS**, 73 bytes exact (`ALLEN-COURIER v=1 run=000000000000004d kind=HELLO seq=0 part=0/0 bytes=0\n`). Wire executable SHA-256 `7e34ddd7422c798c7abd1f1e551f793152539a5ff431d3c175168c43e2b5f08c`; raw emitted bytes SHA-256 `a33f0ab1a0cbf950f93fb4b85808f4b8af02271426c8f60583060a95bde4f801`.

The existing courier parser binds packets to the current run and sequence; the lifecycle suite exercises retries, ordered line delivery/duplicate receipts, queue-full recovery, cancellation and refusal boundaries. No source or production files were modified. No live sockets, service/device calls, iMac or physical-target qualification were performed or inferred; this is host-only native execution, not transport/deployment qualification.

## Review artifacts

- Export: `~/handoffs/L730-review/source`
- Narrow native suite executable/log/source: `~/handoffs/L730-review/build/scheduler`, `scheduler.log`, `scheduler.om`
- Independent Omega probe source/full-source/executable/log: `~/handoffs/L730-review/build/lifecycle-review.om`, `lifecycle-review.full.om`, `lifecycle-review`, `lifecycle-review.log`
- Wire probe source/executable/raw bytes/expected bytes: `~/handoffs/L730-review/build/wire.om`, `wire`, `wire.bin`, `hello.expected`
