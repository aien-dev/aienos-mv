# AienOS on the 2017 iMac: gap map and ordered path (L210)

Status: read-only. Sources are the repo at `18e2765` (origin/main), every `origin/feat/imac-*` and `fix/*imac*` branch, the open PR bodies on `aien-dev/aienos`, the hardware evidence folders, and the iMac handoffs in `~/handoffs`. No machine was touched for this document: no reboot, no boot-order change, no firmware load, no live iMac access. Every claim is tagged OBSERVED (read in a file, a PR body or a command output), INFERRED (with confidence) or UNKNOWN (with the experiment that resolves it). Nothing here says a row works unless it names where it was proven.

Names (Drake, 2026-10-20): Aienux is the kernel, AienOS is the system. Older files say AIENOS for both. This file follows `docs/SPARK_REPLACE_LINUX.md` (L191) in layout; the Pi twin is `docs/PI5_AIENOS_MAP.md`.

Goal (Drake, 2026-10-20): everything, hardware now and in the future. For the iMac that means every device on the board, not only the ones the Allen window uses today.

## 1. Plain-English summary

The iMac is the machine where Aienux has run on real hardware the most. Apple's own firmware starts our kernel off the iMac's internal disk by itself, in about 14 seconds from power-on (PR #418 body). Our kernel starts all four cores, reads both disks without writing, sends and receives network frames, takes a USB keyboard, draws Allen's night-sky window on the 5K screen, and plays a sound through the Cirrus audio chip (PR #415 body). Most of that is in open pull requests, not in `main`: `main` only holds the first five iMac steps (#404 to #408).

Three big things are still missing for "everything":

1. **The GPU.** The screen is drawn by the CPU into the firmware's framebuffer at 3840x2160. The Radeon is not driven at all. A display driver, a compute driver and our own shader encoder exist on branches, tested only against models of the chip and never on the real iMac.
2. **Wi-Fi, Bluetooth, Thunderbolt, SD card, camera, fans and sensors.** No code. Some have an open Linux driver to read; several are UNKNOWN until one read-only look at the machine.
3. **The model.** The inference engine only has an Arm fast path. On this Intel CPU it would run through generic code; the GPU path is the real one.

## 2. The machine, exactly

OBSERVED from `evidence/imac_boot0_2026-10-16/README.md` (a real run of `IMACDIAG.EFI` on this iMac, 2026-10-16):

| Part | Id | Notes |
| --- | --- | --- |
| Model | iMac18,3, board `Mac-BE088AF8C5EB4FA2`, Apple firmware 529.140.2.0.0, UEFI 2.40 | 27-inch Retina 5K, 2017 |
| CPU | Intel Core i5-7500 (Kaby Lake, family 6 model 0x9e), 4 cores, 1 thread each, 3.4 GHz | AVX2 and FMA, no AVX-512, invariant TSC, RDRAND/RDSEED, x2APIC |
| Memory | about 16 GB usable (`conventional_mib=16078`) | |
| GPU | AMD `1002:67df` rev c5 at 01:00.0 | Polaris 10 (Radeon Pro 570, 575 or 580: which one is UNKNOWN until the VRAM size is read). HDMI audio `1002:aaf0` |
| Intel graphics | `8086:5912` HD 630 at 00:02.0 | Not used for the panel in this report. UNKNOWN whether the panel is wired to it or to the Radeon on this board |
| Screen | GOP 3840x2160, framebuffer `0xc0300000` | The panel is 5120x2880; the firmware hands us a 4K mode. Native 5K needs the display driver (INFERRED, medium) |
| Disk 1 | SATA AHCI `8086:a102`, 1 TB HDD, 512 B blocks | The slow half of the Fusion Drive |
| Disk 2 | NVMe Samsung `144d:a806`, 26.7 GiB, 4096 B blocks | The fast half of the Fusion Drive |
| Ethernet | Broadcom `14e4:1686` (BCM57766), MAC `78:7b:8a:bd:d0:51` | |
| Wi-Fi | Broadcom `14e4:43ba` | |
| SD reader | Broadcom `14e4:16bc` | |
| USB | xHCI `8086:a12f` | |
| Thunderbolt 3 | Alpine Ridge `8086:1578`, `15d3`, `15d2`, and xHCI `15d4` | |
| ACPI | 24 tables, checksums ok, includes `DMAR` (Intel VT-d) and `MCFG` | |

OBSERVED on this Spark (a different machine) while writing: the Linux modules `brcmfmac` (licence Dual BSD/GPL) and `tg3` (GPL) carry PCI aliases for `14e4:43ba` and `14e4:1686`, and `amdgpu` carries one for `1002:67df`. `brcmfmac` lists the firmware file `brcm/brcmfmac43602-pcie.bin`. These are read-only references; their code is never copied into Aienux.

Not in the report (UNKNOWN until read): the Bluetooth part, the camera, the Cirrus audio codec's PCI parent (the HDA controller itself is Intel), the fan and temperature controller (Apple SMC), the Apple boot-ROM/security parts, and any 2017-era TPM. OBSERVED: the saved ACPI tables in `evidence/imac_boot0_2026-10-16/ACPI/` (APIC, DMAR, DSDT, ECDT, FACP, HPET, MCFG, SBST, 12 SSDTs, UEFI, XSDT) include no TPM2 table, so no firmware TPM is advertised. INFERRED (medium): keys here use the recovery-secret path like the Pi, not a TPM seal.

## 3. What is merged and what is not

| Piece | PR / branch | State (2026-10-20) |
| --- | --- | --- |
| Boot probe `IMACDIAG.EFI` | #404 | merged |
| C kernel on x86_64 (UEFI stub, GDT/IDT, page tables, APIC timer, S5 power off, GOP console) | #405 | merged |
| SATA AHCI read-only, GPT check | #406 | merged |
| All cores, NVMe read, Ethernet seen, one-line summary | #407 | merged |
| Ethernet report out (`tg3_x86.c` transmit), listener on the Spark | #408 | merged |
| Report is lab-only and allowlisted | #412 | open (desk keeps it; #425 closed as duplicate) |
| W^X page permissions, NX, CR0.WP on all cores | #426 (replaced #413) | open |
| Network loader `NETLDR.EFI` (signed kernels from the Spark) | #414 | open |
| USB keyboard, Allen's window, night sky, sound | #415 | open |
| Boot from the iMac's own disk (two-slot FAT boot section) | #418 | open |
| Keyboard unplug and replug | #421 | open |
| Screen sleep after 10 minutes, MWAIT idle, RAPL power read | #424 | open |
| Panel off, `panel_x86.c` | `feat/goodbye-panel-off` | branch, no PR seen |
| Live kernel swap and signed GPU register console | `feat/imac-gpu-bench` | branch (L142) |
| GPU core driver (display flip, memory controller read, SMU check; microcode, SDMA, compute are stubs) | `feat/imac-gpu-core`, `fix/imac-gpu-regs-verified` | branch (L143, fix L187) |
| GCN4 shader encoder, PM4 packets, interpreter | `feat/imac-gpu-shader` | branch (L144) |

Read: the merged `native/kernel/x86/` directory has 11 files; the stacked branch `feat/imac-gpu-bench` has 23; `feat/imac-gpu-core` adds a `gpu/` directory of 11 files (OBSERVED, `git ls-tree`).

## 4. The gap table

Columns: device; **Aienux today** (file, gate, and where proven: QEMU / host / real iMac); **missing**; **reference** (the document or open source that would be read first, with licence); **host-only now?**. "Real iMac" means a physical run reported in a PR body or handoff; QEMU never proves hardware.

### Class D: devices that need a driver or a kernel service

| # | Device | Aienux today | Missing | Reference | Host-only now? |
| --- | --- | --- | --- | --- | --- |
| IM1 | Boot and firmware handoff | `x86/efi_x86.c`, CHANDOF3 record, `core/handoff_check.c`. Gate `AIENOS_IMAC_KERNEL` (QEMU q35 + OVMF, two CPU models). Real iMac: BOOT-0 report 2026-10-16; kernel runs 2026-10-16 and 10-17. Apple firmware starts our internal boot section with no loader in 14420 ms (#418 body) | #418 not merged. Apple's built-in network boot (BSDP) ignores our answer, so the stick-held `NETLDR.EFI` is the way a new kernel arrives (#414 body). A kernel that hangs after ExitBootServices needs a hand on the power button (no watchdog yet) | Intel PCH watchdog/TCO documentation: DOCS to be read before code | Yes for the watchdog logic in QEMU |
| IM2 | CPU, cores, timer, interrupts | Four cores started with INIT-SIPI-SIPI (`x86/smp_x86.S`), local APIC timer, TSS/IDT. Real iMac: `smp 3/3` PASS 2026-10-17 (handoff, #413/#426 branch) | x2APIC use, MSI/MSI-X routing for devices, an I/O APIC map. Not seen in the files read: UNKNOWN whether any exists | Intel SDM (public) | Yes (QEMU) |
| IM3 | Memory protection | W^X: text read-execute, data no-execute, NX on all other RAM, CR0.WP (#426). Real iMac: `wx ok, wx_audit ok 15/15` 2026-10-17 | #426 not merged | Intel SDM | Done in QEMU; hardware proven |
| IM4 | DMA confinement (VT-d) | **None.** `DMAR` is present (OBSERVED). Handoff 2026-10-17 lists "#411 VT-d before networked sensitive use". No `vtd` file in `x86/` on any branch read | A VT-d driver: root and context tables, queued invalidation, fault handling. Without it, a network chip can write anywhere in RAM | Intel VT-d specification (public); `core/smmu.c` is the Arm pattern to mirror | Yes (QEMU `intel-iommu`) |
| IM5 | PCI and ACPI discovery | MCFG walk (`x86/pci_x86.c`), ACPI table checks, 24 tables read on the real machine | Power-management tables, AML-dependent devices (the SMC, fans and some power rails are described in AML, which Aienux does not interpret). UNKNOWN how much of the board can be driven without an AML interpreter | ACPI specification (public) | Partly |
| IM6 | SATA HDD (1 TB, the bulk store) | `x86/ahci_x86.c`: polled, read-only. QEMU PASS. Real iMac: both disks read 2026-10-16 (#407 evidence) | Writes. The write hooks always refuse (README). Fusion Drive halves are separate devices to us: no tiering, which is fine | AHCI specification (public, Intel) | Writes: yes in QEMU |
| IM7 | NVMe half (26.7 GiB, 4096 B) | Read-only via the shared NVMe driver, real iMac read 2026-10-16 | Write, flush, queue depth above 1 (the Spark's L199 does these for a different controller; the code is shared). Small size: the model cannot live here | NVMe base spec (public) | Yes |
| IM8 | The boot section on the ESP (A/B kernel slots) | `native/boot` FAT boot-slot writer (#418). Gate `AIENOS_FAT_BOOT` host PASS (61 checks per handoff 2026-10-18), QEMU `imac_bootdisk` PASS. Real iMac: Apple's FAT driver rewrote file times; tolerated at `1c91bb2`; next power-on result not in the files read: UNKNOWN | A real second boot from disk without stick or network | none | Done in QEMU |
| IM9 | Wired Ethernet | `x86/tg3_x86.c`: reset, bootcode handshake, rings, transmit; receive in the lab build (`feat/imac-gpu-bench`). Gates `AIENOS_IMAC_REPORT`, `AIENOS_IMAC_NETBOOT` (25 checks, QEMU). Real iMac: loader fetched, verified and started a signed kernel 2026-10-17 (#414 body); receive path in the kernel on hardware: UNKNOWN | Receive in the default image, DMA under VT-d (IM4), a real stack beyond ARP/IPv4/ICMP/UDP (`native/net/aienos_net.c`) | Broadcom publishes programmer guides for this family: DOCS to be located and quoted before more chip code. Linux `tg3` (GPL) is read-only reference | Yes |
| IM10 | USB keyboard | `x86/xhci_x86.c`, `dev/usb_kbd.c`, `usb_hid.c`. Gate `AIENOS_IMAC_KEYBOARD` (37 checks in #421, 50 in #424, QEMU). Real iMac: a keyboard worked and the unplug bug was found on the real machine (handoff 2026-10-18); the replug fix is NOT_RUN on hardware | Mouse/trackpad, USB hubs beyond one, mass storage, the camera, Bluetooth if it is on USB | xHCI specification (public, Intel) | Yes |
| IM11 | Screen as a framebuffer | `core/fbcon.c`, `ui` Allen window, `x86/console_x86.c`. Gate `ck_allen_ui_test` (61 checks, host). Real iMac: night-sky build ran, Drake watched (#415 body) | Real 5K (5120x2880) and a GPU-drawn picture, see IM13 | none | Done on the CPU path |
| IM12 | Panel power | `x86/panel_x86.c` drives the panel's power sequencer to turn the backlight off and on. Branch only. Real iMac: UNKNOWN | Hardware run. The register numbers were wrong in an earlier draft and were corrected in L187 (`fix/imac-gpu-regs-verified`, 164 checks vs Linux headers) | AMD register headers (Linux, MIT licence, read-only, cited per define) | Register model yes, chip no |
| IM13 | GPU display engine (Polaris 10, DCE 11.2) | `gpu/amd_display.c`: page flip and vsync, double buffer. Gate `AMD_GPU_HOST_TEST` (21 to 24 checks against a mock). Not run on the real iMac | Hardware run of the read-only steps first (memory controller aperture, VRAM size, SMU answer). Native 5K mode set. Chip-id naming is wrong in the code and docs (they say Polaris 11, `67ef`; the evidence says `67df`): fixer **L213** is correcting it | AMD publishes GCN4 ISA and Polaris register headers; Linux `amdgpu` (GPL) read-only | Model yes, chip no |
| IM14 | GPU compute path | `gpu/gcn_enc.c`: our own GCN encoder, `GCN_ENC_TEST` 283 instructions, 0 mismatches against `llvm-mc` (host). `pm4.c`, interpreter. `amd_ucode.c`, `amd_sdma.c`, `amd_compute.c` are stubs that need AMD firmware blobs (`scripts/fetch_gpu_ucode.sh`, hashes pinned, blobs not committed). Known open issues from L187: compute queue selection uses the wrong register; ring write values were checked for numbers only | Firmware load (not allowed in this task and not scheduled), GART, command ring on hardware, fences, kernels for matmul | AMD GCN4 ISA document (public), Linux `amdgpu` | Shader side: yes. Ring and microcode: needs the chip |
| IM15 | Audio | `x86/hda_x86.c`: Intel HDA controller, command rings, Cirrus CS8409 bridge path, chirp stream. Gate `ck_chirp_test` (25), QEMU kernel test with four sound checks. Real iMac: CS8409 path worked, all four amplifiers answered, chirp played (#415 body) | Microphone, HDMI audio (`1002:aaf0`, belongs to the GPU), volume policy | Intel HDA specification (public); Linux `snd-hda-codec-cs8409` (GPL) read-only | Done for output; input: yes |
| IM16 | Wi-Fi (`14e4:43ba`) | **None** | A driver and firmware. The open Linux driver `brcmfmac` (Dual BSD/GPL, OBSERVED) claims this id and wants `brcmfmac43602-pcie.bin`. UNKNOWN: whether that firmware's licence allows shipping it, whether a chip data sheet exists | Linux `brcmfmac` as reference (BSD dual-licence still means read, then write ours); firmware licence file in `linux-firmware`: not yet read | Driver logic against a chip model: yes. Firmware load: not before Drake allows it |
| IM17 | Bluetooth | **None** | The chip is not in the 2026-10-16 report. UNKNOWN whether it is on USB or the Wi-Fi card. Resolve by listing the xHCI ports (read-only) on the next lab boot | Bluetooth core specification (public) | After the lab list |
| IM18 | Thunderbolt 3 (Alpine Ridge) | **None** | NHI ring driver, tunnelling, PCIe hot-add. UNKNOWN whether Apple's firmware has already set up the controller and what a native kernel must re-do. Resolve by reading controller state read-only | Intel Thunderbolt documentation, if published: not located. Linux `thunderbolt` (GPL) read-only | After a read |
| IM19 | SD card reader (`14e4:16bc`), camera | **None** | Drivers. Camera is not in the report: UNKNOWN whether USB | none located | After the lab list |
| IM20 | Fans, temperatures, SMC | **None. Open question from Drake, 2026-10-18: does the fan work natively?** No record anywhere (handoff 2026-10-18). #424 reads the processor energy counter (RAPL) for watts, nothing for temperature or fans | An answer by measurement: fan speed and temperature over a long run. INFERRED (low): on Intel Macs the SMC runs the fans on its own default curve without help | Apple SMC is undocumented publicly: record DOCS SILENT; Linux `applesmc` (GPL) is the read-only reference | The read path can be modelled; the answer needs hardware |
| IM21 | Idle power and sleep | MWAIT idle, screen sleep after 10 min, RAPL watts line (#424). QEMU PASS; hardware NOT_RUN | The first real `power: processor X.Y W` line | Intel SDM | Done in QEMU |
| IM22 | Power off, restart | S5 power off via ACPI (merged); restart via FADT reset register with 0xCF9 fallback in the lab build (#414) | Real S3 sleep and wake, power button handling beyond tap | ACPI specification | Yes |
| IM23 | Clock and entropy | TSC timer; RDRAND/RDSEED present | Wall-clock time (RTC read, same gap as Spark D10) and network time | Intel PCH RTC registers (public) | Yes |
| IM24 | Firmware replacement (so Allen is on screen right at power-on) | Not built. The plan of 2026-10-18 (read-only backup first, lock survey, external programmer before any write) is in `~/handoffs/2026-10-18-imac-firmware-backup.md`. The step was stopped twice by an automated safeguard and is marked "do not retry" in the handoff | This map does not plan it. It stays Drake's decision with an external flash programmer. Until then the Apple logo and Apple's 14 seconds come first | Intel PCH SPI documentation (saved locally per the handoff) | n/a |
| IM25 | Intel HD 630 and the Apple security/boot parts | **None** | UNKNOWN whether they must be touched at all; leave them alone unless a measurement shows the panel or Thunderbolt depends on them | n/a | n/a |

### Class M: the model run

OBSERVED: the inference engine (`crates/aienos-infer`) has an Arm dot-product fast path (`dotprod.rs`, cfg `aarch64`); on any other CPU it takes the generic path (`dotprod.rs:435`). No x86 vector path and no iMac image build target exist (`make pi5-infer` is the only inference image target). The iMac has not run a model.

| # | Path | State | What it takes |
| --- | --- | --- | --- |
| IM26 | CPU, small model | Not built | An `x86_64` image target for the engine and an AVX2 inner loop (AVX2 and FMA are present, OBSERVED). The Pi proof (1.20 tok/s, 1B model) sets the shape: model on a raw partition named by a map file, read by the firmware before ExitBootServices. The iMac's SATA HDD or the 26.7 GiB NVMe half could hold the model. Speed vs the machine's memory bandwidth: not measured, the ceiling is UNKNOWN until measured (DDR4, read the SPD/timings) |
| IM27 | GPU, the honest path | Not built | Polaris 10 has no matrix hardware; it is GCN4 shader cores. The path is our own GCN4 kernels (encoder exists, IM14) through PM4 on a ring (needs microcode, a firmware load) with weights in VRAM. Model size is capped by VRAM: the 570 has 4 GB and the 580 has 8 GB per vendor specs (UNVERIFIED, not from this repo); the real figure is one read-only register value (`gpu: mc:` line) once IM13's first step runs. Memory bandwidth ceiling is likewise UNKNOWN until `CONFIG_MEMSIZE` and the memory clock are read; do not quote a tok/s target before then |
| IM28 | Verdict | | The iMac is a test bench for the AMD GPU path and for Allen's presence, not the machine that runs the big model. A small model on CPU first is the cheaper first proof; the GPU path is the part of this map that is new engineering |

## 5. Role on the network of AienOS machines

OBSERVED in the repo:

- Pairwise secure transport: `native/net/aienos_sec.{h,c}`, a three-message SIGMA-style handshake (X25519, Ed25519 identities, HKDF, AES-256-GCM-SIV records). Each side is told one peer public key (a pinned roster of one). Only TEST identities exist; real keys wait for the owner-key chain. An owner-key ceremony was PERFORMED for the Pi on 2026-10-14 (`docs/native/PI5-8-OWNER-CEREMONY.md`); the Spark map lists the hardware-image ceremony as BLOCKED_OPERATOR. Whether the Pi key is the root for other machines' transport identities: UNKNOWN.
- A reliable channel inside it: `native/net/aienos_ctl.c`. Plain UDP, IPv4, ARP, ICMP below: `aienos_net.c`. No TCP, no TLS, no DNS, no DHCP in `native/` (L191 grep).
- Lab wire formats already on the iMac: UDP 47031 report (unauthenticated, lab only), 47032/47033 netboot (kernels checked with Ed25519 before start), 47034 live kernel swap (signed slot), 47035 GPU register console (HMAC-SHA256 plus counter) on `feat/imac-gpu-bench`.
- The word "gateway" does not appear in `native/net`, `docs/adr` or `docs/*.md`. A gateway is a design item, not code.

Role for the iMac (INFERRED, high): the **test bench and Allen's display**. The Spark serves signed kernels to it over the LAN; the iMac answers with reports and takes the signed GPU console. It is a leaf. It does not need to be a gateway, a relay or a time source. It becomes a **peer of the Spark** once the pinned roster supports more than one peer; today every pair is a separate one-peer pairing.

Needed on the wire before the iMac is a real network member: real identities (ceremony), a roster of more than one peer, receive in the default image, VT-d (IM4), a stack that can carry the model-server traffic if the iMac ever serves one.

## 6. Ordered path

Notation as in the Spark map: **[H]** host or QEMU only, a builder can do it now; **[A]** needs the iMac with the lab stick and a power-on; **[O]** Drake only; **[C]** needs the chip.

1. [H] Land the open iMac stack in order (the desk owns the PRs): #412, #426, #414, #415, #418, #421, #424. Until then `main` understates the machine.
2. [H] Finish **L213** (chip name and per-chip constants) and re-run `scripts/gpu_bench/check_regs.sh` on the corrected tree.
3. [H] VT-d driver against QEMU `intel-iommu` (IM4). Receive path in the default image.
4. [A] One lab boot that is read-only everywhere: list the xHCI ports (Bluetooth, camera), read the GPU's memory controller and VRAM size, read Thunderbolt state, report the power line. This resolves IM17, IM19, IM13's first step and the RAPL watts, with no write to any device.
5. [A][O] Fan and temperature question (IM20): a long run with a human listening and a temperature read, per the `imac-hardware-manual` skill.
6. [H] x86 AVX2 inner loop for the inference engine and an iMac image target (IM26); [A] first CPU token on the iMac, reported against a measured memory ceiling.
7. [H][A] GPU display: 5K mode set and a GPU-drawn frame, display first, compute later (IM13).
8. [H] Wi-Fi driver against a chip model and a firmware inventory with licences (IM16); [A] hardware only after Drake allows the firmware load.
9. [H] Thunderbolt and SD, only if step 4 shows they are reachable (IM18, IM19).
10. [C][O] GPU compute: microcode and rings on hardware, first fill kernel, then matmul (IM14). Needs Drake's explicit go for the firmware load.
11. [H] Real keys and a multi-peer roster (section 5).

## 7. Operator steps (Drake only)

None of these is authorized by this document.

- Power the iMac on once per lab kernel with the stick in. The queued kernel and the report path are described in the PR bodies.
- Tell the orchestrator what the screen shows, or let the report arrive over the LAN.
- Look at the machine: is the fan audible and does it ramp (IM20)?
- Decide on the flash chip and an external programmer (IM24). This map neither plans nor prepares it.
- Allow the first GPU firmware load when step 10 is reached.
- The owner-key ceremony, same as the Spark.

## 8. Limits of this document

- No test was run for this file. Gate names and results are quoted from PR bodies and handoffs and were not re-run. A PR body is a claim by its author; the PRs are open.
- The branches were read with `git show` and `git ls-tree` on `origin/*` refs only. No other worktree was touched.
- Reuse search: `origin/main`, every `origin/feat/imac-*` and `fix/*imac*` branch, open PR bodies #412 to #426, `~/handoffs` (five iMac files), the ledger (L040, L142 to L145, L187, L210), and `modinfo` on this machine for `brcmfmac`, `tg3`, `amdgpu`. The 2026-10-16 evidence folders `imac_boot0`, `imac_kernel0`, `imac_kernel1` exist; only `imac_boot0` was read in full.
- The iMac was not contacted. The hardware table is the 2026-10-16 report, 4 days old.
- No speed, size or time estimate is given because none was measured, except where a receipt id is named.

## 9. Joining the tailnet (Drake's idea of 2026-10-20, mind map `2026-10-20-aienos-on-the-imac-joins-tailscale`)

A native tailnet node needs the WireGuard data plane and Tailscale's control protocol, which runs over TCP and TLS; none exists in `native/` (L191 grep). INFERRED (medium): the near-term route is the Linux hub (`docs/PI5_AIENOS_MAP.md` section 4) advertising the home LAN so the Spark and the Macs reach the iMac at its LAN address, with the iMac speaking the signed UDP transport. The native node waits on TCP, TLS and a vetted WireGuard implementation (crypto is never hand-rolled).
