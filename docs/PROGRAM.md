# AienOS program map: every machine, every rung, the network between them, and what comes after (L211)

Status: REVISION 2, 2026-10-20. Written from the repo at `18e2765`, the ledger, the hand-off notes and all four machine maps (Spark L191, iMac and Pi L210, MacBook L208, PC L209). Every row names the map row it came from. Nothing here is a claim that a row works unless the "Proven where" column says so.

Names (Drake, 2026-10-20): **Aienux** is the kernel, **AienOS** is the system. Older files still say AIENOS for both.

Tags on every claim: OBSERVED (read from source, a record or a live box), INFERRED (with confidence), UNKNOWN (with the experiment that would settle it). Nothing unmeasured is called done. No time, effort or speed estimate appears on this page, because none has a receipt.

| Map | Machine | Ledger | State on 2026-10-20 |
| --- | --- | --- | --- |
| `docs/SPARK_REPLACE_LINUX.md` | DGX Spark (GB10, aarch64) | L191 | LANDED (READY `bb2399f`, branch `feat/spark-replace-linux-map`) |
| `docs/IMAC_AIENOS_MAP.md` | iMac 2017 (x86_64) | L210 | LANDED (READY `97a0945`, branch `feat/aienos-imac-pi-map`) |
| `docs/PI5_AIENOS_MAP.md` | Raspberry Pi 5 (hub) | L210 | LANDED (same branch and commit) |
| `docs/MACBOOK_AIENOS_MAP.md` | MacBook Air M2 (Apple Silicon) | L208 | LANDED (READY `1d77025`, branch `feat/aienos-macbook-map`) |
| `docs/PC_AIENOS_MAP.md` | Ballentine_PC (Ryzen 5 7600X, RTX 5070, Windows 11) | L209 | LANDED (READY `eb552cb`, branch `feat/aienos-pc-map`) |

## 1. The whole shape in plain English

AienOS is one operating system meant to run on five different computers. Each computer climbs the same twelve-step ladder from "the chip powers on" to "another machine can ask it for an answer". The lower rungs are drivers (disk, network, screen, keyboard). The upper rungs are the graphics chip and the AI model. A computer is "done" when every rung has been proven **on the real machine**, not only in the emulator (QEMU), and Linux (or macOS or Windows) can be switched off.

Where each machine stands today, from the records:

- **iMac 2017**: furthest on real hardware. Our kernel has booted on it from a stick and fetched a signed kernel over the network, started its cores, turned on memory protection, read both disks, drawn ALLEN on the 4K screen, taken a USB keyboard and played sound. Most of that sits in open pull requests, not in `main`. The graphics chip (a Polaris 10 Radeon) driver exists but has never run on the chip. No model runs on it natively.
- **Raspberry Pi 5**: our kernel has booted on it, read and written its SD card, kept a sealed store across boots, and run a small language model on its own processor at 1.20 tokens per second. Network send works, receive has not been seen, and nobody can type on it yet (no USB or serial input). It is also the always-on Linux hub that runs the private network coordinator, so native runs are short windows.
- **DGX Spark**: our kernel booted once, for 75 seconds, in September, and only printed a report. Everything since is emulator work plus drivers in flight. The Spark is the machine that has to run the 27B model, so it carries the longest ladder (the GPU path).
- **MacBook Air (M2) and the PC (Ryzen 5 7600X, RTX 5070)**: no AienOS code has run on either. The Mac is a different kind of computer (Apple boot chain, interrupt controller and GPU), reachable only by a tethered boot over a cable from the Spark first. The PC keeps Windows and is an ordinary x86 machine whose chips are not yet inventoried.

Three things hold for the whole program:

1. **Some Linux programs will never run on AienOS.** Claude and Codex agents, git, compilers, Docker, the Tailscale and Headscale services and the test judge are Linux programs with no native counterpart and no plan for one (Spark map, rows U1 to U6). "Linux off" on a machine means that machine runs only AienOS code. The work that needs those programs lives on a machine that keeps Linux. This is a physical boundary, not a gap to close.
2. **The network between machines has one real piece today**: a signed, encrypted transport (section 4). It is proven in the emulator with test keys only, and it knows exactly one peer. Both limits matter for "many machines". There is no TCP or TLS at all, which is why a gateway machine is needed.
3. **Omega is the destination language** (ADR 0024). Rust is scaffolding, C and assembly only where hardware forces them. The future section (section 6) says how the kernel itself moves to Omega.

## 2. The ladder and the tables

The twelve rungs, bottom to top: **1 boot, 2 CPU, 3 memory protection, 4 disk, 5 network, 6 input, 7 display, 8 trust (TPM or equivalent), 9 GPU firmware, 10 GPU compute, 11 model run, 12 service API.**

Columns: **What** is the rung, **Gate** is the named test that would prove it, **Proven where** is QEMU, HARDWARE (real machine) or NONE, **Ledger** is who owns the next step or `unowned`. Gate names come from `native/kernel/GATES.md` and the machine maps; `no gate yet` means the rung has no named test, which is itself a gap.

### 2.1 DGX Spark (GB10): source `docs/SPARK_REPLACE_LINUX.md` (L191, landed)

Pin: repo `18e2765`, live box `spark-b87b` read 2026-10-20. Row ids (D, G, U) point at that map's gap table.

| Rung | What | Gate | Proven where | Ledger |
| --- | --- | --- | --- | --- |
| 1 boot | C kernel started from firmware, device list matches the box, return to Linux unharmed (D1) | `AIENOS_CK_M1`, `M0_ROLLBACK` (NOT_RUN for the C loader); attended boot 1 | HARDWARE once (2026-09-24, handoff image, `M2_GATE: PASS`, 75 s); C kernel QEMU only | L197 (boot package, in flight); attended boot is Drake |
| 1 boot | Installer and rollback-safe A/B slots (path item 17) | `SLOT_LOADER` (N7), `DISK_LAYOUT` | QEMU for slots; `DISK_LAYOUT` NOT_RUN | unowned |
| 2 CPU | MMU, EL2 to EL1, GICv3, timer, threads, EL0, capabilities, IPC, 20 cores in two classes (D2) | `M1`, `M3`, `SMP` | QEMU; `SMP` NOT_RUN; physical NONE | L201 (baseline re-run) |
| 2 CPU | Power, thermal, fans, CPU frequency (D11) | no gate yet | NONE; UNKNOWN who holds fans if no OS does | unowned |
| 2 CPU | Clock, RTC, watchdog (D10) | TPM+RTC gate (L200) | QEMU timer and RNDR; RTC NONE | L200 |
| 3 memory protection | Two SMMUv3, DMA confinement, two-level stream table (D3) | `SMMU`, `NVME_SHUTDOWN` | QEMU PASS; real Spark IORT shape host case owed | L201 |
| 3 memory protection | GB-scale pinned regions, huge pages, 27B weight image (G5); the DMA pool is 8 MiB today | no gate yet | NONE | unowned |
| 4 disk | NVMe `144d:a810`, 512-byte blocks: read, write, flush; queue depth above 1, direct DMA (D4) | `QEMU_NVME`, `QEMU_NVME_RW`, `M4_NVME`, `STORE_512B_CRASH_RECOVERY_QEMU` | QEMU PASS, depth 1 only; never driven on this controller | L199 |
| 4 disk | Move data off ext4 and the Atlas TPM-sealed mounts (P3) | no gate yet | NONE | unowned |
| 5 network | Realtek `10ec:8127` 10 GbE, UDP report out (D5) | RTL8127 model gate + QEMU report gate | NONE (no driver exists; org-check found 0 hits) | L198 |
| 5 network | ConnectX: zero `15b3` devices seen on this unit (D5, section 3 of the map) | read link state of both root ports, read-only | UNKNOWN | L204 (Drake looks at the unit) |
| 5 network | Wi-Fi MT7925, Bluetooth, HDA audio (D13): Drake reversed the drop and asked for drivers | model gates | NONE | L206 (Wi-Fi), L207 (Bluetooth, audio) |
| 6 input | Six platform xHCI controllers, USB keyboard (D6) | `KEYBOARD` | QEMU PASS; physical NONE; UNKNOWN whether the MediaTek glue is powered when we first touch it | unowned |
| 6 input | Serial console via SPCR (D8) | line on a serial capture | QEMU; NONE; UNKNOWN whether a header exists | L204 |
| 7 display | UEFI framebuffer console (D7) | photo of the console on the HDMI | QEMU; NONE (first boot showed `gb10: unavailable`) | L197 (checklist), attended boot |
| 8 trust | TPM 2.0 CRB driver, PCR policy, NV anti-rollback counter (D9) | `TPM2_Startup`, `GetRandom`, NV read against swtpm | NONE | L200 |
| 8 trust | Owner key ceremony and Secure Boot on for release builds (P1) | TRUST-1 | BLOCKED_OPERATOR | Drake (section 7) |
| 9 GPU firmware | GSP boot bytes and RPC subset (G2, cut C) | host byte-exact tests | host only, in flight | L192 |
| 9 GPU firmware | Kernel finds the GB10 and owns BAR0 (G1) | `BOOT_0` read after ExitBootServices | NONE | unowned |
| 9 GPU firmware | GPU interrupts, or a written reason polling suffices (G4) | fault or completion seen | NONE | unowned |
| 10 GPU compute | Page tables, allocator, channel and doorbell model (G3, cut D) | simulated device end to end, then one vector add on the chip with teardown and reopen | host simulator, in flight; chip NONE | L193 |
| 10 GPU compute | Omega platform services, typed handles, freestanding link (G6, cut A) | omega gates green hosted and freestanding | host | L202 |
| 10 GPU compute | Matmul, attention, launchers, session, resident runtime in Omega (G6, cut E) | receipts against the Linux reference | NONE | unowned |
| 11 model run | CPU inference on the Spark (`aienos-infer`) | tok/s against the memory ceiling (ceiling not measured) | NONE on Spark; Pi 5 proves the path | unowned |
| 11 model run | 27B weights from NVMe, KV on the GPU, token stream (G7) | a token stream from the Spark | NONE | unowned |
| 12 service API | Request path from another machine (G8) | a request answered by the Spark | QEMU for the transport; no TCP or TLS exists | unowned (section 4) |

### 2.2 iMac 2017 (iMac18,3, Core i5-7500, x86_64): source `docs/IMAC_AIENOS_MAP.md` (L210, landed `97a0945`)

Row ids (IM) point at that map. Most iMac work sits in **open pull requests**, not in `main`: `main` holds only #404 to #408. The desk owns those PRs (L040 is the iMac bundle). Hardware results are transcriptions from photos or reports recorded in the evidence folders and PR bodies; a PR body is its author's claim.

| Rung | What | Gate | Proven where | Ledger |
| --- | --- | --- | --- | --- |
| 1 boot | UEFI stub, Apple firmware starts our kernel; own-disk boot with no stick (#418, Apple started it in 14.4 s); signed network loader (#414: fetched, verified and started a signed kernel) (IM1, IM8) | `AIENOS_IMAC_KERNEL`; `AIENOS_FAT_BOOT` (61 checks, host); `AIENOS_IMAC_NETBOOT` (25, QEMU) | HARDWARE for stick boot (2026-10-16, 10-17) and netboot (10-17); own-disk second boot UNKNOWN (Apple's FAT driver rewrote file times, tolerated at `1c91bb2`, next power-on result not in the files) | L040 (desk, PRs #414, #418) |
| 2 CPU | 4 cores, APIC timer, TSS and IDT (IM2); idle and RAPL watts (#424); wall clock (IM23) | `smp`, kernel gate | HARDWARE: `smp 3/3` PASS 2026-10-17; #424 NOT_RUN on hardware; RTC NONE | L040; RTC unowned |
| 3 memory protection | W^X, NX, CR0.WP (#426, replaced #413) (IM3) | QEMU mutation probes | HARDWARE 2026-10-17: `wx ok`, audit 15 of 15; PR open | L040 |
| 3 memory protection | VT-d DMA confinement (IM4): `DMAR` exists, no driver; without it a network chip can write anywhere in RAM | none yet | NONE | unowned |
| 4 disk | SATA HDD 1 TB and the 26.7 GiB NVMe half of the Fusion Drive, read-only (IM6, IM7); writes always refused by the build | `AIENOS_IMAC_KERNEL` disk lines | HARDWARE read-only (2026-10-16) | write path and Store: unowned |
| 5 network | BCM57766: transmit and report on UDP 47031 (#408, merged); receive in the lab build only; signed netboot ports 47032 and 47033; live swap 47034 and GPU register console 47035 on `feat/imac-gpu-bench` (IM9) | `AIENOS_IMAC_REPORT`, `AIENOS_IMAC_NETBOOT` | HARDWARE for report send and netboot fetch; receive in the kernel on hardware UNKNOWN; bench QEMU test NOT_RUN at last note | L142 |
| 5 network | Wi-Fi `14e4:43ba`, Bluetooth, Thunderbolt 3, SD reader, camera (IM16 to IM19) | none | NONE; Bluetooth and camera ids UNKNOWN until one read-only lab boot lists the xHCI ports | unowned |
| 6 input | USB keyboard through xHCI (#415); replug fix (#421) (IM10) | `AIENOS_IMAC_KEYBOARD` (37 checks in #421, 50 in #424, QEMU) | HARDWARE: a keyboard worked and the unplug bug was found on the real machine; replug fix NOT_RUN on hardware | L040 |
| 7 display | CPU-drawn 4K framebuffer, ALLEN night-sky window (IM11); panel power (IM12); sound through the Cirrus codec worked on hardware (IM15, #415) | `ck_allen_ui_test` (61, host), `ck_chirp_test` (25) | HARDWARE: the window ran and Drake watched; the panel is 5120x2880 and the firmware gives 4K | L040, L076; panel power unowned |
| 7 display | Native 5K mode set and a GPU-drawn frame (IM13) | `AMD_GPU_HOST_TEST` (21 to 24 against a mock) | host only | L143; chip naming fix L213 |
| 8 trust | TPM or any secure element: UNKNOWN (not in the 2026-10-16 report); owner-key ceremony same as the Spark | none | NONE | unowned until a read answers |
| 9 GPU firmware | Polaris 10 (`1002:67df`, Radeon Pro 570, 575 or 580): memory controller read, SMU answer, microcode and rings are stubs needing AMD blobs (IM13, IM14); the code and docs name Polaris 11, which is wrong (L213 fixing) | `check_regs.sh` (164 checks exit 0 after L187, 87 of 88 numbers had been wrong) | host only; chip NONE; firmware load needs Drake's go | L143, L187, L213 |
| 10 GPU compute | Our own GCN 4 encoder, PM4 packets, interpreter, fill, blit and sky-lens kernels (IM14) | `GCN_ENC_TEST`: 283 instructions, 0 mismatches against `llvm-mc` | host only | L144, L145 |
| 11 model run | No x86 inference path exists: the engine has an Arm fast path only; needs an AVX2 loop and an image target (IM26); GPU path capped by VRAM (4 or 8 GB by vendor spec, UNVERIFIED) (IM27) | none yet | NONE; verdict of the map: the iMac is a test bench for the AMD GPU path and Allen's display, not the big-model machine | unowned |
| 12 service API | ALLEN answers through the Spark's 27B over the UDP courier (L039); native role is a **leaf**: serves nothing | QEMU end to end with the 27B | QEMU PASS | L039 (done) |

Also unplanned and not retried: firmware replacement (IM24). Stopped twice by an automated safeguard, marked do-not-retry in `~/handoffs/2026-10-18-imac-firmware-backup.md`.

### 2.3 Raspberry Pi 5, the "hub": source `docs/PI5_AIENOS_MAP.md` (L210, landed `97a0945`)

Row ids (PI) point at that map. The Pi is two things: a board where Aienux has run on real hardware, and the always-on Linux hub that runs the **Headscale** tailnet coordination server, **Pi-hole** DNS, **nginx**, a **Forgejo** git server in Docker, a vault and the Discord bots (OBSERVED, ssh on 2026-10-20). Every native run so far is a one-time window after which the Pi goes back to Linux; while a window is open the tailnet coordination is down.

| Rung | What | Gate | Proven where | Ledger |
| --- | --- | --- | --- | --- |
| 1 boot | `BOOTAA64.EFI` by one-shot tryboot, eleven image variants build byte-identical twice (PI1) | `CK_PI5_BUILD: PASS`, `ck_pi5_repro_check.sh` | HARDWARE (2026-10-13 and later runs) | done |
| 2 CPU | Cortex-A76 x4, GICv2, timer, PM watchdog 15 s (PI2, PI11); whether cores 2 to 4 start: UNKNOWN; clock RTC none (PI13); fan and thermal none, firmware left as found (PI14) | build checks | HARDWARE for boot and watchdog | RTC, thermal: unowned |
| 3 memory protection | No IOMMU: ADR 0020 software confinement with a 2 MiB bounce window (PI8); confinement A BLOCKED (canary 0 changed bytes), B INFERRED only | NET-3 runs | HARDWARE partial | unowned |
| 4 disk | microSD host, PIO, 25 MHz (PI3); sealed Store v1 survives boots (PI4); mid-commit power cut on the real card is QEMU only | `pi5-sd1a/b`, `pi5-store1..4` | HARDWARE | done; power-cut test needs Drake |
| 5 network | Gigabit MAC: loopback PASS twice, one ARP frame sent PASS; **receive of an incoming frame not seen in 400 ms** (PI9); Wi-Fi, Bluetooth none (PI17, PI18) | `pi5_gem`, `tests/test_pi5_gem.c` | HARDWARE for send; branch `pi5/net-398-wire` unmerged | unowned |
| 6 input | No USB (RP1 xHCI) and no UART input, so a person cannot type the unlock secret (PI5, PI6, PI12) | none yet | NONE | unowned |
| 7 display | HDMI native: none, DOCS to be searched (PI16) | none | NONE | unowned |
| 8 trust | No TPM. Slot 1 recovery-secret unlock runs on hardware but the secret comes from a labelled card file (qualification only); owner key ceremony performed 2026-10-14 on the YubiKey | `PI5-UNLOCK-QUALIFICATION`, PI5-8 Q2 | HARDWARE for the qualification tier | production tier unowned |
| 9 to 10 GPU | VideoCore (V3D): no driver, no plan; shares the same memory bus so gain is bounded (PI-M4) | none | NONE | not planned |
| 11 model run | Llama-3.2-1B Q4_K_M: **1.20 tok/s** (`calc:591b5dec...`), scalar was 0.22; TinyLlama 1.462, danube3-500m 3.227. Vendor-figure ceiling 21.13 tok/s (UNVERIFIED, `calc:166ba5b3...`), measured is about 5.68% of it | `PI5-6-INFER-RUNS.md` | HARDWARE; what bounds it UNKNOWN (one thread as far as logs show) | multi-core and ceiling measurement: unowned |
| 12 service API | Signed units, agent lifecycle, ALLEN REFLEX, Cortex memory survives cold boot (PI21) | `PI5-5/6/7-*-RUNS.md` | HARDWARE | done |

Hub services and what they need if the Pi itself ran Aienux: Headscale, nginx, Forgejo, vault, bots and the tunnels all need TCP and TLS or are Linux programs, so they must move or be rebuilt; Pi-hole DNS is possible over UDP and is the proposed first native service.

### 2.4 MacBook Air M2 (Mac14,2, board J413AP, macOS 26.6.2): source `docs/MACBOOK_AIENOS_MAP.md` (L208, landed `1d77025`)

Row ids (D, G, F) point at that map. Nothing from AienOS has run on this machine. It is not a UEFI machine: Apple's own boot chain, interrupt controller (AIC2), device IOMMU (DART), disk controller (ANS2) and GPU (AGX G14G), almost all behind Apple helper processors spoken to through a mailbox protocol (RTKit). Apple allows a third-party kernel under a per-install "Permissive Security" setting; Asahi Linux has reverse engineered nearly every chip, so most rows are "documented by a third party" (licences GPL-2.0-only OR MIT, read and then written ours, never copied). **This Mac is the owner-key ceremony machine, with Activation Lock on** (OBSERVED), so which Mac to use is Drake's decision.

| Rung | What | Gate | Proven where | Ledger |
| --- | --- | --- | --- | --- |
| 1 boot | Tethered boot: the Spark sends the kernel over USB-C into RAM through m1n1 each time, nothing stored on the Mac; needs a device-tree parser and a raw `Image` entry (D1) | boot #1 proxy read-only, boot #2 `kernel: alive` on the m1n1 virtual UART | NONE | unowned |
| 2 CPU | 8 cores in two clusters via spin-table (D2); AIC2 interrupts, FIQ timer, fast IPI (D3); SMC for reset, off and RTC (D9) | `SMP`, chip models | NONE; host models feasible | unowned |
| 3 memory protection | DART IOMMU and SART (D6): 16 KiB pages, security-critical | chip model tests | NONE | unowned |
| 4 disk | ANS2 NVMe behind the ANS helper: one admin and one I/O queue, depth 64 (D4); the macOS APFS container is never mounted or written; test region or a separate USB SSD (Drake) | NVMe read-only at boot #4 | NONE | unowned |
| 5 network | None native; USB Ethernet (AX88179B, DOCS SILENT) first, which needs USB-C PD, the ATC PHY and a DWC3 host (D7, D10); Wi-Fi and Bluetooth need APCIe plus Broadcom firmware extracted from macOS (D11 to D13, F4) | UDP report as on the iMac | NONE | unowned |
| 6 input | Keyboard and trackpad via the MTP helper and dockchannel (D8); until read in full the tether console is the input | `keyboard: ready` | NONE | unowned |
| 7 display | iBoot framebuffer first (D14); the real DCP/IOMFB path is firmware-version-specific and not needed early (D15) | framebuffer text at boot #3 | NONE | unowned |
| 8 trust | No TPM. The SEP stays; anti-rollback candidates: SEP or an owner-held network counter, UNKNOWN (F1 to F3) | none yet | NONE | unowned |
| 9 GPU firmware | `gfx-asc` helper is booted by iBoot; RTKit client, GPU DART, UAT page tables (same format as ARM64, 16 KiB, 40-bit VA), firmware init structures marked unfinished by Asahi (G1 to G3). First step is record, do not send | m1n1 hypervisor trace | NONE | L214 (reverse engineering on the Mac, branch `feat/macbook-agx-reverse-engineering`) |
| 10 GPU compute | **DOCS SILENT** on a compute-only path (G4); instruction set known only from Asahi's userspace reverse engineering | integer vector add, teardown and reopen | NONE | L214 |
| 11 model run | None; the map does not claim the chip can or cannot; speed would be against the chip's published memory bandwidth, not measured | none | NONE | unowned |
| 12 service API | None; the Mac is not a replacement for any Spark function | none | NONE | unowned |

### 2.5 Ballentine_PC (Ryzen 5 7600X, RTX 5070 12 GB, Windows 11, Windows kept): source `docs/PC_AIENOS_MAP.md` (L209, landed `eb552cb`)

Row ids (PD, PG) point at that map. Nothing from AienOS has run here. The PC is an ordinary x86 UEFI machine, so the iMac work (almost all of it still on unmerged branches) is the base. The motherboard, network chip, storage controller and USB controllers are **not known yet**: one read-only PowerShell command (L212, Drake) fixes that. Nothing is planned around a guess.

| Rung | What | Gate | Proven where | Ledger |
| --- | --- | --- | --- | --- |
| 1 boot | UEFI stub from a USB stick chosen once in the firmware boot menu, no boot entry created; Secure Boot off or our key enrolled beside Microsoft's; **save the BitLocker recovery key first** (PD1, PD14) | `kernel: alive`, Windows back unchanged | QEMU only | L212 first |
| 2 CPU | `AuthenticAMD` paths, x2APIC, 6 cores and 12 threads, no HPET assumption, power meter (PD2) | `cpu` and `smp` lines | QEMU with an AMD CPU model | unowned |
| 3 memory protection | AMD-Vi, not VT-d (PD3): `IVRS` parser, device table. Real risk: firmware may leave it enabled, so DMA from a driver that never configured it could be blocked or redirected. First boot reads the IOMMU control register and reports it before any DMA | IOMMU state printed; a device granted a range only | NONE | unowned |
| 4 disk | NVMe read-only first (PD4); write path only on a partition of its own type, or the USB stick or a second drive; Windows `C:` never touched without Drake's approval of a disk model | GPT and Store superblock read, no write | QEMU for the driver | L199 shares the queue-depth work |
| 5 network | The PC's chip is UNKNOWN (Realtek or Intel guessed, low to medium); one frame out seen on the LAN (PD5); Wi-Fi and Bluetooth follow Drake's Spark decision (PD13) | UDP report | NONE | unowned |
| 6 input | USB keyboard through xHCI, AMD quirks (PD6); serial likely absent on AM5 (PD9) | `keyboard: ready` | QEMU | unowned |
| 7 display | GOP console first (PD7); native modesetting for the 5070 only if Drake wants it (PG7) | photo | QEMU | unowned |
| 8 trust | TPM CRB: the Spark driver (L200) may carry over; AMD firmware TPM behaviour UNKNOWN (PD10) | `TPM2_GetRandom` from the kernel | swtpm only | L200 |
| 9 GPU firmware | RTX 5070 (`10de:2f04`, GB202 family) boots its GSP through **FSP**, which the GB10 does not have, so L192 covers the image family, reset step and page-table format but the opening handshake is an extra piece (PG1, PG2) | GSP alive and a channel reply | host encoder only | L192 partly; FSP unowned |
| 10 GPU compute | Page tables and channels (PG4, shares L193), memory in two pools with BAR1 (PG3), interrupts (PG5), DMA confinement (PG6) | vector add, clean teardown and reopen | simulator | L193; the rest unowned |
| 11 model run | CPU baseline first against a measured DDR5 ceiling (not measured); a 27B model does **not fit in 12 GB**, so it must be streamed, or a smaller model resident (PG8, section 5.3 of that map) | token stream, speed against ceilings | NONE | unowned |
| 12 service API | Joins the iMac's lab path: signed kernels in, reports out (ports 47034 and 47035); adds a discrete GPU and Windows alongside | request answered | NONE | unowned |

## 3. What the machines share (so work is built once)

| Shared piece | Used by | Where it lives | State |
| --- | --- | --- | --- |
| Kernel core (memory, scheduler, capabilities, IPC, Store, recovery, M5 crypto, signed slots) | all | `native/kernel/core`, `native/store`, `native/m5` | QEMU PASS; hardware on iMac and Pi, Spark once |
| NVMe driver | Spark, iMac, PC | `native/disk/disk_nvme.c`; queue depth work L199 | QEMU PASS; iMac HARDWARE read-only |
| x86 kernel (cores, PCI, AHCI, xHCI, HDA, report) | iMac, PC | `native/kernel/x86` on `main` is 11 files; 23 on `feat/imac-gpu-bench` | iMac HARDWARE; PC needs vendor-neutral changes |
| xHCI and keyboard classes | Spark, iMac, Pi (RP1), PC, Mac (DWC3) | `dev/xhci_fence.c`, `usb_kbd.c`, `usb_hid.c` | QEMU; iMac HARDWARE |
| TPM CRB driver | Spark, PC | L200 | QEMU model only |
| CPU model inference | Pi (proven), iMac and PC (need AVX2), Mac | `crates/aienos-infer`, `core/infer.c` | Pi HARDWARE only |
| GPU path abstractions (context, channel, allocator, completion) | Spark, PC, Mac, iMac | L193 simulator, L192, L202, L214 | host simulation only |
| Signed transport and UDP stack | all | `native/net/aienos_sec.c`, `aienos_net.c` | QEMU and host, TEST keys, roster of one |
| Omega compiler, kernels, platform services | Spark first | Omega repo, L202 | host; 14 fixture kernels byte-identical (omega#419) |

Not shared, so each machine pays separately: interrupt controller (GICv3, GICv2, APIC, AIC2), IOMMU (SMMUv3, none on Pi, VT-d, AMD-Vi, DART), boot handoff (ACPI, device tree, Apple tables), disk controller path, GPU.

## 4. The network between machines

### 4.1 What exists (OBSERVED from source at `18e2765` and the machine maps)

- **UDP stack** (`native/net/aienos_net.c`): Ethernet, ARP, IPv4, ICMP echo, UDP. No TCP, no TLS, no DNS, no DHCP, no IPv6.
- **Reliable channel** (`aienos_ctl.c`): ordered, exactly-once delivery with retransmit, TEST identity only.
- **Signed transport** (`aienos_sec.c`, milestone M6-B): three-message SIGMA-style handshake, Ed25519 identities, fresh X25519 keys, HKDF-SHA256, AES-256-GCM-SIV records, replay window, key ratchet. Proven in QEMU and host tests. **Two limits that matter:** the keys are public TEST labels that protect nothing, and the peer list is "a pinned roster of one".
- **Drivers behind it:** virtio-net (QEMU), Pi 5 Gigabit MAC (send proven), iMac BCM57766 (send and netboot fetch proven), Spark RTL8127 (none, L198), PC (chip unknown), Mac (none; USB Ethernet first).
- **Lab paths already used on hardware (iMac):** report broadcast UDP 47031 (unauthenticated, lab only), netboot 47032 and 47033 (kernels checked with Ed25519 before start), live kernel swap 47034 and GPU register console 47035 (HMAC plus counter) on `feat/imac-gpu-bench`; ALLEN's UDP courier relays a question to the Spark's model server (L039).
- The word "gateway" appears nowhere in `native/net`, `docs/adr` or `docs/*.md` (OBSERVED by the iMac and Pi maps). It is a design item, not code.

### 4.2 What the hub does today and the gateway

The Pi hub runs the tailnet's **Headscale** coordination server, a Tailscale node (100.64.0.3), Pi-hole DNS, the nginx front door, a Forgejo server, a vault and a WireGuard link to another site. The Spark runs its own Tailscale node and its own Forgejo (Spark map section 4). Today the model server on the Spark answers HTTPS over Linux itself.

AienOS has no TCP or TLS, so a browser, an agent or `curl` cannot talk to an AienOS machine. A **gateway** is a Linux machine that speaks HTTPS to clients and the signed transport to the AienOS machines. It does not exist in code.

- **Who is it?** L203 (Drake's decision) is still open, but the Pi map and this page now agree on the recommendation: the Pi stays the always-on Linux hub and gateway until a second always-on Linux machine exists. The reason is physical: a native Pi window takes the tailnet coordination down for its whole length. The MacBook is the other candidate and is not always on.
- **Effect on the Pi:** native Aienux on the Pi is a short window, not the resident system, until services move one at a time (DNS first) after TCP and TLS exist.
- **What the gateway needs that does not exist:** an HTTP to signed-transport translator (host program), a peer list with one entry per AienOS machine, and real identity keys.

### 4.3 How machines reach each other without Linux, and Tailscale's replacement

Tailscale does four jobs: a private address per machine, authentication, a path through firewalls and routers, and relays. For AienOS machines the second is the signed transport; the others are not built. A native tailnet node needs the WireGuard data plane and Tailscale's control protocol, which runs over TCP and TLS (INFERRED, medium; the iMac and Pi maps did not read the protocol). None of TCP, TLS or WireGuard exists natively, and encryption math is never hand-rolled (Drake's exception to "build our own"), so vetted primitives or a vetted implementation are required for those parts.

**Near term, no new code on the AienOS side:** the Linux hub advertises the home LAN as a subnet router, so the Spark, the Macs and the PC reach an AienOS machine at its LAN address, and the AienOS machines speak the signed UDP transport on the LAN (INFERRED, medium; iMac map section 9).

| Piece | Today | Gap | Gate that retires it | Ledger |
| --- | --- | --- | --- | --- |
| Machine identity and authentication | Ed25519 plus X25519, TEST keys | Production keys from the owner key chain; machine ids; TRUST-1 BLOCKED_OPERATOR | handshake between two machines with owner-issued keys, TEST keys refused | unowned |
| Many peers | roster of one | A roster of N signed by the owner: add, revoke, rotate; kept in the Store | a three-machine handshake matrix in QEMU plus one revoked peer refused | unowned |
| TCP | none | TCP in `native/net`, tested against QEMU and a hostile corpus like the UDP stack | a TCP echo and a file-sized transfer in QEMU with loss and reordering | unowned |
| TLS 1.3 | none | UNKNOWN which mandatory TLS 1.3 pieces `native/crypto` lacks (experiment: list the required suite against the tree). Vetted primitives only | a TLS server and client handshake against a reference implementation in QEMU | unowned |
| WireGuard data plane and Headscale control protocol | none | Both are needed for a native tailnet node; near-term route above avoids them | an AienOS machine appears as a tailnet node, or a written decision that the hub subnet router is permanent | unowned |
| DNS | none | A UDP DNS server is possible without TCP; the proposed first native hub service (Pi-hole role) | DNS answers on the Pi from Aienux | unowned |
| Addressing | LAN IPv4 and ARP | A stable name per machine inside the roster | peer reachable after its address changes | unowned |
| Path across firewalls and routers | none | Same-LAN is enough if all machines are home machines; UNKNOWN whether the Mac or PC will leave the LAN (Drake) | off-LAN reach through a relay, or a written decision that the hub keeps it | unowned |
| Remote control | operator keyboard and screen; signed register console (47035) on the iMac bench | A signed control console on every machine (the iMac console is the pattern) | an operator command round-trip, signed, on hardware | unowned |
| Time | no RTC read yet (Spark L200, Pi PI13, iMac IM23) | RTC read plus network time over the transport | wall clock set from a peer, drift measured | L200 for Spark RTC; rest unowned |

## 5. Linux (or macOS, Windows) fully off, per machine

"Off" means: the machine boots AienOS from its own disk by default, and the old operating system is gone or only a recovery option. Every step is an operator step for Drake (section 7); none is authorised by this page. The maps differ on what "off" even means, so it is stated per machine.

| Machine | Today's OS | Target | Blocking rungs | Order | Operator steps |
| --- | --- | --- | --- | --- | --- |
| Spark | Ubuntu on ext4, Secure Boot off, BootOrder `0001` ubuntu | Linux fully off, serving the 27B model | rungs 1 to 8 on hardware, then 9 to 12; services moved (section 4.2) | Spark map section 7, items 1 to 20: attended boots 1 to 3, native CPU run, GPU path, move non-model work off, installer, soak with Linux still on the disk, boot-order change, wipe last | monitor, keyboard and recovery stick; attended boots; key ceremony; allow GSP firmware load; look at the unit (L204); decide L203; lift the `BootOrder` invariant in writing for the one cutover change |
| iMac | macOS on the Fusion Drive (what is on the disk is not recorded in the map: GPT valid, no AIENOS partition at the first read, UNKNOWN beyond that) | resident Aienux booting from its own disk through Apple's firmware | #418 second boot, VT-d, Store write path, GPU, model | land the open PR stack in order, then the read-only lab boot, then the rest (iMac map section 6) | power on once per lab kernel with the stick; tell the orchestrator what the screen shows; listen for the fan (IM20); allow the first GPU firmware load |
| Pi 5 | Raspberry Pi OS on a USB stick, hub services | native windows now; resident Aienux only after the hub's services have another home | network receive, USB or UART input, production unlock, TCP and TLS | last; after a second always-on Linux machine exists | one-time boot menu setting per window; a USB-serial cable to the header; pulling the plug for the power-cut test; owner-key signing on the Mac |
| MacBook | macOS 26.6.2 | tethered Aienux; macOS stays; a standalone boot only after a soak | everything on the Mac ladder; the firmware blobs (F4) | Mac map section 9, steps 1 to 15 | choose which Mac (this one is the key-ceremony machine); one-time recovery setup (new boot container, Permissive Security, installs m1n1 stage 1; undoable from recovery); USB-C cable from the Spark; no macOS updates before first boot |
| PC | Windows 11 | **Windows is kept.** Aienux as a dual boot beside it, never replacing Windows Boot Manager | AMD-Vi, NIC driver, GPU through FSP | PC map phases 0 to 4 | run the inventory (L212); save the BitLocker key first; boot-menu key at power-on; decide the storage home; allow GPU firmware load; decide whether a permanent boot entry is wanted |

## 6. The future: what is measured, what is planned, and what retires each

Everything here is PLANNED unless the status column says measured. "Retired by" is the test that, once green, removes the item from this section.

| Item | Status | Retired by | Ledger |
| --- | --- | --- | --- |
| **Omega as the only language** (ADR 0024): the kernel written in Omega, C kept only where hardware forces it | PLANNED; C kernel and Rust scaffolding exist today; R2 floats in Omega done (L135) | Omegese rungs K1 to K6 below | L184 |
| K1: bytes, text and long arrays in Omegese | PLANNED | merged with tests, local `make` per rung | L184 |
| K2: peek and poke of machine memory | PLANNED | same | L184 |
| K3: system words (the calls the kernel needs) | PLANNED | same | L184 |
| K4: flat placement (fixed layout of code and data) | PLANNED | same | L184 |
| K5: units (L096) | PLANNED | same | L184, L096 |
| K6: Aienux stage 0 written in Omega boots the Spark | PLANNED | a receipt from the real Spark; the final head judged once on the Spark (Drake's one-PR rule for #601) | L184 |
| Omega platform services (cut A) so Omega runs without Linux | in flight | omega gates green on hosted and freestanding builds | L202 |
| **Reimagined drivers:** a driver as a checked Omegese device contract (registers, rings, state machine) that the kernel compiles, with the chip model and tests generated from it, measured against the hand-written NVMe and virtio-net drivers. This is the plan for the many drivers in sections 2.1 to 2.5 | PLANNED, ADR and prototype in flight | a generated driver passes the same QEMU gates as the hand-written one | L215 |
| AienOS installer: write kernel and Store onto a blank disk, A/B slots, rollback | PLANNED; `SLOT_LOADER` PASS in QEMU only; iMac own-disk boot (#418) is the first real slot code | an install from a recovery stick on a real machine with a forced-failure rollback | unowned |
| AienOS updater: signed update of kernel and system on every machine, rollback, no Linux | PLANNED; nothing beyond the slot loader | a signed update applied and rolled back on hardware | unowned |
| TCP, TLS 1.3, then HTTP natively (shared by the Spark gateway, git, Pi hub services, Tailscale replacement) | PLANNED; none exists | section 4.3 gates | unowned |
| Multi-machine model serving: one model spread across the Spark, the PC and others, or requests routed by load | PLANNED; L190 (draft and verifier on one machine) is a different thing; the PC cannot hold the 27B in 12 GB and the iMac GPU is far smaller | two machines answer one prompt through the signed transport, output identical to one machine | unowned |
| Native model run beyond the Pi | Pi MEASURED at 1.20 tok/s (about 5.68% of an unverified vendor ceiling); Spark, iMac, PC, Mac NONE; Linux Spark numbers and ceilings are in L123 and L151 | native token rate reported as a fraction of the measured ceiling per machine | unowned per machine |
| Production trust chain: owner keys, Secure Boot on, anti-rollback (no TPM on Pi or Mac; SEP or an owner-held counter UNKNOWN) | PLANNED; BLOCKED_OPERATOR | TRUST-1 qualification receipt | Drake |
| Public proof (one-command 27B demo) | in flight, runs on Linux today | demo on a clean Spark | L182 |

## 7. What Drake does, so nothing is asked twice

Only Drake can do these. Each belongs to one ledger id so the coordinator can find it. Do not ask again anything marked answered.

| Item | Why only Drake | Where | Ledger |
| --- | --- | --- | --- |
| Decide where build, judge, git and agents live after Linux is off; approve moving the Phase 3 judge; confirm the Pi hub stays the gateway until a second always-on Linux machine exists | changes how the project is tested and what the tailnet depends on | all | L203 |
| Look at the Spark unit: ConnectX ports or a serial header? | software cannot see a connector that enumerates nothing | Spark | L204 |
| Run the PowerShell inventory on the PC and send the file | read-only, but only Drake is at the machine | PC | L212 |
| Save the BitLocker recovery key before any firmware setting changes | Windows may ask for it once | PC | L209 map |
| Choose which MacBook (this one holds the owner-key ceremony, Activation Lock on, 752 cycles); the one-time recovery setup; a USB-C cable from the Spark; say whether a separate USB SSD may be used for Store tests; no macOS updates before first boot | changes the Mac's boot setup | MacBook | L208 map |
| Attended boots: monitor, keyboard, recovery stick or lab stick, press the button, send a photo or let the report come over the cable | physical presence | Spark (three), iMac (each lab kernel), Pi (each window), PC (boot-menu key) | L197 for the Spark |
| Listen to the iMac fan on a long run (IM20); look at the machine | no sensor record exists | iMac | iMac map |
| Allow the first GPU firmware load and every later GPU use | irreversible on hardware | Spark, PC, iMac, Mac | per GPU row |
| Allow firmware blobs extracted from macOS (Wi-Fi, Bluetooth, GPU, display), and any Wi-Fi or Bluetooth firmware on other machines | a firmware load and a licence question | MacBook, iMac, Pi | per row |
| A USB-serial cable to the Pi UART header; pull the plug for the power-cut test; owner-key signing on the Mac with PIN and touch | physical | Pi | PI5-8 plan |
| Owner key ceremony and machine ids for hardware images | secret material | all | TRUST-1 |
| Lift the `BootOrder` invariant in writing for the one cutover change; change the boot order at cutover; decide a permanent PC boot entry | the rule forbids it today | Spark first, PC | unowned |
| Say whether the MacBook or PC will ever be off the home network | decides whether a relay is built | network | unowned |
| Decide on the iMac flash chip and an external programmer (IM24) | stopped twice by an automated safeguard, do not retry | iMac | none, on hold |

Already answered: Wi-Fi, Bluetooth and audio drivers for the Spark are wanted (L206, L207, after the earlier drop in L205). iMac firmware replacement is on hold and must not be retried.

## 8. Unowned rows for the coordinator

Run these lines as written. Rows already owned by a ledger id or by an open PR on the desk are not listed.

```
ledger add unassigned "Spark: installer + rollback-safe A/B slots on a blank disk (SLOT_LOADER, DISK_LAYOUT NOT_RUN)" "install from a recovery stick on a real machine with a forced-failure rollback; DISK_LAYOUT PASS"
ledger add unassigned "Updater: signed AienOS update with rollback on every machine, no Linux" "signed update applied and rolled back on hardware"
ledger add unassigned "Spark: power, thermal, fans, CPU frequency under no OS (D11)" "long hold at load with temperatures read; or written reason firmware holds them"
ledger add unassigned "Spark: GB10 discovery and BAR0 ownership by the kernel (G1)" "BOOT_0 read after ExitBootServices on the real Spark"
ledger add unassigned "Spark: GPU interrupts or written reason polling suffices (G4)" "a fault or completion interrupt seen, or a measured reason"
ledger add unassigned "Spark: GB-scale pinned and mapped memory, huge pages, 27B weight image (G5)" "one multi-GiB allocation mapped for CPU and GPU, in QEMU first"
ledger add unassigned "Spark: USB xHCI physical bring-up, keyboard (D6)" "KEYBOARD lines from the real Spark; glue power and clock state read"
ledger add unassigned "Spark: Omega cut E, matmul, attention, launchers, session, resident runtime (G6)" "receipts against the Linux reference per kernel"
ledger add unassigned "Spark: native CPU inference with aienos-infer on the Spark, speed against the memory ceiling (G7 step 10)" "tok/s and fraction of the measured memory ceiling"
ledger add unassigned "Spark: 27B load from NVMe, KV on GPU, token stream (G7)" "a token stream from the real Spark under AienOS"
ledger add unassigned "Gateway: HTTP to signed-transport translator, and the request path from another machine (G8)" "a request from another machine answered by the Spark"
ledger add unassigned "Spark: move Atlas data and Forgejo off the TPM-sealed mounts, retire or relocate every Linux service (P3, U1 to U7, U10)" "service list in the Spark map, each row moved or retired with a receipt"
ledger add unassigned "Network: signed-transport roster of N peers (add, revoke, rotate), stored in the Store" "three-machine handshake matrix in QEMU plus one revoked peer refused"
ledger add unassigned "Network: production machine identity (owner-issued keys, TEST refused) for the signed transport" "handshake between two machines with owner-issued keys, TEST keys refused"
ledger add unassigned "Network: native TCP in native/net" "TCP echo and file-sized transfer in QEMU under loss and reordering, hostile corpus"
ledger add unassigned "Network: TLS 1.3 with vetted primitives; list what native/crypto lacks first" "TLS handshake against a reference implementation in QEMU"
ledger add unassigned "Network: native tailnet node or written decision that the Linux hub subnet router is permanent (WireGuard data plane, Headscale control protocol)" "an AienOS machine appears as a tailnet node, or Drake records the decision"
ledger add unassigned "Network: first native hub service, DNS over UDP on the Pi (Pi-hole role)" "DNS answers served by Aienux on the Pi"
ledger add unassigned "Network: network time over the signed transport" "wall clock set from a peer with drift measured"
ledger add unassigned "Network: signed control console on every machine (pattern: iMac port 47035 console)" "signed operator command round-trip on hardware"
ledger add unassigned "Network: off-LAN reach decision (relay vs hub-held Tailscale)" "Drake answers; if relay, a path through a relay in QEMU"
ledger add unassigned "iMac: VT-d DMA confinement (IM4)" "VT-d QEMU gate PASS (QEMU intel-iommu) then physical report"
ledger add unassigned "iMac: disk write path and Store v1 on the iMac" "Store write, flush and crash recovery on the real iMac disk"
ledger add unassigned "iMac: one read-only lab boot listing xHCI ports, GPU VRAM size and memory controller, Thunderbolt state, RAPL watts (IM13, IM17, IM19, IM21)" "report lines resolving each UNKNOWN row"
ledger add unassigned "iMac: fan and temperature under a long native run (IM20, Drake's question 2026-10-18)" "fan speed and temperature read over a long run, plus a human listening"
ledger add unassigned "iMac: x86 AVX2 inference path and image target, first CPU token (IM26)" "token output equals the llama.cpp reference; tok/s against a measured memory ceiling"
ledger add unassigned "iMac: Wi-Fi 14e4:43ba, Thunderbolt, SD reader drivers against chip models (IM16, IM18, IM19)" "model gates; firmware inventory with licences"
ledger add unassigned "Pi 5: network receive (no frame in 400 ms, PI9) and UDP echo, then first signed datagram Spark to Pi" "an incoming frame received on the real Pi; signed datagram round trip"
ledger add unassigned "Pi 5: UART input and RP1 xHCI keyboard so a person can type the unlock secret (PI5, PI6, PI12)" "unlock from an operator secret, no card file"
ledger add unassigned "Pi 5: one lab image printing started cores, a bandwidth loop, temperature and fan state, RTC value (PI2, PI13, PI14, PI-M3)" "report lines from the real Pi"
ledger add unassigned "Pi 5: multi-core inference, speed against a measured memory ceiling (PI-M3)" "tokens identical to the reference; tok/s as a fraction of the measured ceiling"
ledger add unassigned "Pi 5: Wi-Fi, Bluetooth, GPIO and I2C and SPI, HDMI driver models (PI15 to PI18)" "documentation quote or DOCS SILENT before chip code; model gates"
ledger add unassigned "MacBook: platform layer, device-tree parser, raw Image entry, AIC2, RTKit client, DART and SART on chip models (D1 to D6)" "QEMU boot from a synthetic device tree; AIC2 and RTKit model tests"
ledger add unassigned "MacBook: tethered boots 1 to 4 (proxy read-only, kernel alive, framebuffer, disk read-only)" "pass lines per boot with the Mac returning to macOS unchanged"
ledger add unassigned "MacBook: ANS2 NVMe, SMC, MTP keyboard, USB-C PD and DWC3 USB, USB Ethernet (D4, D7 to D10)" "model gates, then a UDP report from the Mac"
ledger add unassigned "PC: make the x86 kernel vendor-neutral on a clean base (Intel-only checks, x2APIC, NVIDIA display match)" "QEMU q35 with an AMD CPU model boots the kernel; iMac gates still pass"
ledger add unassigned "PC: AMD-Vi driver with IVRS parser against QEMU's emulated device (PD3)" "IOMMU state printed before any DMA; a device granted a range only"
ledger add unassigned "PC: NIC driver for the chip the inventory names, plus the UDP report (PD5)" "one frame out of the PC seen on the LAN"
ledger add unassigned "PC: FSP boot-command encoder and GB202-family image extraction (PG2); second memory pool in the GPU simulator (PG3)" "host tests against driver structures; simulator vector add with two pools"
ledger add unassigned "PC: 12 GB model policy, stream a 27B or run a smaller resident model (PG8)" "token stream on the PC with a stated policy and speed against the ceiling"
ledger add unassigned "Multi-machine model serving across the Spark and the PC (or others)" "two machines answer one prompt over the signed transport, output identical to one machine"
```

## 9. Limits of this document and what it did not read

- Revision 2, 2026-10-20, after all four machine maps landed (`97a0945`, `1d77025`, `eb552cb`; Spark `bb2399f`). No test was run for this page. Gate names, hardware results and speeds are quoted from the maps, PR bodies, evidence folders and receipts and were not re-run. A PR body is its author's claim, and the iMac PRs are still open.
- Where the maps disagree with each other or with the ledger, the map is right and the ledger row is stale: iMac GPU is Polaris 10 `1002:67df` (the code and L143 say Polaris 11; fixer L213 is correcting it).
- The Pi map's ceiling of 21.13 tok/s uses a vendor memory figure that is not from this repo (UNVERIFIED). The 5.68% is therefore a bound, not a result.
- Tailscale protocol details (control protocol, TCP and TLS) are INFERRED from general knowledge by the iMac and Pi maps and were not read here.
- The Spark map's tags are not upgraded by this page. UNKNOWN rows stay UNKNOWN until their named experiment is run.
- No time, effort or speed estimate appears here except those tied to a receipt id.
- Reuse search: origin/main, `git branch -r`, the four map branches read with `git show`, sibling worktrees `~/workspace/aienos-*`, `~/handoffs` (iMac, Pi, Spark inventories and hand-offs), the ledger through L216, the mind map note on the iMac joining Tailscale as quoted by the iMac map. Not read: ADR 0010 (fabric), ADR 0020, ADR 0024 in full, the iMac PR bodies and the Pi runs beyond what the maps quote, and the raw MacBook and Pi inventory folders. No existing document covers all five machines or a multi-peer network.
