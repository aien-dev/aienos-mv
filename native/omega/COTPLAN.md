# Native SEC2 COT packet construction (CTP1)

> Offline only. This is a serializer and simulator-input adapter, not firmware
> authentication, a DMA allocator, a hardware loader or a replacement kernel module.

`cotplan.om` hand-ports the final SEC2 packet-construction stage of
`native/kernel/dev/gb10_gsp_boot.c`. It writes the MCTP/NVDM headers, version and
payload size, FMC address and image size, hash/key/signature bytes and boot-argument
address. Reserved fields remain zero. The executable is built directly by the
pinned handwritten-seed compiler; no C translation or linked foreign runtime.

Its BTS1 output feeds the existing native causal boot simulator directly through
a pipe. Neither C nor Python re-encodes data between those native processes.
Shell/Python orchestrate external tests, and Linux implements the I/O syscalls.

## Contract

Input is exactly908 bytes: `CTP1`, the ten-word BTS1 header (packet length868,
reserved read-count0, argument-PA low/high, application version, four budgets,
pause flag), FMC PA as LE64, FMC image size as LE32, then hash48/key384/signature384
bytes, and nine LE32 causal-model settings. The image size must be nonzero,
budgets at most128 and boolean flags0/1. Physical addresses are serialized, not
validated as allocated or hardware-accessible. Cryptographic bytes are opaque;
this program neither calculates nor verifies their authenticity.

Output is exactly948 bytes: BTS1 header44, SEC2 packet868 and model settings36.
Within the packet, the FMC address starts at12, image size at28, opaque data at44
and argument address at860. The packet's frts size receives the FMC image size,
as the pinned legacy implementation does. All scalar fields are little-endian.
Malformed/overlong input and I/O failures exit2. Input is fully read and validated
before output begins; an output error may leave a prefix already written.

## Actual reference and consumer

The oracle calls **the unchanged full `gsp_boot_build`**, not a second packet
encoder. It supplies synthetic image bytes and an aligned host arena, takes
addresses and packet bytes from the returned plan, and compares native output
byte-for-byte with that packet. Both ordinary and ASan/UBSan oracle builds run.
These synthetic images are serialization fixtures, not bootable/authenticated
firmware; no real firmware or device is touched.

Forty fixtures exercise multiple arena-address ranges, image sizes, opaque data
and modeled success/error settings. Each also executes `cotplan | bootsim` and
compares it with the same pinned native consumer fed the C-produced packet.
The simulator itself has separate C-reference qualification; this pipeline is
consumer integration, not an independent physical-device oracle.

Evidence:48cases (40builder comparisons/native pipelines plus8 refusals), four
compiled encoder mutants, three real I/O failure controls, and two real Make
caller controls. RED retains40 missing-implementation failures and the old Make
target accepting a broken encoder. `sim-test` now requires the encoder suite and
I/O checks; the positive target runs both legacy simulations and rejects the
broken encoder in its new prerequisite.

`cot-replay.sh` reconstructs the compiler in a clean source export and runs the
combined actual Make entry. Prior packet fixtures and old receipts remain as
historical evidence, not substitutes for this execution.

## Remaining work

This replaces packet construction inside the native offline pipeline only.
The legacy arena/WPR/libos/radix/RPC/FMC-parameter builder still supplies production
callers. C shim/boot callers, NVIDIA modules, firmware admission, hardware bring-up
and recovery remain unretired under L332. Publication is not deployment.
