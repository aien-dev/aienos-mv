# Allen in seeded Omega (L694, WIP)

This directory contains handwritten seeded Omega implementations of Allen's
transport-independent state, protocol, memory codecs, scene state, code layout
and paint primitives. It compiles directly to static native Linux AArch64 ELF.
It has no foreign implementation, interpreter, C translator, launcher or fallback.

It is **not the complete iMac Allen replacement**. The old production C/Rust
paths remain live. No service, machine, physical disk or boot path was changed.

Run from this checkout on Linux AArch64, with the brief's immutable compiler:

```sh
quiet-build native/omega/allen/gate.sh target/allen-omega-L694/fresh-output
```

The output directory must be fresh. `build-one.sh` hashes the compiler before
**every invocation**, rejects empty/wrong-machine/nonstatic ELF and marks only
qualified output executable. The gate builds all modules together for every test,
executes handwritten Omega test programs, compares a byte-exact independent memory
fixture and rejects five compiled behavioral mutants. The build uses POSIX shell,
Git (source reconstruction), GNU file/text utilities, readelf, dd, xxd and
sha256sum as external development tools. Existing Rust courier `--split` was used
once as a fixture oracle, never as an Omega runtime dependency.

## Modules and API boundary

* `runtime.om`: one outstanding job, chat/code/follow-up, immediate code ack,
  ID exhaustion refusal, stale answer rejection, cancel/reset,20s slow-once and
 30s silence timeout. Injected monotonic milliseconds, no clock syscall.
 32 transcript lines of159 bytes, evict oldest whole line, restoration entry.
* `intent.om`: legacy intent word tables and JOB composition, preserving Allen
  and restored self/person names. No new canned ordinary answer table.
* `code.om`: CODE headers/escapes,128-byte pieces/32768-byte code,256 pieces,
 IEEE CRC32,800 lines, reorder/loss/duplicate handling; display only when whole.
* `reply.om`: SAY multipart/WAIT folding,159-byte answer capacity, one AGAIN
 retry for missing/bad code before giving up. Model text is always external input.
* `courier.om`: byte-compatible ALLEN-COURIER datagrams, session binding,
 JOB reassembly4096 bytes, ordered LINE queue8x319 bytes and duplicate receipt.
 Provider must implement retransmission/keepalive scheduling and actual transport.
 This is protocol core, not a socket, Ethernet driver or cryptographic peer check.
* `memory.om`:4096-byte AIENALM1 v2/v3/v4 and AIENNOT1 codecs. `Mem` is canonical
 decoded payload, `Unit` is input/staging, `Enc` is committed encoder output.
 SHA256(header0:32 || payload) is **unkeyed integrity**, never authorization or
 rollback freshness. Those independent proofs remain absent in this legacy format.
 `loadbeg` resets load state, `loadcopy(slot)` considers each valid copy once.
 `txbegin -> txwrite -> txflush -> txread` publishes store generation only after
 exact readback. Provider reads back into Unit and writes Enc to Tx's target.
 `nbegin -> nwrite -> nflush -> nread` verifies note writes before parent saves;
 provider must update the parent's note count only after nread succeeds. No disk
 syscall/driver is bound. Crash tests use explicit fixed simulated disk buffers.
* `view.om`: legacy code row wrapping, continuation numbering and saturated scroll,
 bounded4096 rows. The full code remains held when the row index reaches capacity.
* `scene.om`: mood/say/eight thing kinds, placement/emphasis/arrival/direction,
 typed field bounds, four things/six fields/2048-byte input/12 lines.
* `paint.om`: legacy integer color operations and soft-light pixel evaluation.
 Executed192x128 halo/heart component pose, colors from `ck_mind_draw`. This is
 not the full Allen sky/network animation, font, canvas item renderer or UI.

Fixed global pools model the platform boundary because seeded Omega globals
cannot be passed as array parameters. Functions are serialized, not reentrant.
Caller-owned metadata is trusted only where stated; lengths and external wire
values are validated at entry. Kernel/platform ABI and object ownership wrappers
must be specified when a qualified native provider exists.

## Intentional refusals and unfinished compatibility

Overflow refuses whole prompts/answers/scene fields instead of silent truncation.
Reply capacity159, prompt capacity159, code32768, job4096, note127. Conflicting
CODE duplicates and differing language headers are refused rather than overwriting.
Numeric overflow, backward timestamps, request ID wrap and v4 sky pad are refused.
Job instructions and record bytes retain legacy structure; envelope metadata is
never added to Store v1 superblocks. No identity migration fields are rewritten.

Remaining portable work: full scene tidying, fences/think suppression, plain-text
scene fallback, field ellipsis, timer interpretation, transcript-to-notes restore,
full notes indexing/retrieval, full memory metadata job append, transport timers,
full sky/catalogue/font/animated-network/canvas visual parity, integration and
broader malformed-input coverage. Partial modules are not whole compatibility.

Required target gates: qualified seeded x86 EFI target (FR9), privileged/MMIO and
interrupt/native device access(FR8), durable I/O(FR1), bounded networking(FR16),
keyboard/framebuffer/disk/NIC providers, iMac QEMU boot/chat/keyboard/bootdisk,
then signed physical trial. No C bridge can satisfy these. L408 owns compiler
work; L690 owns Hermes JSON/WebSocket/admission. Neither tree was modified.
