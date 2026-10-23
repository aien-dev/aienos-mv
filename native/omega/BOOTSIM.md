# Causal Omega GSP boot simulator — BTS1

> **Offline model, not the GPU.** The driver control sequence and the boot-used
> SEC2/GSP device state now run together in one seeded Omega executable. It
> performs no MMIO, firmware loading, ioctl or module operation. A successful
> modeled boot is not evidence of real hardware admission or production parity.

## What changed

`bootcore.om` contains six functions moved byte-for-byte from the qualified BTP1
implementation: EMEM decoding, readiness, bounded polling, packet packing,
upload and boot control. `bootseq.om` retains the recorded-read adapter for
regression. `bootcore-reuse.py` verifies exact extraction against the prior
published source; this is reuse of handwritten Omega, not C transpilation.

`bootsim-model.om` hand-ports the boot-used behavior of pinned `gsp_sim.c`.
Writes update the 256-word EMEM buffer and its auto-increment pointer. The SEC2
head write checks the assembled packet and changes phase. Later mailbox and
HWCFG2 reads advance command, mask, argument and lockdown state. Fault flags
control stuck counters, missing EMEM advancement, FMC errors and an inactive
core. No read tape, C subprocess or hidden runtime supplies the native responses.

The earlier C simulator remains an **external oracle** and serves existing
legacy callers. This does not remove that file, switch the production C boot
engine, or replace Linux/NVIDIA modules. It replaces the device-response
implementation inside the new native boot-simulation path. The C model's
SIM-CHOICE assumptions are preserved, including its zero-low-address behavior;
matching them does not validate those assumptions on hardware.

## Construction and bounds

Compose `regcore.om`, `bootsim.om`, `bootsim-model.om`, `bootcore.om`, in that
order, with one LF between files. Global declarations must precede their use in
the pinned seed compiler; forward function calls work. The recorded-read build
uses `regcore.om`, `bootseq.om`, `bootcore.om`. Both share the same control core
and permission core. Compiler fingerprint and C source/header closure are pinned
by the harness. The output is a static AArch64 executable with Linux I/O only.

BTS1 input: magic, the BTP1 ten-word header (read-count slot reserved and zero),
raw packet, then nine u32 model settings: busy, command, mask, argument and
lockdown read delays; FMC error; inactive-core and no-EMEM-increment flags; and
initial zero-HWCFG2 read delay. All words are LE32. Delays allow any u32;
0xffffffff means no end. Boot budgets cap at128, independent of delay values.
Boolean flags and pause flag are 0/1. Packet envelope is at most2048 bytes;
boot acceptance remains1..1024. Maximum input2128 bytes plus one excess byte.

Output starts with the BTP1 17-word header and ordered `(op,offset,value)` events.
It then appends the nine final model settings, 17 state words and all256 EMEM
words. State order: phase, delay counter, zero counter, SEC2 head/tail, EMEMC,
SEC2 mailboxes0/1, HWCFG2, GSP mailboxes0/1, CPUCTL, OS, violations, packet-valid,
OS-write phase, GSP-notify count. Maximum1600 events and20396 output bytes.
Every field and every event is compared with the C reference, not just status.
Exit0 means a result frame; inspect its stage for modeled success/failure.
Framing/capacity/I/O errors return2. Prior modeled effects remain on failure.

This public adapter exercises boot only. Standalone arbitrary-register access,
EMEMD reads, post-boot notification and repeated requests are not qualified by
this corpus. The policy still checks each access. No transactional rollback,
cryptographic firmware validation or timing fidelity is claimed.

## Evidence

- Recorded-read implementation RED:139 failures in146 cases.
- Native causal model GREEN:146 cases, including139 byte-exact differentials
  against **both plain and ASan/UBSan** C boot+device implementations; all stages
  0..8 represented. Tests cover partial/max packets, bad packet headers, every
  stuck poll, zero-mask periods, delayed success, PA boundaries and fault mixes.
- Six compiled model mutants caught, targeting auto-increment, packet validation,
  zero-mask delay, inactive core, argument delay and lockdown release.
- Four real I/O refusal controls, including a short write followed by EAGAIN.
- Shared-core extraction retains the125 recorded-read cases, six original
  control-core mutants and four I/O controls.
- Wolfram independently checks packed constants and capacity formulas.

`tests/bootsim.py ROOT OMC` builds the native program and two external oracles.
`bootsim-mutants.py ROOT OMC` and `bootsim-io.py ROOT` run the other checks.
No performance or hardware qualification follows from these host-only results.

The existing `make -C native/linux/aien-gpu sim-test` entry now requires both
native backends and both I/O suites. `bootsim-caller-red.json` preserves the old
target accepting a deliberately broken causal model; six caller controls now
pass, including refusal of that model while the recorded-read suite still
passes. The shared-core substitution controls were retargeted to `bootcore.om`
so they remain successfully compiled semantic failures after extraction.
`bootsim-replay.sh` reconstructs the seed compiler and runs this actual combined
entry from a clean source export, once per native backend.
