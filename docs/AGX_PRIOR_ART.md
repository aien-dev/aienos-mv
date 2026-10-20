# AGX prior art: what is already known about the Apple GPU, and what AienOS may use

Ledger L214. Target machine: MacBook Air Mac14,2, Apple M2 (SoC T8112, GPU generation G14, variant
G, "G14G"), 10 GPU cores, 8 GB, macOS 26.6.2 (25G83), system firmware 18000.161.10, SIP enabled
(OBSERVED: `~/.hermes/cache/scratch/linux-replace/macbook-inventory-2026-10-20/`, `system_profiler`,
`sw_vers`, `csrutil`). The Metal compiler on this machine emits `applegpu_g14g` Mach-O GPU
executables (OBSERVED: `file` on a compiled archive, see `docs/AGX_ISA_NOTES.md`).

Every claim below carries a tag:

- **OBSERVED**: read by us in this session from a cloned source tree, a fetched page, or this Mac.
- **INFERRED**: stated by a prior-art source, not re-checked by us on this machine.
- **UNKNOWN**: nobody has published it; the row names the experiment that resolves it.

Sources were cloned read-only into `~/.hermes/cache/scratch/_ext/` on 2026-10-20 at these heads:

| Clone | Upstream | Commit | Commit date |
|---|---|---|---|
| `m1n1` | github.com/AsahiLinux/m1n1 | `7cde7d1a784d851ebfee5579fa363c38b14150c0` | 2026-10-19 |
| `asahi-linux` (sparse: `drivers/gpu/drm/asahi`, uapi, dts) | github.com/AsahiLinux/linux branch `asahi` | `94fb23346d522edf53722357c426a3e58030beea` | 2026-09-12 |
| `mesa` (sparse: `src/asahi`, `include/drm-uapi`) | gitlab.freedesktop.org/mesa/mesa | `ba3dacb9832bd073198d22c402f4e0d75ec3a09a` | 2026-10-20 |
| `applegpu` | github.com/dougallj/applegpu | `28dd80468d4a6c94fa4dac34ed488196f5fedc5f` | 2026-10-06 |
| `asahi-docs` | github.com/AsahiLinux/docs | `0d1f8917fa88745d62a4c05d802c4a7298c8616b` | 2026-10-05 |

## 1. Licences (OBSERVED from each tree) and what that means for AienOS

AienOS is `AGPL-3.0-or-later` (repo `LICENSE`, `Cargo.toml`). Rule for this work: nothing GPL is
copied into the AienOS kernel; GPL sources are read to learn facts (struct layouts, sequences) that
we then re-derive and re-express ourselves, with the source cited.

| Source | Licence as found | Brief assumed | AienOS use |
|---|---|---|---|
| Linux `drivers/gpu/drm/asahi/*.rs` (Rust kernel driver) | `SPDX: GPL-2.0-only OR MIT` on all 22 top-level files and `fw/`, `hw/` | GPL-2.0 OR MIT | **May use under MIT.** Firmware struct layouts (`fw/*.rs`), hardware configs (`hw/t8112.rs`), page-table code (`pgtable.rs`, `mmu.rs`) can be ported with attribution. |
| Linux `include/uapi/drm/asahi_drm.h` | `SPDX: MIT` | n/a | May use directly. |
| Mesa `src/asahi/**` (Gallium + Vulkan driver, compiler, ISA, genxml) | `SPDX: MIT` on every file (186 SPDX lines, all MIT) | MIT | **May use directly** with attribution. `isa/AGX2.xml` (machine-readable ISA), `isa/test/cases.txt` (555 encode/decode vectors), `genxml/cmdbuf.xml` (command-stream structs) are the most valuable. |
| dougallj `applegpu` (docs, disassembler, emulator, assembler) | **BSD-3-Clause** (`LICENSE`, "Copyright (c) 2021, Dougall Johnson") | MIT | May use with the BSD-3 notice kept. BSD-3 is AGPL-compatible. We use it only as a cross-check oracle, not as code. |
| m1n1 (bootloader, hypervisor, Python proxyclient incl. GPU tracer) | **GPL-2.0-or-later from v2.0.0**; code that existed in releases up to 1.9.9 stays MIT (`README.md` "License"). `proxyclient/m1n1/agx/*`, `fw/agx/*`, `hv/trace_agx.py` carry `SPDX: GPL-2.0-or-later`. | MIT | **Not copied.** Used as a tool (it boots and runs on the Mac, it is not linked into AienOS). The AGX struct definitions in `fw/agx/initdata.py` are read as facts only; the MIT-licensed Rust driver carries the same layouts and is the source we port from. |
| Asahi docs (`docs/hw/soc/agx.md`, `docs/sw/agx-driver-notes.md`) | **CC-BY-3.0** | n/a | Facts used with attribution; prose not copied. |
| Asahi Lina, "Tales of the M1 GPU", asahilinux.org, 2022-11-29 | blog post | n/a | Read (fetched 2026-10-20). Background only. |
| Alyssa Rosenzweig, "Dissecting the Apple M1 GPU, part I", 2021-01-07 (now at alyssarosenzweig.ca) | blog post | n/a | Read (fetched 2026-10-20). Background only. |

Compatibility note (INFERRED from the licence texts, not legal advice): GPL-2.0-only code cannot be
combined into an AGPL-3.0 work at all, and GPL-2.0-or-later can only be combined under GPL-3
terms. Taking the MIT branch of the drm/asahi dual licence avoids both problems.

## 2. What prior art documents, by layer

### 2.1 Boot and firmware ownership

- **INFERRED** (asahi-docs `agx.md`, Lina 2022): the GPU is driven through an ASC coprocessor
  ("gfx-asc") running Apple RTKit firmware. The OS never schedules GPU work on hardware directly;
  it writes shared-memory structures and rings a mailbox doorbell.
- **INFERRED** (asahi-docs `agx.md`): the GPU firmware is loaded by iBoot and write-protected.
  Asahi treats running Apple's firmware as non-negotiable. AienOS inherits this: the AGX path is
  "talk to Apple's GPU firmware", not "replace it".
- **OBSERVED** (`drm/asahi/driver.rs` lines 194-206): the MIT driver supports exactly five
  firmware/GPU combinations: G13 12.3, G14 12.4, G13 13.5, G14 13.5, G14X 13.5. M2 (G14G) is
  supported only with firmware **12.4 or 13.5**.
- **INFERRED** (Lina 2022): Apple changes firmware struct definitions between versions (100+
  changes for the second version). The firmware that runs is the one iBoot loads for the booted
  OS container, so a Linux or AienOS install picks its firmware version at install time (the Asahi
  installer installs a stub macOS of a supported version for this).
- Consequence for AienOS (decision): pin the GPU firmware to **13.5** (Asahi's newest supported
  ABI) via an Asahi-installer-style stub, so every struct in section 2.3 applies unchanged. The
  firmware this Mac's macOS 26.6.2 runs is a different, unpublished ABI (see
  `docs/AGX_FIRMWARE_INTERFACE.md`); tracing it is useful for cross-checks, not as our target.

### 2.2 GPU memory: UAT (the GPU MMU)

- **INFERRED** (asahi-docs `agx.md`): UAT uses ARM64-format page tables, 16 KiB pages, 40-bit
  GPU VAs (sign-extended), kernel/user split like TTBR0/TTBR1, up to 64 contexts in the driver
  (docs say 16; unverified), context table at `/arm-io/sgx` `gpu-region-base + ctx*0x10`.
- **INFERRED** (asahi-docs, phire on T6000 macOS 12.2): macOS VA layout `0x015_00000000` user,
  `0xf80_00000000` firmware private, `0xfa0_00000000` kernel-allocated firmware objects.
- **OBSERVED** (`drm/asahi/hw/t8112.rs`): `uat_oas: 40`, `base_clock_hz: 24_000_000`,
  `max_num_cores: 10`, `max_num_clusters: 1`, `num_dies: 1`, and the 20-entry MMIO list the
  firmware must have mapped (RGX at `0x204000000` size `0x20000`, Fender `0x204d00000`,
  AICTimer `0x20e100000`, MCache `0x200000000`, CRE `0x204e00000`, and others).
- Code: `drm/asahi/pgtable.rs` (690 lines), `mmu.rs` (1654 lines), MIT branch usable.

### 2.3 Firmware interface (initdata, channels, queues, work items)

All **INFERRED** from `drm/asahi/fw/*.rs` (MIT branch), versioned with `#[ver(...)]` attributes
for V12_3/V12_4/V13_0B4/V13_2/V13_3/V13_5:

| What | File (lines) | Content |
|---|---|---|
| Init data | `fw/initdata.rs` (1352), builder `initdata.rs` (1021) | `InitData` root: region A/B/C, power and DVFS tables, globals, channel pointers, firmware status, ~1000 fields |
| Channels | `fw/channels.rs` (443), `channel.rs` (631) | ring control (read/write pointers, cacheline-separated), message formats for the 4x3 work channels (TA, 3D, CP), DeviceControl, Event, FwLog, KTrace, Stats, FwCtl |
| Work queues | `fw/workqueue.rs` (187) | `CommandQueueInfo`, ring of command pointers per queue |
| Compute job | `fw/compute.rs` (120), `queue/compute.rs` (385) | `RunCompute` command, compute "microsequence" (start/wait/timestamp/finish ops) |
| Microsequences | `fw/microseq.rs` (412) | the op list the firmware executes per job |
| Events | `fw/event.rs` (104), `event.rs` | stamp objects for completion |
| Job common | `fw/job.rs` (160), `fw/types.rs` (224) | shared pointer and stamp types |
| Register map | `regs.rs` (494) | ID, status, fault registers the CPU reads |

- **INFERRED** (asahi-docs `agx.md`): RTKit endpoints `0x20` (init / "pong") and `0x21` (kick:
  `0x83..00` TA, `..01` 3D, `..02` compute, `..10` firmware, `..11` device control).
- The same structures exist as Python `construct` definitions in m1n1
  `proxyclient/m1n1/fw/agx/initdata.py` (2148 lines), `channels.py`, `cmdqueue.py`,
  `microsequence.py`, `handoff.py`: **GPL-2.0-or-later, read as facts only**.

### 2.4 Command stream (what the GPU front end consumes)

All **OBSERVED** in Mesa `src/asahi/genxml/cmdbuf.xml` (MIT):

- **CDM** (compute data master) stream: `CDM Launch Word 0/1` (uniform/texture/sampler/preshader
  register counts, mode direct/indirect, pipeline address `>>6`), `CDM Global size` and
  `CDM Local size` (3x u32 each), `CDM Barrier`, `CDM Stream Link/Terminate/Return`
  (block type in bits 29-31).
- **USC** pipeline words referenced by the launch: `USC Uniform` (load uniform registers from a
  memory buffer: start, size in halfs, address `>>2`), `USC Shared` (threadgroup memory),
  `USC Shader` (code address), `USC Registers` (GPR count in groups of 8), `USC No Preshader`,
  `USC Texture`, `USC Sampler`.
- VDM/PPP structures for vertex and fragment work (not needed for a first compute job).

### 2.5 Shader ISA

- **OBSERVED** Mesa `src/asahi/isa/AGX2.xml` (1641 lines, Valve 2025, MIT): a machine-readable
  description of every instruction encoding: 136 `<ins>` entries, operand encodings, length rules
  (2/4/6/8/10/12 bytes, bit 15 "length" extension), register hints (`cache`, `discard`), uniform
  vs GPR vs immediate sources. Plus 555 encode/decode cases in `isa/test/cases.txt` and a
  pure-Python reference disassembler in `isa/test/disasm.py`.
- **OBSERVED** Mesa `src/asahi/compiler/agx_pack.c` (1236 lines): the production encoder.
- **OBSERVED** dougallj `applegpu`: `applegpu.py` instruction descriptions, `disassemble.py`,
  `assemble.py`, an emulator, `docs.html`, and macOS tools (`compiler_explorer_tools/`) that
  compile Metal at runtime and extract the GPU binary from a binary archive. Written against
  G13 (M1); we confirm below it decodes G14G output.
- **INFERRED** (Rosenzweig 2021): scalar ISA at all bit sizes, hardware-scheduled (no NOP
  padding), free float modifiers (neg/abs/sat) and free f16/f32 conversion on sources.
- **INFERRED** (Mesa `compiler/README.md`): `r0l` is the hardware execution-nesting counter,
  `r1` the link register.

### 2.6 Tracing methodology

- **INFERRED** (Lina 2022, asahi-docs): Asahi learned the firmware interface by running macOS as
  a guest under the m1n1 hypervisor and logging every MMIO access and shared-memory change from
  the GPU driver. m1n1 ships the tracers: `proxyclient/hv/trace_agx.py` (113 lines),
  `trace_agx_defer.py`, `trace_agx_pwr.py`, and the decoder `proxyclient/m1n1/trace/agx.py`
  (1530 lines). This is the attended step in `docs/MACBOOK_AGX_TRACE_PROCEDURE.md`.

## 3. Use directly vs re-derive

| Item | Source | Verdict |
|---|---|---|
| ISA encodings for the subset we need | Mesa `AGX2.xml` (MIT), cross-checked with applegpu (BSD-3) and this Mac's compiler | Re-expressed in our own C (`native/gpu/agx/`), citing Mesa. Test vectors may be reused (MIT). |
| CDM / USC command words | Mesa `cmdbuf.xml` (MIT) | Use directly (field positions) with attribution. |
| Firmware structs for 13.5 | drm/asahi `fw/*.rs` (MIT branch) | Port with attribution when the kernel driver is written. |
| Firmware structs for macOS 26 firmware | nobody | UNKNOWN; resolved by the m1n1 trace. |
| UAT page tables | drm/asahi `pgtable.rs` (MIT branch) | Port. |
| m1n1 tracer and AGX Python model | m1n1 (GPL-2.0-or-later) | Run as a tool on the Mac only; never copied. |
| Hardware tables for T8112 (`hw/t8112.rs`) | drm/asahi (MIT branch) | Use directly. |

## 4. Gaps that prior art leaves open (and the experiment for each)

| Gap | Status | Experiment |
|---|---|---|
| G14G encodings the compiler emits that Mesa/applegpu do not decode | UNKNOWN until measured | Coverage run in `native/gpu/agx/` over compiler output (`docs/AGX_ISA_NOTES.md`). |
| macOS 26.6.2 GPU firmware struct layouts | UNKNOWN | m1n1 hypervisor trace of a Metal compute job (attended). |
| Whether a 13.5 stub firmware boots on this M2 with system firmware 18000.161.10 | INFERRED yes (Asahi supports M2 on current system firmware via its stub) | Asahi installer dry-run listing, then the attended install. |
| UAT context count limit (16 vs driver's choice) | UNKNOWN | read `uat_contexts` behaviour in the trace. |
