# Apple GPU (AGX) firmware and driver interface: what a first compute job needs

Ledger L214, step 3. Machine: MacBook Air Mac14,2, Apple M2 (T8112), GPU
G14G with 10 cores, macOS 26.6.2 (25G83). Everything here was gathered
read-only from the running macOS, from the firmware files macOS keeps on
disk, and from published open-source drivers. Nothing on the machine was
changed. ISA notes: `docs/AGX_ISA_NOTES.md`. Prior art and licences:
`docs/AGX_PRIOR_ART.md`.

Every claim is tagged:
- **OBSERVED**: read on this machine or out of an Apple firmware binary, with the command or address given.
- **INFERRED**: taken from a named open-source source, at the version given.
- **UNKNOWN**: not settled. Each UNKNOWN row names the experiment that settles it.

## Sources

| Id | Source | Version |
|---|---|---|
| DT | `ioreg -p IODeviceTree` nodes `sgx` and `gfx-asc` (Apple device tree, as exposed by macOS) | this Mac, macOS 26.6.2 (25G83) |
| IOK | `ioreg` service `AGXAcceleratorG14G` | same |
| FW26 | `/System/Volumes/Preboot/<volume>/restore/Firmware/agx/armfw_g14g.im4p` (the GPU firmware iBoot loads on this Mac), read-only copy in scratch | sha256 `4c96c202ad07f572afb3b1a35993d4ef3bc10a83861029f11132480c90811092`, `RTKit-3255.160.4.release` |
| FW135 | `Firmware/agx/armfw_g14g.im4p` inside Apple's macOS 13.5 restore image (`UniversalMac_13.5_22G74_Restore.ipsw`, the image the Asahi installer uses). Only this member was fetched, by HTTP range requests (1.2 MB of 12.9 GB). | sha256 `8efecf9275b50ff6e7efc671b6a0de7078c274547991cae45222fbfbaa9b5df1`, `RTKit-2062.141.1.release` |
| ASAHI | Linux `drivers/gpu/drm/asahi` (Rust, `GPL-2.0-only OR MIT`), struct layouts evaluated for G14, firmware V13_5 | asahi-linux `94fb23346d52` |
| M1N1 | m1n1 `proxyclient/m1n1/fw/agx/*.py` (independent Python definitions of the same structs; used only as a cross-check) | m1n1 `7cde7d1a784d` |
| MESA | Mesa `src/asahi/genxml/cmdbuf.xml` and `src/asahi/libagx/libagx_dgc.h` (MIT) | Mesa `ba3dacb9832b` |

Apple's firmware binaries are not in the repo and are not redistributed.
Only facts about them (addresses, constants, sizes) appear here.

## Result in one paragraph

The macOS 26 GPU firmware on this Mac does **not** speak the interface the
open-source driver implements. Its init handler accepts InitData only if the
first 8 bytes equal `0x0c89c32b039004c8`. It copies 0xc8 bytes, and it
checks a flag at offset 0x2c. The macOS 13.5 firmware for the same GPU
expects `0x00b00601_1f286ba0`, which is drm/asahi's `ver_info` for G14 V13_5.
It copies 0xb0 bytes and checks the flag at 0xa8, where drm/asahi puts
`host_mapped_fw_allocations` (all **OBSERVED**, addresses below). So
AienOS's first compute job targets firmware 13.5, which is what iBoot loads
when it boots a small macOS 13.5 stub. That is the same arrangement the
Asahi installer creates. The struct layouts for that firmware are
**INFERRED** from drm/asahi, and their sizes and offsets agree with m1n1's
independent definitions (table below). The macOS 26 interface stays an
UNKNOWN, with the experiments that would map it.

## 1. GPU identity

| Fact | Value | Status, source |
|---|---|---|
| Device tree compatible | `gpu,t8112` (node `sgx`) | OBSERVED, DT |
| Accelerator class | `AGXAcceleratorG14G`, `IONameMatched` = `gpu,t8112`, `model` = `Apple M2` | OBSERVED, IOK |
| Driver kext versions | `IOSourceVersion` 353.14; `AGXFirmwareKextG14GRTBuddy` 353.14; `AGXTraceCodeVersion` 3.44.6 | OBSERVED, IOK |
| GPU configuration | `GPUConfigurationVariable` = num_gps 4, gpu_gen 14, usc_gen 2, num_cores 10, num_mgpus 1, gpu_var G, core_mask_list (1023), num_frags 10; `gpu-core-count` 10 | OBSERVED, IOK |
| Driver config for T8112 | `gpu_core: G14G`, `max_num_clusters 1`, `max_num_cores 10`, `max_num_frags 10`, `max_num_gps 4`, `base_clock_hz 24 MHz` | INFERRED, ASAHI `hw/t8112.rs`. Agrees with the OBSERVED row above. |
| System firmware | `mBoot-18000.161.10` (`system_profiler`: System Firmware and OS Loader 18000.161.10). m1n1's table maps this to macOS 26.6.2. | OBSERVED, DT `/chosen`; INFERRED mapping, m1n1 `src/firmware.c` |
| Firmware ABI m1n1 hands to Linux | m1n1 passes `apple,firmware-compat` = the booted OS's firmware version, except 12.3.1 maps to 12.3 and 13.5 beta 4 / 13.6.1 map to 13.5. drm/asahi only implements 12.3, 12.4 (G14) and 13.5. | INFERRED, m1n1 `src/kboot_gpu.c`; ASAHI `driver.rs` |

## 2. Firmware images

| Fact | macOS 26.6.2 (FW26, running) | macOS 13.5 (FW135) | Status |
|---|---|---|---|
| Container | IM4P, type `gfxf`, uncompressed | IM4P, type `gfxf`, uncompressed | OBSERVED |
| Image table | `rkosftab`, 2 entries (`a000`, `b000`). Offsets are relative to the start of the IM4P payload. | same layout | OBSERVED |
| Image sizes | a000 and b000 each 0x175d30 bytes | each 0xd1eb8 bytes | OBSERVED |
| RTKit version string | `RTKit-3255.160.4.release` | `RTKit-2062.141.1.release` | OBSERVED |
| Which image runs | `b000`: its Mach-O UUID `DB320FB5-0CF0-3951-A187-E90716D782E9` equals the `uuid` property of `gfx-asc` | not applicable | OBSERVED (DT, FW26) |
| Image format | arm64e Mach-O, `MH_PRELOAD`. `__TEXT` at VA `0xffffff8000000000` (0x5c000 bytes), `__DATA` at VA `0xffffff800005c000` (0x114000 bytes). | arm64 Mach-O, `__TEXT` at the same VA | OBSERVED (`objdump --macho --private-headers`) |
| Where iBoot placed it | `gfx-asc` `segment-ranges`: TEXT phys `0x800af4000` maps to `0xffffff8000000000`; DATA phys `0x801d0c000` maps to `0xffffff800005c000`, size 0x114000. `sgx` `gfx-data-base` `0x801d0c000`, `gfx-data-size` 0x114000. | not applicable | OBSERVED, DT |
| Loaded by | `gfx-asc` child `iop-gfx-nub`: `firmware-name` `GFX`, `pre-loaded` 1. The firmware is already in memory when the OS starts. | not applicable | OBSERVED, DT |

### 2.1 The init handshake, read from both firmware binaries

Both images contain the same routine. FW26 b000 has it at
`0xffffff80000291c8`, FW135 b000 at `0xffffff800002de24`. FW26 a000 carries
the identical constant (OBSERVED). The routine:

1. Waits on RTKit endpoint `0x20` for one message (`mov w0, #0x20` before
   the receive call). Both images. **OBSERVED**
2. Accepts it only if `(msg & 0x00bf000000000000) == 0x0081000000000000`.
   This is drm/asahi's `MSG_INIT = 0x81 << 48`. Otherwise it aborts with
   "Unable to grab init data from host". Both images. **OBSERVED**
3. Forms the InitData address as `msg | 0xfffff00000000000`. That
   sign-extends drm/asahi's 44-bit `INIT_DATA_MASK` field into the kernel
   VA range. Both images. **OBSERVED**
4. Compares the first 8 bytes of InitData with a constant. On mismatch it
   aborts with "Host and FW Interface is mismatched, please ensure the
   versions are aligned." **OBSERVED**

| | FW135 (13.5) | FW26 (26.6.2) | drm/asahi G14 V13_5 |
|---|---|---|---|
| Expected first 8 bytes, as `u16[4]` | `[0x6ba0, 0x1f28, 0x0601, 0x00b0]` (`0xffffff800002de6c`) | `[0x04c8, 0x0390, 0xc32b, 0x0c89]` (`0xffffff8000029218`) | `ver_info: [0x6ba0, 0x1f28, 0x601, 0xb0]` |
| Bytes of InitData copied | 0xb0 | 0xc8 | struct is 0xbc |
| Offset of the "host-mapped FW allocations" flag (abort if 0) | 0xa8 | 0x2c | `host_mapped_fw_allocations` at 0xa8 |
| 8-byte values read right after the copy | +0x18, +0x20, +0x28 | +0x18, +0x20, +0xa8, +0xb0, +0xb8, +0xc0 | `runtime_pointers` +0x18, `globals` +0x20, `fw_status` +0x28 |

FW135 matches drm/asahi on every point. **OBSERVED.** The routine
continues into the structs below. For example, it reads
`RuntimePointers+0x1a8` (`hwdata_b_2`), then `HwDataB+0x968` (`chip_id`),
`+0x978..0x99c` (`unk_460..unk_484`) and `+0x6a8` (inside `io_mappings`,
entry 3, +8). It also takes the address `RuntimePointers+0x21c`
(`buffer_mgr_ctl_fw_addr`). Every one of these lands on a field boundary of
the INFERRED layout. **OBSERVED** (FW135 `0xffffff800002dec4`..`0x2df84`).

FW26 differs in the version constant, the size, the flag offset and the
pointer set. A driver built for 13.5 would be rejected at step 4.
**OBSERVED.** Bytes `0xb0..0xbb` of the 13.5 InitData (`unk_b0`, `unk_b4`,
`unk_b8`) are not copied by this routine. **OBSERVED.**

## 3. Memory regions and MMIO

| Device tree property (node `sgx`) | Value | Meaning | Status |
|---|---|---|---|
| `reg` / IODeviceMemory | `reg` (bus-relative): `0x4000000` size `0x4000000`, `0x4d00000` size `0x20000`. IODeviceMemory (absolute): `0x204000000` (8657043456) 64 MiB, `0x204d00000` (8670674944) 128 KiB | GPU MMIO windows | OBSERVED values; meaning INFERRED (ASAHI `regs.rs` maps the GPU register block from `reg`) |
| `gfx-handoff-base` / `-size` | `0x9fff70000` / 0x4000 | UAT handoff page shared by the AP and the firmware (`/reserved-memory/uat-handoff`) | OBSERVED values; mapping INFERRED, m1n1 `kboot_gpu.c` line 689 |
| `gfx-shared-region-base` / `-size` | `0x9fff78000` / 0x40000 | GPU page tables for the kernel half (`/reserved-memory/uat-pagetables`) | same, line 691 |
| `gpu-region-base` / `-size` | `0x9fffb8000` / 0x4000 | TTBAT: per-context page-table base pairs (`/reserved-memory/uat-ttbs`) | same, line 693 |
| `rtkit-private-vm-region-base` / `-size` | `0xffffff8000000000` / 0x20000000 | firmware's own VA window; equals the image VA base | OBSERVED |
| `gfx-data-shared-ro/rw-base/size` | 0 | unused on this machine | OBSERVED |
| `ttbat-phys-addr-base` | raw `<eeff2700>` | UNKNOWN (4 bytes, not a full physical address) | OBSERVED raw; meaning UNKNOWN (U6) |
| `interrupts` | `0x2b9 0x2ba 0x2bb 0x2bc 0x2c9` (u32 each), `interrupt-parent` 0x69 | GPU interrupt lines | OBSERVED raw; per-line meaning UNKNOWN (U7) |
| `clock-ids` / `power-gates` | 0x166 / 0x165, 0x166 | PMGR clock and power domains | OBSERVED |
| `gfx-asc` IODeviceMemory | 8694792192 (0x206400000) len 442368; 8690925568 len 8 | ASC (firmware coprocessor) mailbox and control | OBSERVED; meaning INFERRED (ASAHI uses the generic Apple RTKit/ASC mailbox) |
| `gfx-asc` `compatible`, `iop-version`, `role` | `iop,ascwrap-v4`, 1, `GFX` | | OBSERVED |
| Other `sgx` flags | `has-kf` 1, `agx-address-space-mgmt-mode` 1, `gptbat-ready` 1, `gfx-qos` 1/1, `metal-standard` `<00010000>` | | OBSERVED; meaning UNKNOWN, not needed for a first job |

Sizes drm/asahi needs inside these regions (INFERRED, ASAHI `mmu.rs`):
- handoff struct 0x648 bytes (fits in 0x4000);
- TTBAT 64 contexts x 16 bytes = 0x400 (fits in 0x4000);
- at least one 16 KiB page-table page.

## 4. RTKit endpoints and messages

| Item | Value | Status |
|---|---|---|
| Firmware endpoint (init, firmware-to-host notifications) | `EP_FIRMWARE = 0x20` | INFERRED (ASAHI `gpu.rs`); used as the init endpoint in FW135 and FW26: OBSERVED |
| Doorbell endpoint (host to firmware) | `EP_DOORBELL = 0x21` | INFERRED (ASAHI `gpu.rs`) |
| Init message | `0x81 << 48 \| (InitData GPU VA & (2^44 - 1))` on EP 0x20 | INFERRED; decode OBSERVED in both firmwares (section 2.1) |
| Pipe doorbell | `0x83 << 48 \| pipe_type \| (priority << 2)` on EP 0x21; pipe_type vertex 0, fragment 1, compute 2; priority 0..3 | INFERRED (ASAHI `gpu.rs::run_job`, `fw/channels.rs::PipeType`) |
| Firmware kick | `0x83 << 48 \| 0x10` | INFERRED |
| Device-control doorbell | `0x83 << 48 \| 0x11` | INFERRED |
| Firmware-control doorbell | `0x84 << 48` | INFERRED |
| Receive doorbell notification (firmware to host) | `0x42 << 48` on EP 0x20 | INFERRED |

## 5. GPU MMU (UAT)

All INFERRED from ASAHI `pgtable.rs` and `mmu.rs`:
- 16 KiB pages (`UAT_PGBIT` 14), 3 levels, 39-bit input address space.
- User VA from 0x4000 up to `1 << 39`. The page just below the top is reserved.
- 64 contexts in the TTBAT. Context 0 is the kernel/firmware; users get 1..63.
- TTBR value = table physical address, with bit 0 valid and the ASID in bits 48 and up.
- Kernel VA ranges used for firmware objects:
  - private `0xffffffa000000000..0xffffffa600000000`
  - GPU-RO `..a800000000`
  - shared `..aa00000000`
  - shared-RO `..ac00000000`
  - GPU `..ae00000000`
  - RTKit `0xffffffae00000000..0xffffffae10000000`
  - MMIO `0xffffffaf00000000..0xffffffb000000000`
- Handoff struct (`repr(C)`, 0x648 bytes):

| Offset | Field | Notes |
|---|---|---|
| 0x000 | `magic_ap` u64 | AP writes `PPL_MAGIC = 0x4b1d000000000002` |
| 0x008 | `magic_fw` u64 | firmware's answer |
| 0x010 | `lock_ap` u8, `lock_fw` u8, 2 bytes padding | Dekker lock between AP and firmware |
| 0x014 | `turn` u32 | |
| 0x018 | `cur_slot` u32, 4 bytes padding | context the firmware has bound |
| 0x020 | `flush[65]` of {state u64, addr u64, size u64} | TLB flush requests per context, plus one |
| 0x638 | `unk2` u8, 7 bytes padding | UNKNOWN meaning |
| 0x640 | `unk3` u64 | UNKNOWN meaning |

## 6. Performance states

Decoded from the little-endian `(u32 Hz, u32 mV)` pairs in `sgx`
`perf-states` and `perf-states-sram`. `perf-state-count` 9,
`gpu-num-perf-states` 8 (index 0 is the off state). **OBSERVED.**

| Index | Frequency | GPU voltage | SRAM voltage |
|---|---|---|---|
| 0 | 0 (off) | 125 mV | 780 mV |
| 1 | 444 MHz | 625 mV | 780 mV |
| 2 | 612 MHz | 655 mV | 780 mV |
| 3 | 808 MHz | 690 mV | 780 mV |
| 4 | 968 MHz | 755 mV | 780 mV |
| 5 | 1110 MHz | 815 mV | 815 mV |
| 6 | 1236 MHz | 855 mV | 855 mV |
| 7 | 1338 MHz | 895 mV | 895 mV |
| 8 | 1398 MHz | 930 mV | 930 mV |

The frequencies equal the Asahi T8112 device tree's GPU OPP table. The
voltages differ (Asahi: 603..950 mV) because m1n1 overwrites
`opp-microvolt` with each machine's own `perf-states` at boot. INFERRED,
m1n1 `kboot_gpu.c` lines 569 to 657; Linux `arch/arm64/boot/dts/apple/t8112.dtsi`.

## 7. Boot: the InitData tree (firmware 13.5, G14)

The host builds this tree in GPU-visible memory and sends its address in the
init message. All sizes are INFERRED (ASAHI), with every size cross-checked
against M1N1 (section 9). Every struct in the tree has a full field table
in `docs/AGX_FW_LAYOUT_G14_V13_5.md`, generated from the driver sources.

| Struct | Size | Points to | Notes |
|---|---|---|---|
| `InitData` | 0xbc | `RuntimePointers` (+0x18), `Globals` (+0x20), `FwStatus` (+0x28) | `ver_info` at +0; `host_mapped_fw_allocations` at +0xa8, OBSERVED in FW135 |
| `RuntimePointers` | 0x6bc0 | the 12 pipe channel rings (`pipes`, +0x0: 4 priorities x vertex/fragment/compute); device-control (+0xc0), event (+0xd0), firmware-log (+0xe0), ktrace (+0xf0) and stats (+0x100) rings; stats buffers; `HwDataA` (+0x188); `HwDataB` (+0x1a0, +0x1a8); buffer-manager control | +0x1a8 and +0x21c used by FW135, OBSERVED |
| `Globals` | 0x12394 | | power management tuning, firmware tunables |
| `HwDataA` | 0x421c | | per-chip power and clock tables (filled from perf states, section 6) |
| `HwDataB` | 0x1884 | | chip id, IO mappings, more per-chip constants |
| `FwStatus` | 0x80 | the firmware-control ring (`fwctl_channel`) | firmware-to-host status flags |
| `ChannelRing` (per ring) | 0x10 | `ChannelState` (0x30) + the message array | |
| `UatLevelInfo` x3 (inside InitData) | 0x20 each | | page-table level description |

The many `unk_*` fields in `Globals`, `HwDataA` and `HwDataB` take the
constant values drm/asahi writes in `initdata.rs` and `hw/t8112.rs`.
Their offsets are INFERRED; their meanings are UNKNOWN unless named.

## 8. Submitting one compute job (firmware 13.5, G14)

The path drm/asahi takes in `queue/compute.rs` and `workqueue.rs`
(INFERRED, ASAHI):

1. **Command buffer (built by the user-space driver).** A CDM control
   stream in GPU memory: a launch block followed by a stream terminate.
   The launch names a USC word list ("pipeline"), whose `Shader` word
   points at the shader machine code (the ISA in `AGX_ISA_NOTES.md`).
   Encoding in section 8.1.
2. **`RunCompute` (0x31c bytes, GPU-RO kernel memory).** Fields that matter:
   - `tag` = 3 (`CommandType::RunCompute`);
   - `counter` = per-queue sequence;
   - `vm_slot` = the TTBAT context of the job's address space;
   - `notifier`;
   - `job_params1.cdm_ctrl_stream_base` = the control stream address;
   - `job_params1.usc_exec_base_cp` = the base every USC code and pipeline offset is relative to;
   - `job_params1.unk_38` = 0x8c60;
   - `job_params1.unk_58` = 1;
   - `job_params2.cdm_ctrl_stream_end`;
   - `microsequence` / `microsequence_size`;
   - `encoder_params` (sampler heap);
   - `meta` (stamp pointer, stamp value, slot);
   - `client_sequence`.
3. **Microsequence (private kernel memory).** Firmware opcodes, packed back to back:

| Op | Opcode | Size | Purpose |
|---|---|---|---|
| `StartCompute` | 0x29 | 0x16c | points at `job_params1`/`job_params2`, the work queue, stats, VM slot, event sequence, attachments |
| `Timestamp` (optional) | 0x19 (bit 31 = start) | 0x3c | user timestamps |
| `WaitForIdle` | 0x01 \| (`Pipe::Compute` = 0x8000) << 8 | 0x4 | wait for the compute pipe |
| `Timestamp` (optional) | 0x19 | 0x3c | |
| `FinalizeCompute` | 0x2a | 0x7c | stamp pointer and value to write on completion; `restart_branch_offset` = offset back to `StartCompute` |
| `RetireStamp` | 0x18 \| 0x40000000 | 0x4 | signal completion |

4. **Work queue.** One `QueueInfo` (0xb8) per queue, with:
   - `RingState` (firmware-visible part 0x60: `gpu_doneptr`, `gpu_rptr`, `cpu_wptr`, `rb_size`, each in its own 16-byte slot);
   - a ring of `u64` command pointers;
   - `GpuContextData` (0x40);
   - a `NotifierList`.

   The host writes the `RunCompute` GPU VA into `ring[wptr]`, advances `wptr`, then stores `cpu_wptr` with release ordering.
5. **Pipe channel message.** The host appends a `RunWorkQueueMsg` to the
   compute channel ring of the chosen priority. It carries:
   - `pipe_type` 2;
   - `work_queue` pointer;
   - `wptr`;
   - `event_slot`;
   - `is_new`;
   - padding.

   The message is 0x40 bytes on G14 with firmware 13.2 or later (0x30 elsewhere).
6. **Doorbell.** RTKit message `0x83 << 48 | 2 | (priority << 2)` on endpoint 0x21.
7. **Completion.** The firmware writes the stamp value to the stamp address
   in `FinalizeCompute` and `meta`, then sends an event message
   (0x38 bytes = 4 + `EVENT_SZ` 0x34) on the event channel and rings
   `0x42 << 48` on endpoint 0x20. The host compares stamps to retire the job.

### 8.1 CDM control stream and USC words

INFERRED from MESA `cmdbuf.xml` (MIT) and `agx_cdm_launch()` in
`libagx_dgc.h`. Encodings are little-endian 32-bit words. The block type is
bits 29..31 of the first word.

| Block | Words | Fields |
|---|---|---|
| Launch, word 0 | 1 | bits 1..3 uniform register count (groups of 64, 0 = 512); 4..8 texture state count (groups of 8); 9..11 sampler state count; 12..15 preshader register count (groups of 16); 27..28 mode (0 direct, 1 indirect global, 2 indirect local); 29..31 = 0 |
| Launch, word 1 | 1 | bits 6..31 = pipeline address >> 6, relative to `usc_exec_base_cp` |
| Global size (direct, indirect-global uses an 8-byte indirect pointer instead) | 3 | X, Y, Z in threads (Mesa multiplies the workgroup count by the local size) |
| Local size (not in indirect-local mode) | 3 | X, Y, Z |
| Barrier | 1 | bit 3 USC cache invalidate, bit 27 returns, bits 29..31 = 3; other bits UNKNOWN |
| Stream link | 2 | target address hi (bits 0..7) and lo (word 1), bit 28 with-return, type 1 |
| Stream terminate | 2 | type 2 |
| Stream return | 2 | type 4 |

On G14X an extra 8-byte word follows the launch words. It is not present on
G14G, which has one cluster (MESA `decode.c`: present when generation >= 14
and the cluster count > 1).

The pipeline is a list of USC control words. Each word's low byte is its
tag. The list ends at `No preshader` (2 bytes) or `Preshader` (8 bytes).

| Tag | Word | Size | Fields |
|---|---|---|---|
| 0x1d | Uniform | 8 | bits 8..15 start (in 16-bit halves); 20..25 size in halves (0 = 64); 26..63 buffer address >> 2. Loads the uniform registers from memory. |
| 0x3d | Uniform high | 8 | same, for the upper uniform bank |
| 0x4d | Shared | 4 | bit 8 uses shared memory; 10..15 layout (0x24 vertex/compute); 24..31 bytes per threadgroup (groups of 256) |
| 0x0d | Shader | 6 | bits 16..47 = code address, relative to `usc_exec_base_cp` |
| 0x8d | Registers | 4 | bits 8..12 GPR count (groups of 8, 0 = 256); 18..21 spill size |
| 0x9d / 0xdd | Sampler / Texture | 8 | descriptor table address and count |
| 0x88 | No preshader | 2 | end of list |
| 0x38 | Preshader | 8 | bits 32..63 preshader code address; end of list |

For `vadd` (see `AGX_ISA_NOTES.md`), the shader reads its three buffer
addresses from uniforms `u0_u1`, `u2_u3` and `u4_u5`. A host therefore
needs one `Uniform` word, start 0 and size 12 halves, pointing at a table of
the three 64-bit addresses. **INFERRED** (the shader's operand use is
OBSERVED; that the hardware fills uniforms from `Uniform` words is MESA).
The simulator in step 5 executes this exact arrangement.

## 9. Cross-check: drm/asahi layouts against m1n1's independent definitions

My layout calculator (`fwlayout.py`, kept with the L214 handoff) evaluates
drm/asahi's `#[ver(...)]` conditions for G14 V13_5 and applies Rust
`repr(C)` rules. `U64`, `U32` and `GpuPointer` are packed with alignment 1,
as the driver defines them. M1N1 sizes come from `construct`'s `sizeof()`
with `AGX_GPU=G14 AGX_FWVER=V13_5`. **OBSERVED** (both tools run in
scratch).

| drm/asahi | m1n1 | Size (asahi / m1n1) | Agreement |
|---|---|---|---|
| `compute::RunCompute` | `WorkCommandCP` | 0x31c / 0x320 | Same offsets for every field checked: `microsequence` 0x1f0, `job_params2` 0x1fc, `encoder_params` 0x25c, `meta` 0x284, `command_time` 0x2b0, `unk_flag` 0x305. The 4-byte difference is tail padding: Rust ends at 0x319 and aligns to 4; m1n1 adds 7 explicit pad bytes. |
| `compute::JobParameters1` + `__pad1` | `ComputeInfo` + register fields + `unk_pad` | 0x180 / 0x180 | both end at 0x1f0 |
| `compute::JobParameters2` | `ComputeInfo2` | 0x60 / 0x60 | yes |
| `microseq::StartCompute` | `StartComputeCmd` | 0x16c / 0x16c | yes |
| `microseq::FinalizeCompute` | `FinalizeComputeCmd` | 0x7c / 0x7c | yes |
| `microseq::Timestamp` | `TimestampCmd` | 0x3c / 0x3c | yes |
| `job::EncoderParams` | `EncoderParams` | 0x28 / 0x28 | yes |
| `job::JobMeta` | `JobMeta` | 0x2c / 0x2c | yes |
| `job::TimestampPointers` | `TimeStampPointers` | 0x10 / 0x10 | yes |
| `workqueue::QueueInfo` | `CommandQueueInfo` | 0xb8 / 0xb8 | yes |
| `workqueue::RingState` | `CommandQueuePointers` | 0x70 / 0x60 | yes: drm/asahi's last 16 bytes (`cpu_freeptr`) are a host-only debugging field, per its own comment |
| `workqueue::GpuContextData` | `GPUContextData` | 0x40 / 0x40 | yes |
| `event::Notifier` | `EventControl` | 0xb0 / 0xb0 | yes |
| `channels::RunWorkQueueMsg` | `RunCmdQueueMsg` (`RunCmdQueueSize`) | 0x40 / 0x40 | yes |
| `channels::ChannelState` | `ChannelStateFields._SIZE` | 0x30 / 0x30 | yes |
| `channels::FwCtlMsg` | ring entry size | 0x14 / 0x14 | yes |
| `initdata::ChannelRing` | `ChannelInfo` | 0x10 / 0x10 | yes |
| `initdata::InitData` | `InitData` | 0xbc / 0xbc | yes |
| `initdata::RuntimePointers` | `InitData_RegionB` | 0x6bc0 / 0x6bc0 | yes |
| `initdata::Globals` | `InitData_RegionC` | 0x12394 / 0x12394 | yes |
| `initdata::HwDataA` | `AGXHWDataA` | 0x421c / 0x421c | yes |
| `initdata::HwDataB` | `AGXHWDataB` | 0x1884 / 0x1884 | yes |
| `initdata::FwStatus` | `InitData_FWStatus` | 0x80 / 0x80 | yes |
| `initdata::UatLevelInfo` | `UatLevelInfo` | 0x20 / 0x20 | yes |

The calculator was also run for G13 V12_3. The `unk_XXX` field names in
drm/asahi encode the offset in the version where the field was first named.
That check passes for most structs. The mismatches are names relative to a
parent struct (`EncoderParams`, `NotifierState`, `FwStatusFlags`) or names
taken from a newer version (`InitData` without `ver_info`). None of them is
a size disagreement. **OBSERVED** (`out/fw_g13_v12_3.txt` in the handoff
artifacts).

Both sources come from the same project (Asahi Linux), so this cross-check
catches calculator and transcription errors, not shared misunderstandings
of the firmware. The FW135 offsets in section 2.1 are the independent check
against Apple's binary.

## 10. Field tables for the compute path (G14, firmware 13.5)

Generated by `fwlayout.py` from ASAHI (`94fb23346d52`). "INFERRED" means the
offset and the name's meaning come from drm/asahi. "INFERRED offset; meaning
UNKNOWN" marks `unk_*` and padding fields: their offset is as reliable as
the rest, but nobody has published what they do. Rows marked "absent" exist
only for other firmware versions or GPUs. Sizes are hexadecimal bytes.

#### `compute::RunCompute`: 0x31c bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `tag` | `workqueue::CommandType` | INFERRED |
| 0x0004 | 0x8 | `counter` | `U64` | INFERRED |
| 0x000c | 0x4 | `unk_4` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0010 | 0x4 | `vm_slot` | `u32` | INFERRED |
| 0x0014 | 0x8 | `notifier` | `GpuPointer<'a, event::Notifier::ver>` | INFERRED |
| 0x001c | 0x4 | `unk_pointee` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0020 | 0x50 | `__pad0` | `Array<0x50, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x0070 | 0x160 | `job_params1` | `JobParameters1<'a>` | INFERRED |
| | | `registers` | `job::raw::RegisterArray` | absent for G14 V13_5 |
| 0x01d0 | 0x20 | `__pad1` | `Array<0x20, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x01f0 | 0x8 | `microsequence` | `GpuPointer<'a, &'a [u8]>` | INFERRED |
| 0x01f8 | 0x4 | `microsequence_size` | `u32` | INFERRED |
| 0x01fc | 0x60 | `job_params2` | `JobParameters2::ver<'a>` | INFERRED |
| 0x025c | 0x28 | `encoder_params` | `job::raw::EncoderParams` | INFERRED |
| 0x0284 | 0x2c | `meta` | `job::raw::JobMeta` | INFERRED |
| 0x02b0 | 0x8 | `command_time` | `U64` | INFERRED |
| 0x02b8 | 0x10 | `timestamp_pointers` | `job::raw::TimestampPointers<'a>` | INFERRED |
| 0x02c8 | 0x10 | `user_timestamp_pointers` | `job::raw::TimestampPointers<'a>` | INFERRED |
| 0x02d8 | 0x1 | `client_sequence` | `u8` | INFERRED |
| 0x02d9 | 0x3 | `pad_2d1` | `Array<3, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x02dc | 0x4 | `unk_2d4` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x02e0 | 0x1 | `unk_2d8` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x02e1 | 0x8 | `context_store_req` | `U64` | INFERRED |
| 0x02e9 | 0x8 | `context_store_compl` | `U64` | INFERRED |
| 0x02f1 | 0x14 | `unk_2e9` | `Array<0x14, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x0305 | 0x4 | `unk_flag` | `U32` | INFERRED offset; meaning UNKNOWN |
| 0x0309 | 0x10 | `unk_pad` | `Array<0x10, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x0319 | 0x3 | `(tail padding)` | `3 bytes` | INFERRED (repr(C) rule) |

#### `compute::JobParameters1`: 0x160 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x8 | `preempt_buf1` | `GpuPointer<'a, &'a [u8]>` | INFERRED |
| 0x0008 | 0x8 | `cdm_ctrl_stream_base` | `U64` | INFERRED |
| 0x0010 | 0x8 | `preempt_buf2` | `GpuPointer<'a, &'a [u8]>` | INFERRED |
| 0x0018 | 0x8 | `preempt_buf3` | `GpuPointer<'a, &'a [u8]>` | INFERRED |
| 0x0020 | 0x8 | `preempt_buf4` | `GpuPointer<'a, &'a [u8]>` | INFERRED |
| 0x0028 | 0x8 | `preempt_buf5` | `GpuPointer<'a, &'a [u8]>` | INFERRED |
| 0x0030 | 0x8 | `usc_exec_base_cp` | `U64` | INFERRED |
| 0x0038 | 0x8 | `unk_38` | `U64` | INFERRED offset; meaning UNKNOWN |
| 0x0040 | 0x4 | `helper_program` | `u32` | INFERRED |
| 0x0044 | 0x4 | `unk_44` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0048 | 0x8 | `helper_arg` | `U64` | INFERRED |
| 0x0050 | 0x4 | `helper_cfg` | `u32` | INFERRED |
| 0x0054 | 0x4 | `unk_54` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0058 | 0x4 | `unk_58` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x005c | 0x4 | `unk_5c` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0060 | 0x4 | `iogpu_unk_40` | `u32` | INFERRED |
| 0x0064 | 0xfc | `__pad` | `Pad<0xfc>` | INFERRED offset; meaning UNKNOWN |

#### `compute::JobParameters2`: 0x60 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `unk_0_0` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0004 | 0x24 | `unk_0` | `Array<0x24, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x0028 | 0x8 | `preempt_buf1` | `GpuPointer<'a, &'a [u8]>` | INFERRED |
| 0x0030 | 0x8 | `cdm_ctrl_stream_end` | `U64` | INFERRED |
| 0x0038 | 0x20 | `unk_34` | `Array<0x20, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x0058 | 0x4 | `unk_g14x` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x005c | 0x4 | `unk_58` | `u32` | INFERRED offset; meaning UNKNOWN |
| | | `unk_5c` | `u32` | absent for G14 V13_5 |

#### `job::EncoderParams`: 0x28 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `unk_8` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0004 | 0x4 | `sync_grow` | `u32` | INFERRED |
| 0x0008 | 0x4 | `unk_10` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x000c | 0x4 | `encoder_id` | `u32` | INFERRED |
| 0x0010 | 0x4 | `unk_18` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0014 | 0x4 | `unk_mask` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0018 | 0x8 | `sampler_array` | `U64` | INFERRED |
| 0x0020 | 0x4 | `sampler_count` | `u32` | INFERRED |
| 0x0024 | 0x4 | `sampler_max` | `u32` | INFERRED |

#### `job::JobMeta`: 0x2c bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x2 | `unk_0` | `u16` | INFERRED offset; meaning UNKNOWN |
| 0x0002 | 0x1 | `unk_2` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x0003 | 0x1 | `no_preemption` | `u8` | INFERRED |
| 0x0004 | 0x8 | `stamp` | `GpuWeakPointer<Stamp>` | INFERRED |
| 0x000c | 0x8 | `fw_stamp` | `GpuWeakPointer<FwStamp>` | INFERRED |
| 0x0014 | 0x4 | `stamp_value` | `EventValue` | INFERRED |
| 0x0018 | 0x4 | `stamp_slot` | `u32` | INFERRED |
| 0x001c | 0x4 | `evctl_index` | `u32` | INFERRED |
| 0x0020 | 0x4 | `flush_stamps` | `u32` | INFERRED |
| 0x0024 | 0x4 | `uuid` | `u32` | INFERRED |
| 0x0028 | 0x4 | `event_seq` | `u32` | INFERRED |

#### `job::TimestampPointers`: 0x10 bytes, align 1

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x8 | `start_addr` | `Option<GpuPointer<'a, AtomicU64>>` | INFERRED |
| 0x0008 | 0x8 | `end_addr` | `Option<GpuPointer<'a, AtomicU64>>` | INFERRED |

#### `microseq::StartCompute`: 0x16c bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `header` | `op::StartCompute` | INFERRED |
| 0x0004 | 0x8 | `unk_pointer` | `GpuWeakPointer<u32>` | INFERRED offset; meaning UNKNOWN |
| 0x000c | 0x8 | `job_params1` | `Option<GpuWeakPointer<compute::raw::JobParameters1<'a>>>` | INFERRED |
| | | `registers` | `GpuWeakPointer<job::raw::RegisterArray>` | absent for G14 V13_5 |
| 0x0014 | 0x8 | `stats` | `GpuWeakPointer<initdata::GpuStatsComp>` | INFERRED |
| 0x001c | 0x8 | `work_queue` | `GpuWeakPointer<workqueue::QueueInfo::ver>` | INFERRED |
| 0x0024 | 0x4 | `vm_slot` | `u32` | INFERRED |
| 0x0028 | 0x4 | `unk_28` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x002c | 0x4 | `event_generation` | `u32` | INFERRED |
| 0x0030 | 0x8 | `event_seq` | `U64` | INFERRED |
| 0x0038 | 0x4 | `unk_38` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x003c | 0x8 | `job_params2` | `GpuWeakPointer<compute::raw::JobParameters2::ver<'a>>` | INFERRED |
| 0x0044 | 0x4 | `unk_44` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0048 | 0x4 | `uuid` | `u32` | INFERRED |
| 0x004c | 0x104 | `attachments` | `Attachments` | INFERRED |
| 0x0150 | 0x4 | `padding` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0154 | 0x8 | `unk_flag` | `GpuWeakPointer<U32>` | INFERRED offset; meaning UNKNOWN |
| 0x015c | 0x8 | `counter` | `U64` | INFERRED |
| 0x0164 | 0x8 | `notifier_buf` | `GpuWeakPointer<Array<0x8, u8>>` | INFERRED |

#### `microseq::Timestamp`: 0x3c bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `header` | `op::Timestamp` | INFERRED |
| 0x0004 | 0x8 | `command_time` | `GpuWeakPointer<U64>` | INFERRED |
| 0x000c | 0x8 | `ts_pointers` | `GpuWeakPointer<job::raw::TimestampPointers<'a>>` | INFERRED |
| 0x0014 | 0x8 | `update_ts` | `GpuWeakPointer<Option<GpuPointer<'a, AtomicU64>>>` | INFERRED |
| 0x001c | 0x8 | `work_queue` | `GpuWeakPointer<workqueue::QueueInfo::ver>` | INFERRED |
| 0x0024 | 0x8 | `user_ts_pointers` | `GpuWeakPointer<job::raw::TimestampPointers<'a>>` | INFERRED |
| 0x002c | 0x8 | `unk_ts` | `GpuWeakPointer<U64>` | INFERRED offset; meaning UNKNOWN |
| 0x0034 | 0x4 | `uuid` | `u32` | INFERRED |
| 0x0038 | 0x4 | `unk_30_padding` | `u32` | INFERRED offset; meaning UNKNOWN |

#### `microseq::WaitForIdle`: 0x4 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `header` | `op::WaitForIdle` | INFERRED |

#### `microseq::FinalizeCompute`: 0x7c bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `header` | `op::FinalizeCompute` | INFERRED |
| 0x0004 | 0x8 | `stats` | `GpuWeakPointer<initdata::GpuStatsComp>` | INFERRED |
| 0x000c | 0x8 | `work_queue` | `GpuWeakPointer<workqueue::QueueInfo::ver>` | INFERRED |
| 0x0014 | 0x4 | `vm_slot` | `u32` | INFERRED |
| | | `unk_18` | `u32` | absent for G14 V13_5 |
| 0x0018 | 0x8 | `job_params2` | `GpuWeakPointer<compute::raw::JobParameters2::ver<'a>>` | INFERRED |
| 0x0020 | 0x4 | `unk_24` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0024 | 0x4 | `uuid` | `u32` | INFERRED |
| 0x0028 | 0x8 | `fw_stamp` | `GpuWeakPointer<FwStamp>` | INFERRED |
| 0x0030 | 0x4 | `stamp_value` | `EventValue` | INFERRED |
| 0x0034 | 0x4 | `unk_38` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0038 | 0x4 | `unk_3c` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x003c | 0x4 | `unk_40` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0040 | 0x4 | `unk_44` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0044 | 0x4 | `unk_48` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0048 | 0x4 | `unk_4c` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x004c | 0x4 | `unk_50` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0050 | 0x4 | `unk_54` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0054 | 0x4 | `unk_58` | `u32` | INFERRED offset; meaning UNKNOWN |
| | | `unk_5c_g14` | `U64` | absent for G14 V13_5 |
| 0x0058 | 0x4 | `restart_branch_offset` | `i32` | INFERRED |
| 0x005c | 0x4 | `has_attachments` | `u32` | INFERRED |
| 0x0060 | 0xd | `unk_64` | `Array<0xd, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x006d | 0x8 | `unk_flag` | `GpuWeakPointer<U32>` | INFERRED offset; meaning UNKNOWN |
| 0x0075 | 0x7 | `unk_79` | `Array<0x7, u8>` | INFERRED offset; meaning UNKNOWN |

#### `microseq::RetireStamp`: 0x4 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `header` | `op::RetireStamp` | INFERRED |

#### `microseq::Attachments`: 0x104 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x100 | `list` | `Array<MAX_ATTACHMENTS, Attachment>` | INFERRED |
| 0x0100 | 0x4 | `count` | `u32` | INFERRED |

#### `microseq::Attachment`: 0x10 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x8 | `address` | `U64` | INFERRED |
| 0x0008 | 0x4 | `size` | `u32` | INFERRED |
| 0x000c | 0x2 | `unk_c` | `u16` | INFERRED offset; meaning UNKNOWN |
| 0x000e | 0x2 | `unk_e` | `u16` | INFERRED offset; meaning UNKNOWN |

#### `workqueue::QueueInfo`: 0xb8 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x8 | `state` | `GpuPointer<'a, super::RingState>` | INFERRED |
| 0x0008 | 0x8 | `ring` | `GpuPointer<'a, &'a [u64]>` | INFERRED |
| 0x0010 | 0x8 | `notifier_list` | `GpuPointer<'a, event::NotifierList>` | INFERRED |
| 0x0018 | 0x8 | `gpu_buf` | `GpuPointer<'a, &'a [u8]>` | INFERRED |
| 0x0020 | 0x4 | `gpu_rptr1` | `AtomicU32` | INFERRED |
| 0x0024 | 0x4 | `gpu_rptr2` | `AtomicU32` | INFERRED |
| 0x0028 | 0x4 | `gpu_rptr3` | `AtomicU32` | INFERRED |
| 0x002c | 0x4 | `event_id` | `AtomicI32` | INFERRED |
| 0x0030 | 0x1c | `priority` | `Priority` | INFERRED |
| 0x004c | 0x4 | `unk_4c` | `i32` | INFERRED offset; meaning UNKNOWN |
| 0x0050 | 0x4 | `uuid` | `u32` | INFERRED |
| 0x0054 | 0x4 | `unk_54` | `i32` | INFERRED offset; meaning UNKNOWN |
| 0x0058 | 0x8 | `unk_58` | `U64` | INFERRED offset; meaning UNKNOWN |
| 0x0060 | 0x4 | `busy` | `AtomicU32` | INFERRED |
| 0x0064 | 0x20 | `__pad` | `Pad<0x20>` | INFERRED offset; meaning UNKNOWN |
| 0x0084 | 0x4 | `unk_84_0` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0088 | 0x4 | `unk_84_state` | `AtomicU32` | INFERRED offset; meaning UNKNOWN |
| 0x008c | 0x4 | `error_count` | `AtomicU32` | INFERRED |
| 0x0090 | 0x4 | `unk_8c` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0094 | 0x4 | `unk_90` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0098 | 0x4 | `unk_94` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x009c | 0x4 | `pending` | `AtomicU32` | INFERRED |
| 0x00a0 | 0x4 | `unk_9c` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x00a4 | 0x8 | `gpu_context` | `GpuPointer<'a, super::GpuContextData>` | INFERRED |
| 0x00ac | 0x8 | `unk_a8` | `U64` | INFERRED offset; meaning UNKNOWN |
| 0x00b4 | 0x4 | `unk_b0` | `u32` | INFERRED offset; meaning UNKNOWN |

#### `workqueue::RingState`: 0x70 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `gpu_doneptr` | `AtomicU32` | INFERRED |
| 0x0004 | 0xc | `__pad0` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0010 | 0x4 | `unk_10` | `AtomicU32` | INFERRED offset; meaning UNKNOWN |
| 0x0014 | 0xc | `__pad1` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0020 | 0x4 | `unk_20` | `AtomicU32` | INFERRED offset; meaning UNKNOWN |
| 0x0024 | 0xc | `__pad2` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0030 | 0x4 | `gpu_rptr` | `AtomicU32` | INFERRED |
| 0x0034 | 0xc | `__pad3` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0040 | 0x4 | `cpu_wptr` | `AtomicU32` | INFERRED |
| 0x0044 | 0xc | `__pad4` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0050 | 0x4 | `rb_size` | `u32` | INFERRED |
| 0x0054 | 0xc | `__pad5` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0060 | 0x4 | `cpu_freeptr` | `AtomicU32` | INFERRED |
| 0x0064 | 0xc | `__pad6` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |

#### `workqueue::GpuContextData`: 0x40 bytes, align 1

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x1 | `unk_0` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x0001 | 0x1 | `unk_1` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x0002 | 0x2 | `unk_2` | `Array<0x2, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x0004 | 0x1 | `unk_4` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x0005 | 0x1 | `unk_5` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x0006 | 0x18 | `unk_6` | `Array<0x18, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x001e | 0x1 | `unk_1e` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x001f | 0x1 | `unk_1f` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x0020 | 0x3 | `unk_20` | `Array<0x3, u8>` | INFERRED offset; meaning UNKNOWN |
| 0x0023 | 0x1 | `unk_23` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x0024 | 0x1c | `unk_24` | `Array<0x1c, u8>` | INFERRED offset; meaning UNKNOWN |

#### `workqueue::Priority`: 0x1c bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `.0` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0004 | 0x4 | `.1` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0008 | 0x8 | `.2` | `U64` | INFERRED offset; meaning UNKNOWN |
| 0x0010 | 0x4 | `.3` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0014 | 0x4 | `.4` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0018 | 0x4 | `.5` | `u32` | INFERRED offset; meaning UNKNOWN |

#### `event::Notifier`: 0xb0 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x8 | `threshold` | `GpuPointer<'a, super::Threshold>` | INFERRED |
| 0x0008 | 0x4 | `generation` | `AtomicU32` | INFERRED |
| 0x000c | 0x4 | `cur_count` | `AtomicU32` | INFERRED |
| 0x0010 | 0x4 | `unk_10` | `AtomicU32` | INFERRED offset; meaning UNKNOWN |
| 0x0014 | 0x9c | `state` | `NotifierState::ver` | INFERRED |

#### `event::NotifierState`: 0x9c bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `unk_14` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0004 | 0x8 | `unk_18` | `U64` | INFERRED offset; meaning UNKNOWN |
| 0x000c | 0x4 | `unk_20` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0010 | 0x4 | `vm_slot` | `u32` | INFERRED |
| 0x0014 | 0x4 | `has_vtx` | `u32` | INFERRED |
| 0x0018 | 0x20 | `pstamp_vtx` | `Array<4, U64>` | INFERRED |
| 0x0038 | 0x4 | `has_frag` | `u32` | INFERRED |
| 0x003c | 0x20 | `pstamp_frag` | `Array<4, U64>` | INFERRED |
| 0x005c | 0x4 | `has_comp` | `u32` | INFERRED |
| 0x0060 | 0x20 | `pstamp_comp` | `Array<4, U64>` | INFERRED |
| | | `unk_98_g14_0` | `Array<0x14, u8>` | absent for G14 V13_5 |
| 0x0080 | 0x4 | `in_list` | `u32` | INFERRED |
| 0x0084 | 0x10 | `list_head` | `LinkedListHead` | INFERRED |
| | | `unk_a8_g14_0` | `Pad<4>` | absent for G14 V13_5 |
| 0x0094 | 0x8 | `unk_buf` | `Array<0x8, u8>` | INFERRED offset; meaning UNKNOWN |

#### `channels::ChannelState`: 0x30 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `read_ptr` | `AtomicU32` | INFERRED |
| 0x0004 | 0x1c | `__pad0` | `Pad<0x1c>` | INFERRED offset; meaning UNKNOWN |
| 0x0020 | 0x4 | `write_ptr` | `AtomicU32` | INFERRED |
| 0x0024 | 0xc | `__pad1` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0030 | 0x0 | `_p` | `PhantomData<&'a ()>` | INFERRED |

#### `channels::RunWorkQueueMsg`: 0x40 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `pipe_type` | `PipeType` | INFERRED |
| 0x0004 | 0x8 | `work_queue` | `Option<GpuWeakPointer<super::workqueue::QueueInfo::ver>>` | INFERRED |
| 0x000c | 0x4 | `wptr` | `u32` | INFERRED |
| 0x0010 | 0x4 | `event_slot` | `u32` | INFERRED |
| 0x0014 | 0x1 | `is_new` | `bool` | INFERRED |
| 0x0015 | 0x2b | `__pad` | `Pad<0x2b>` | INFERRED offset; meaning UNKNOWN |
| | | `__pad` | `Pad<0x1b>` | absent for G14 V13_5 |

#### `channels::FwCtlChannelState`: 0x20 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x4 | `read_ptr` | `AtomicU32` | INFERRED |
| 0x0004 | 0xc | `__pad0` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0010 | 0x4 | `write_ptr` | `AtomicU32` | INFERRED |
| 0x0014 | 0xc | `__pad1` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0020 | 0x0 | `_p` | `PhantomData<&'a ()>` | INFERRED |

#### `channels::FwCtlMsg`: 0x14 bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x8 | `addr` | `U64` | INFERRED |
| 0x0008 | 0x4 | `unk_8` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x000c | 0x4 | `slot` | `u32` | INFERRED |
| 0x0010 | 0x2 | `page_count` | `u16` | INFERRED |
| 0x0012 | 0x2 | `unk_12` | `u16` | INFERRED offset; meaning UNKNOWN |

#### `initdata::InitData`: 0xbc bytes, align 4

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x8 | `ver_info` | `Array<0x4, u16>` | INFERRED |
| 0x0008 | 0x8 | `unk_buf` | `GpuPointer<'a, &'a [u8]>` | INFERRED offset; meaning UNKNOWN |
| 0x0010 | 0x4 | `unk_8` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0014 | 0x4 | `unk_c` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0018 | 0x8 | `runtime_pointers` | `GpuPointer<'a, super::RuntimePointers::ver>` | INFERRED |
| 0x0020 | 0x8 | `globals` | `GpuPointer<'a, super::Globals::ver>` | INFERRED |
| 0x0028 | 0x8 | `fw_status` | `GpuPointer<'a, super::FwStatus>` | INFERRED |
| 0x0030 | 0x2 | `uat_page_size` | `u16` | INFERRED |
| 0x0032 | 0x1 | `uat_page_bits` | `u8` | INFERRED |
| 0x0033 | 0x1 | `uat_num_levels` | `u8` | INFERRED |
| 0x0034 | 0x60 | `uat_level_info` | `Array<0x3, UatLevelInfo>` | INFERRED |
| 0x0094 | 0x14 | `__pad0` | `Pad<0x14>` | INFERRED offset; meaning UNKNOWN |
| 0x00a8 | 0x4 | `host_mapped_fw_allocations` | `u32` | INFERRED |
| 0x00ac | 0x4 | `unk_ac` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x00b0 | 0x4 | `unk_b0` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x00b4 | 0x4 | `unk_b4` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x00b8 | 0x4 | `unk_b8` | `u32` | INFERRED offset; meaning UNKNOWN |

#### `initdata::ChannelRing`: 0x10 bytes, align 1

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x8 | `state` | `Option<GpuWeakPointer<T>>` | INFERRED |
| 0x0008 | 0x8 | `ring` | `Option<GpuWeakPointer<[U]>>` | INFERRED |

#### `initdata::PipeChannels`: 0x30 bytes, align 1

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x10 | `vtx` | `ChannelRing<channels::ChannelState, channels::PipeMsg::ver>` | INFERRED |
| 0x0010 | 0x10 | `frag` | `ChannelRing<channels::ChannelState, channels::PipeMsg::ver>` | INFERRED |
| 0x0020 | 0x10 | `comp` | `ChannelRing<channels::ChannelState, channels::PipeMsg::ver>` | INFERRED |

#### `initdata::UatLevelInfo`: 0x20 bytes, align 1

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x1 | `unk_3` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x0001 | 0x1 | `unk_1` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x0002 | 0x1 | `unk_2` | `u8` | INFERRED offset; meaning UNKNOWN |
| 0x0003 | 0x1 | `index_shift` | `u8` | INFERRED |
| 0x0004 | 0x2 | `num_entries` | `u16` | INFERRED |
| 0x0006 | 0x2 | `unk_4` | `u16` | INFERRED offset; meaning UNKNOWN |
| 0x0008 | 0x8 | `unk_8` | `U64` | INFERRED offset; meaning UNKNOWN |
| 0x0010 | 0x8 | `unk_10` | `U64` | INFERRED offset; meaning UNKNOWN |
| 0x0018 | 0x8 | `index_mask` | `U64` | INFERRED |

#### `initdata::FwStatus`: 0x80 bytes, align 8

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x10 | `fwctl_channel` | `ChannelRing<channels::FwCtlChannelState, channels::FwCtlMsg>` | INFERRED |
| 0x0010 | 0x70 | `flags` | `FwStatusFlags` | INFERRED |

#### `initdata::FwStatusFlags`: 0x70 bytes, align 8

| Offset | Size | Field | Type | Status |
|---|---|---|---|---|
| 0x0000 | 0x8 | `halt_count` | `AtomicU64` | INFERRED |
| 0x0008 | 0x8 | `__pad0` | `Pad<0x8>` | INFERRED offset; meaning UNKNOWN |
| 0x0010 | 0x4 | `halted` | `AtomicU32` | INFERRED |
| 0x0014 | 0xc | `__pad1` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0020 | 0x4 | `resume` | `AtomicU32` | INFERRED |
| 0x0024 | 0xc | `__pad2` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0030 | 0x4 | `unk_40` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0034 | 0xc | `__pad3` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0040 | 0x4 | `unk_ctr` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0044 | 0xc | `__pad4` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0050 | 0x4 | `unk_60` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0054 | 0xc | `__pad5` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |
| 0x0060 | 0x4 | `unk_70` | `u32` | INFERRED offset; meaning UNKNOWN |
| 0x0064 | 0xc | `__pad6` | `Pad<0xc>` | INFERRED offset; meaning UNKNOWN |

## 11. UNKNOWN: what is not settled, and the experiment for each

| # | Unknown | Why it matters | Experiment that settles it |
|---|---|---|---|
| U1 | The macOS 26 firmware interface (FW26): InitData layout beyond the four offsets in section 2.1, and every other struct | Needed only if AienOS runs on the firmware this Mac boots today rather than a pinned 13.5 | (a) Static, read-only: continue disassembling FW26 b000 from the init routine at `0xffffff80000291c8` and map every InitData and RuntimePointers offset it reads, as section 2.1 did for FW135. (b) Dynamic: the m1n1 hypervisor trace in `docs/MACBOOK_AGX_TRACE_PROCEDURE.md` captures macOS 26's real InitData, RunCompute and channel traffic. |
| U2 | Whether the 13.5 interface is correct beyond the init handshake on this exact chip | Struct layouts are INFERRED; FW135 confirmed only the init path | Run drm/asahi (or AienOS's driver) against FW135 via a 13.5 stub boot. The first `RunCompute` either completes with the expected stamp or the firmware logs a fault on the firmware-log channel. Step 5's simulator gives the expected output buffer to compare. |
| U3 | Meanings of `unk_*` fields in the compute path (`RunCompute.unk_4`, `unk_pointee`, `JobParameters1.unk_38` = 0x8c60, `unk_58` = 1, `JobParameters2.unk_0`, `FinalizeCompute.unk_*`) | A first job only needs the values drm/asahi writes; meanings matter for tuning and for FW26 | Diff the values macOS writes (hypervisor trace, U1(b)) across Metal dispatches that change one parameter at a time (grid size, threadgroup memory, buffer count). |
| U4 | Values macOS's own driver puts in the USC words for a Metal compute dispatch (which uniforms hold which buffer) | AienOS chooses its own uniform layout, so this is needed only to replay Metal binaries unchanged | Hypervisor trace of a Metal `vadd` dispatch (step 4 procedure): decode the CDM stream and USC words with Mesa's `decode.c` (MIT) in scratch. |
| U5 | Exact role of the two firmware images `a000` and `b000` | Only `b000` runs here (OBSERVED by UUID); `a000` may serve another bin of G14G | Compare the two images' `_rtk_tunables` and code; check `uuid` on another G14G machine. |
| U6 | `ttbat-phys-addr-base` = `<eeff2700>` | Not used by drm/asahi or m1n1 | Read it on a second M2 and compare; trace whether macOS reads it (hypervisor trace of AGX kext init). |
| U7 | Meaning of each of the 5 GPU interrupt lines | drm/asahi uses RTKit mailbox interrupts, not these lines, for a first job | Hypervisor trace with AIC (interrupt controller) tracing during a compute job; note which line fires on completion and on fault. |
| U8 | Whether macOS 26 still uses endpoints 0x20/0x21 and doorbell message 0x83 (FW26 uses 0x20 for init: OBSERVED) | FW26 only | Disassemble FW26's doorbell endpoint handler (search for the endpoint number passed to the RTKit receive call) or read it from the hypervisor trace's mailbox log. |

## 12. Regenerating

Scratch tools (Python, kept with the L214 handoff in `l214-artifacts/`, not
in the repo, because AienOS carries no Python dependency):
- `fwlayout.py <drm/asahi dir> G14 V13_5 [module.Struct ...]`: layouts.
  Set `FWLAYOUT_MD=1` for the tables in section 10 and in
  `AGX_FW_LAYOUT_G14_V13_5.md`.
- `m1n1_sizes.py <m1n1/proxyclient>`: m1n1 sizes. Needs `construct` in a
  scratch venv; run with `AGX_GPU=G14 AGX_FWVER=V13_5`.
- `ipsw_fetch.py <ipsw url> <outdir> Firmware/agx/armfw_g14g.im4p`: fetches
  one file from an Apple restore image by HTTP range requests.

The firmware disassembly uses `/usr/bin/objdump --macho -d` (Command Line
Tools) on the images split out of the IM4P payload by their `rkosftab`
entries.
