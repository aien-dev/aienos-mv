# Replacing Linux on the DGX Spark: gap map and tracking plan (L191)

Status: read-only inventory plus source reading. No reboot, no boot-order or ESP change, no firmware load and no GPU access were done to write this. Every claim is tagged OBSERVED (read from the live box or from source at the pinned commit), INFERRED (with confidence) or UNKNOWN. Nothing here is a claim that any row works on the Spark.

Pins: this repo at `18e2765` (origin/main, 2026-10-19). Live box `spark-b87b`, Linux `7.0.0-1019-nvidia` aarch64, NVIDIA driver 580.173.02, read on 2026-10-20. Raw command output with exit codes: `~/handoffs/spark-inventory-20261020/*.txt` (every inventory command exited 0).

Names (Drake, 2026-10-20): Aienux is the kernel, AienOS is the system. Older files still say AIENOS for both.

## 1. Plain-English summary

Today the Spark is a Linux computer that runs everything: the disk, the network cable, the USB ports, the screen, the chip that holds the TPM key, the GPU driver, the 27B model server, the git server, the test judge, and the AI agents. AienOS has run on this machine exactly once, for 75 seconds on 2026-09-24, and it only printed a report and returned to Linux (OBSERVED, `evidence/m2_first_boot_2026-09-24.md`). The C kernel has never been booted on the Spark (OBSERVED, ROADMAP status block, `native/kernel/README.md`).

What replacing Linux 100% means, in three layers:

1. **Drivers only** (disk, network cable, USB, screen, TPM, clock, power). Most have a start in the C kernel, proven in the emulator (QEMU) and nowhere on the real Spark. The one network chip on this box (Realtek 10 gigabit, `10ec:8127`) has no driver at all. The ConnectX network chip does not show up on this box at all (section 3).
2. **The GPU path** (cuts A to E of `docs/GB10_NATIVE_DEPENDENCY_MAP.md`). Everything the NVIDIA Linux driver does for us has to be rebuilt. The host-only halves of cuts C and D are being built right now (L192, L193). Nothing here has touched the chip.
3. **Userland that AienOS does not have**: TCP and TLS, an ssh server, git, compilers, the Claude agent program, Docker, Tailscale. Blunt answer: these will **not** run on AienOS in any path this plan can see. They have to run on another machine (section 6). "Linux fully off" on the Spark means the Spark becomes an appliance that serves the model, and the build, test, git and agent work lives elsewhere.

Shortest path in one line: finish the disk, network and screen drivers on a few attended one-time boots (Linux stays the default every time), then the GPU path, then move everything that is not the model server to another machine, then Drake changes the boot order. Section 7 has the ordered list and section 8 the steps only Drake can do.

## 2. How the live box was read

All commands read-only, run on the Spark itself (`hostname` = `spark-b87b`). Outputs in `~/handoffs/spark-inventory-20261020/`:

| File | Command |
| --- | --- |
| `lspci.txt`, `lspci-tree.txt` | `lspci -nn`, `lspci -tv` |
| `lsusb.txt`, `xhci.txt` | `lsusb`; platform device to driver listing for `NVDA8*` |
| `iplink.txt`, `lsblk.txt`, `lbs.txt` | `ip -br link`, `lsblk`, NVMe logical block size |
| `services.txt`, `docker.txt`, `listen.txt`, `tailscale.txt` | running systemd services, containers, listening sockets, tailnet |
| `efiboot.txt`, `sb.txt`, `tpm.txt`, `drm.txt` | `efibootmgr`, Secure Boot state, TPM device nodes, display cards |
| `mlxcheck.txt` | count of `15b3` PCI devices (0) and loaded `mlx5` modules (11) |

## 3. The hardware, exactly (OBSERVED, live box)

| Part | PCI / platform id | Linux driver | Where |
| --- | --- | --- | --- |
| GB10 GPU (Blackwell) | `10de:2e12` rev a1, VGA class `0300`, BAR0 `0x24000000` 64M | `nvidia` (open module, 580.173.02) | `000f:01:00.0` behind bridge `10de:22d1` |
| NVMe SSD | Samsung `144d:a810` (MZALC4T0HBL1-00B07, 3.7 TB), BAR0 `0x62100000` 16K | `nvme` | `0004:01:00.0`; **512-byte logical block** |
| Ethernet (the only wired NIC) | Realtek `10ec:8127` rev 05, 10 GbE (UEFI calls it "Realtek PCIe 10 GBE Family Controller") | `r8127` (out-of-tree) | `0007:01:00.0`, interface `enP7s7`, 192.168.1.108 |
| Wi-Fi | MediaTek `14c3:7925` (MT7925), link down | `mt7925e` | `0009:01:00.0`, interface `wlP9s9` |
| Bluetooth | USB `13d3:3630` (IMC Networks, MediaTek) | `btusb` | bus 011 |
| ConnectX | **not present in `lspci`**: zero `15b3` devices | `mlx5_core`, `mlx5_ib`, `ib_*` are loaded (11 modules) but have nothing to bind | root complexes `0000:00` and `0002:00` have bridges whose secondary buses are empty |
| USB controllers | six ACPI platform xHCI: `NVDA8000:00` to `:04` and `NVDA8001:00` (not PCI) | `xhci-hcd`, glue `xhci_plat_hcd`, `onboard_usb_dev` | 12 USB buses |
| USB devices | UPS CyberPower `0764:0601`, Yubikey `1050:0407`, Bluetooth above | `usbhid`/nut, ccid | buses 001, 005, 011 |
| Display | card0 = `simple-framebuffer` (the firmware's UEFI framebuffer), connected; card1 = `nvidia`, no connector listed | `simple-framebuffer`, `nvidia_drm` | the monitor is on the firmware framebuffer, not the GB10 display engine (INFERRED, medium; only sysfs was read) |
| TPM 2.0 | ACPI `MSFT0101` / `NTC0702`, CRB interface | `tpm_crb` | `/dev/tpm0`, `/dev/tpmrm0` |
| Other platform blocks | about 30 `NVDA****` ACPI devices (HDA audio `NVDA2014`, pinctrl `NVDA9221`, others unbound or in-kernel) | various | not decoded here: UNKNOWN |
| CPU / memory | 20 cores in two classes of 10 (m2 evidence); 127,598,528 kB RAM | `cppc_cpufreq`, `arch_sys_counter` clock, `rtc0`, `sbsa_gwdt` watchdog | |
| Boot | Secure Boot **disabled**; BootOrder `0001` (ubuntu), `0003` (UEFI PXE on the Realtek NIC); `Boot0000` "AIENOS Allen"; the recovery stick entry is absent now (stick not plugged) | | ESP 298M vfat, root 3.7T ext4, 2.5T used |

Honest gap on ConnectX: the brief asks for "both NICs incl. ConnectX". This box shows one wired NIC (Realtek) and one Wi-Fi. The ConnectX driver stack is loaded, but no ConnectX device is enumerated. UNKNOWN whether the card is absent on this unit, held in reset, or its link did not train. Do not plan around it until someone reads `0000:00:00.0` and `0002:00:00.0` link state (read-only, no reboot) or checks the unit's spec.

## 4. What Linux does on the Spark today (OBSERVED, `services.txt`, `docker.txt`, `listen.txt`)

Running as systemd services (47): `ssh`, `tailscaled`, `docker`/`containerd`, `apache2`, `NetworkManager`, `systemd-networkd`, `systemd-resolved`, `systemd-timesyncd`, `wpa_supplicant`, `avahi-daemon`, `lldpd`, `cups`, `cups-browsed`, `bluetooth`, `ModemManager`, `snapd`, `fwupd`, `unattended-upgrades`, `multipathd`, `udisks2`, `upower`, `rasdaemon`, `smartmontools`, `nut-driver@spark`/`nut-server` (the UPS), `nvidia-persistenced`, `dgx-dashboard`/`-admin`, `cron`, `rsyslog`, `dbus`, `polkit`, `rtkit`, `logind`, `journald`, `udevd`, two user managers, `getty@tty1`, `serial-getty@ttyS0`, and three Atlas units: `atlas-private-storage`, `atlas-forgejo-storage` (both a `gocryptfs` mount whose key is released by the TPM) and `atlas-forgejo` (the Forgejo git server on 3000/2222/3443).

Containers (7): `vaultwarden`, `vaultwarden-caddy`, `aienos-waitlist`, `grafana`, `prometheus`, `node-exporter`, `uptime-kuma`.

Other long-running programs listening on the box: `aien-serve-http` on 18091 (the 27B model server, 38 GB resident), `cortex-rs` 18080, `spark-mail` 18092/2525, `spark-cockpit` 18095, `aien-translate` 18097, `authelia` 9091, `OliveTin` 1337, `conduit` 6167 (Matrix), `hermes`, a Node program on 3773, two `python3` listeners (18098, 18198), and about a dozen `claude` agent processes plus `rustc` and `ld` builds (the test judge, Phase 3, `472ed37`). Tailnet: `spark` 100.64.0.2, peers `drakes-macbook-air` 100.64.0.1 and `hub` 100.64.0.3 (a Linux machine).

## 5. The gap table

Columns: **Linux today**; **hardware**; **Aienux today** (file and gate name, with where it was proven); **missing**; **evidence a native boot would need**; **host-only now?** (can a builder make progress with host builds and QEMU alone). "NOT_RUN on the Spark" means the code and tests exist but no run on this machine has happened.

### Class D: rows that only need drivers or kernel services

| # | Linux today | Hardware | Aienux today | Missing | Native-boot evidence needed | Host-only now? |
| --- | --- | --- | --- | --- | --- | --- |
| D1 | Boot, ESP, firmware handoff | UEFI, ESP 298M | Handoff image booted once on 2026-09-24 (`M2_GATE: PASS`); C kernel boots in QEMU (`AIENOS_CK_M1`); one-time `BootNext` staging scripts exist (`scripts/stage_one_time_boot_ck*.sh`) | C kernel on the Spark; `M0_ROLLBACK` is NOT_RUN for the C loader; the PCI-root open that failed to find the GB10 on the first boot (`gb10: unavailable`) is unresolved | An attended boot printing `kernel: alive` from the C kernel on the Spark, and Linux back afterwards (`verify_native_rollback.sh` 10 invariants) | Yes for loader and rollback in QEMU; no for the physical proof |
| D2 | CPU bring-up, MMU, interrupts, scheduler | 20 cores, GICv3, arch timer | MMU, EL2 to EL1, GICv3 from MADT, timer, threads, preemption, EL0, capabilities, IPC: QEMU PASS (`AIENOS_CK_M1`, `M3`). `smp.c` and the `SMP` gate exist, NOT_RUN | Physical run; 20-core placement across two core classes; SMP gate result | `cpu`/`smp` lines from the Spark | Yes (QEMU) |
| D3 | IOMMU / DMA confinement | two SMMUv3 (first at `0x13800000` behind the USB controllers) | `core/smmu.c`: SMMUv3 service; two-level stream table added in #270 (`3df181c`, QEMU and host only) so ids up to `0xfffff` fit. Gates `SMMU`, `NVME_SHUTDOWN` PASS in QEMU | The paragraph in `native/kernel/README.md` saying the Spark IORT is refused (15 mappings, limit 8) is stale against the code: `CK_IORT_MAX_MAPS` is now 32 and "covers the DGX Spark (MEASURED: 15 root complexes)" (`core/acpi.h` L101-L103), `test_smmu` has a Spark-shape case, and `docs/GB10_IORT_DECODE.md` records the table read on the Spark on 2026-10-10. Host and QEMU only; nothing was run on the Spark's SMMUs. Whether the GPU's traffic goes through a confinable SMMU window is UNKNOWN (dependency map unknown 6) | `smmu: enabled` plus a granted, confined device on the Spark | Yes (the Spark-shape host case exists; the physical result does not) |
| D4 | NVMe disk (root fs, model files, Docker data) | Samsung `144d:a810`, 512 B LBA, 3.7 TB | `native/disk/disk_nvme.c`: read, write, flush, QEMU PASS (`QEMU_NVME`, `QEMU_NVME_RW`, `M4_NVME`); one command in flight, every request through a bounce buffer (`disk_nvme.h` L17-L19); 512 B path `STORE_512B_CRASH_RECOVERY_QEMU` PASS; clean shutdown `NVME_SHUTDOWN` PASS | Never driven on this controller. `NATIVE_GB10_STORE_ROOT_ATOMICITY: FAIL` (this SSD is 512 B, not the 4 K the root write assumes). Queue depth 1 and bounce copies: speed for tens-of-GB model files is not measured | Read of the GPT and a Store superblock from `nvme0n1` by the C kernel, and the controller shut down cleanly | Partly: queue depth and zero-copy can be built and measured in QEMU; real speed needs the Spark |
| D5 | Wired network | Realtek `10ec:8127`, UEFI PXE entry exists (`Boot0003`) | **None.** Native NICs: virtio-net (QEMU), Pi 5 GEM, iMac BCM57766 transmit-only report (`x86/tg3_x86.c`). Stack is ARP, IPv4, ICMP, UDP only (`native/net/aienos_net.c`). org-check "r8127 realtek": 0 hits in 98 repos (2026-10-20T15:56Z) | RTL8127 driver (datasheet needed: UNKNOWN whether Realtek publishes one; Linux `r8127` is out-of-tree vendor code), link, DMA rings under the SMMU, DHCP or static address | A frame sent from the Spark by the C kernel and seen on the LAN; then received frames | Driver logic yes (against a model of the chip), physical no. The iMac lanes (`feat/imac-net-report`, `feat/imac-netboot`, L142) are the pattern to copy |
| D6 | USB: keyboard, UPS, Yubikey | six platform xHCI `NVDA8000:00-04`, `NVDA8001:00` | `dev/xhci_fence.c`, `dev/usb_kbd.c`, `dev/usb_hid.c`, ACPI platform walker; `KEYBOARD` gate PASS in QEMU; expected Spark lines in `tests/fixtures/spark_xhci_acpi_expected.txt` | Physical run. UNKNOWN: whether the MediaTek glue around each controller is powered and clocked when the kernel first touches it (a powered-down block could fault). No class driver for the UPS (HID power device) or the Yubikey (CCID/FIDO) | `keyboard: ready` and a typed line on the Spark | Yes for class drivers (QEMU usb-hid, usb-ccid); no for the glue question |
| D7 | Screen | firmware framebuffer (card0 `simple-framebuffer`) | UEFI GOP framebuffer console (`native/boot/efi_gop.c`, `core/fbcon.c`, held 120 s before reset in `stage_one_time_boot_ck.sh`) | Not shown on the Spark by the C kernel. GB10 display engine: no driver, and none is needed if the firmware framebuffer is the only display (INFERRED, medium) | Photo of the console on the Spark's HDMI | Yes (QEMU); physical is an operator step |
| D8 | Serial console (`ttyS0`) | SPCR UART | `ck_console_set_uart` via SPCR in `native/boot/efi_main.c` | Physical header on this unit UNKNOWN; never run on the Spark | A line on a serial capture, if a header exists | Yes (QEMU) |
| D9 | TPM unseals the Atlas disk key; measured boot | TPM 2.0, CRB (`MSFT0101`) | **None.** M5 key hierarchy and sealed Store exist (`native/m5`, `native/store/store_sealed.c`); TPM slot 0 is BLOCKED_HARDWARE; recovery KEK slot 1 exists. org-check "tpm crb": 0 hits | TPM2 CRB driver (small MMIO interface, spec-defined: INFERRED, medium), PCR policy, NV anti-rollback counter | `TPM2_Startup`/`GetRandom`/an NV read from the C kernel on the Spark | Yes against a TPM simulator (swtpm lane `lane1/trust1-gate5-swtpm` exists) |
| D10 | Clock, RTC, watchdog, entropy | `arch_sys_counter`, `rtc0`, `sbsa_gwdt`, `RNDR` | Timer and `RNDR` (`arch/timer.c`, `arch/rndr.c`) QEMU; Pi-only watchdog (`dev/pi5_wdt.c`) | RTC read (wall-clock time), watchdog for the Spark, network time. Without an RTC AienOS cannot name the date | Wall-clock read, watchdog petting | Yes |
| D11 | Power, thermal, fans, CPU frequency, reset and off | `cppc_cpufreq`, 3 hwmon, many cooling devices, ACPI | PSCI reset and off (QEMU, and the 2026-09-24 boot reset the Spark) | UNKNOWN: who holds the fans and clocks if no OS manages them (firmware may, or the box may overheat or run slow). No frequency or thermal code | A long hold at load with temperatures read (needs a sensor path: UNKNOWN) | No. Needs a documented answer from NVIDIA or the Spark's platform tables first |
| D12 | Fan-out of device discovery | ACPI, 6 PCI segments | ECAM walk, MADT, FADT, IORT parse, platform xHCI discovery; multi-segment fixtures (aienos#287) | Physical walk of all six segments and the two empty root ports | `pci:` listing matching section 3 | Yes |
| D13 | Wi-Fi, Bluetooth, audio (HDMI audio), CUPS, avahi, ModemManager | MT7925, BT, HDA | None | Not planned. Recommendation: drop them. The Spark is wired and headless-ish. Decision is Drake's if any of these is wanted | none | n/a |

### Class G: rows that need the GPU path (cuts A to E)

| # | Linux today | Hardware | Aienux today | Missing | Native-boot evidence needed | Host-only now? |
| --- | --- | --- | --- | --- | --- | --- |
| G1 | Find the GPU, own its BAR | GB10 `10de:2e12` at `000f:01:00.0` | UEFI-time read of `BOOT_0`/`BOOT_42` in the handoff image; Cut B host fixtures (aienos#287) | Not yet seen working on the Spark (first boot: `gb10: unavailable`; leading hypothesis is the firmware holding the PCI root protocol exclusive, unconfirmed) | `BOOT_0` read after `ExitBootServices` by the kernel | Fixtures yes; physical no |
| G2 | Firmware boot of the GPU: SEC2 boots GSP-FMC, GSP-RM runs | GB10B: the host hands over blobs and structures | `docs/GB10_FIRMWARE_INVENTORY.md` (blobs, versions, digests, load path from driver tag 580.173.02); `ck_gsp_fw_inspect`. **L192 in flight** (`feat/gb10-gsp-host`): byte-exact boot structures and an RPC subset, host only | Everything beyond bytes in memory: COT payload and WPR meta preconditions at AienOS entry, mandatory RPC subset (dependency map unknowns 1, 3, 5). Licence limits on shipping the blobs (inventory section 6) | GSP alive and a channel reply on the chip | Host halves: L192. Chip: no |
| G3 | GPU page tables, memory allocation, channel and doorbell | GB10 MMU | Nothing native. **L193 in flight** (`feat/gb10-gpu-mmu-sim`): page tables, allocator and channel model on a simulated device | Real GB10 MMU format and TLB invalidation, work-submit token, the stored per-context state RM sets up (unknowns 2 to 5) | One vector add on the chip with a normal teardown and reopen (cut D acceptance) | Simulated: L193. Chip: no |
| G4 | Interrupts from the GPU | MSI-X / ITS | Polling only in Omega today; GIC code refuses INTIDs over 1020 | Whether polling is enough (unknown 7), LPI/ITS route | A fault or completion interrupt seen, or a written reason polling suffices | Partly |
| G5 | Memory: one big pool shared by CPU and GPU | 121 GiB unified | DMA pool is 8 MiB (`native/kernel/README.md`); heap 64 MiB; frame allocator from the UEFI map | GB-scale pinned and mapped regions, huge pages, a 27B weight image (the live server holds 38 GB resident) | A single multi-GiB allocation mapped for both sides | Yes (allocator in QEMU) |
| G6 | Omega kernels, session, launchers, resident runtime | Omega repo | Codegen and matmul files link freestanding; 14 fixture kernels byte-identical (omega#419, host only). Session, launchers, resident runtime, accelerator world: Linux-tangled (dependency map sections 2 and 5) | Cut A platform services (lock, clock, sleep, threads), handle types instead of `NvrmMem`, native backend beside the Blackwell engine, then cut E ports | Omega gates run on the native backend | Yes for the platform-services and handle work |
| G7 | Model load and inference | 27B weights on NVMe (live: `aien-serve-http` 38 GB resident) | CPU inference exists: `aienos-infer`, Pi 5 run 1.20 tok/s, KV capacity 64 in that probe (`docs/native/PI5-6-INFER-RUNS.md`). No GPU KV code (`GB10_NATIVE_STATUS.md`) | Everything on the Spark: model read from NVMe through a driver with queue depth 1, GPU path, KV, scheduler for several requests | A token stream from the Spark natively; speed reported against the hardware ceiling (not measured) | CPU path yes; GPU no |
| G8 | The model server's API | TCP port 18091, HTTP | **None** (no TCP, no TLS) | A transport the clients can use: either TCP+HTTP, or Aienux's own signed UDP control transport (`native/net/aienos_sec.c`, TEST identities only) with a small gateway on another machine | A request from another machine answered by the Spark | Yes (QEMU) |

### Class U: rows that need userland AienOS does not have

AienOS has a capability kernel, EL0 tasks and signed "OSC" units (`native/kernel/artifact/osc_launch.c`), a small shell over the keyboard and a compat shim for building shared code (`svc/compat`: `stdio.h`, `stdlib.h`, `string.h`, `time.h`, `pthread.h` stubs). It has **no** POSIX layer, no dynamic linker, no process model that loads a Linux executable, no TCP/IP, no TLS, no filesystem other than Store v1 objects (OBSERVED: grep for `ssh`, `dhcp`, `tls`, `tailscale`, `wireguard` in `native/` finds nothing).

| # | Linux today | What it needs | Aienux today | Verdict |
| --- | --- | --- | --- | --- |
| U1 | `ssh` into the Spark (port 22) | TCP, a key exchange, an ssh server, a shell | None. Operator console is keyboard plus screen; remote control is the custom signed UDP transport | **Another machine.** Replace ssh with the signed control transport plus a gateway. A native ssh server is not worth building |
| U2 | `tailscaled` (the tailnet) | WireGuard, DERP relays, the Tailscale control plane. Curve25519 exists (`native/net/x25519.c`); the rest (ChaCha20-Poly1305, BLAKE2s, coordination) does not. Rule: never hand-roll crypto | None | **Another machine** (the hub or the Mac keeps Tailscale and forwards). AienOS is reached on the LAN |
| U3 | Claude agents (`claude` is a dynamically linked Linux aarch64 ELF; about a dozen running), Codex | Linux ABI, TLS 1.3 to the API, DNS, a large POSIX runtime | None, and none planned (ADR 0024: Rust as scaffolding, Omega as destination; no external runtime) | **Another machine, permanently.** Not a gap to close, a boundary to state: agents drive AienOS from outside |
| U4 | git (Forgejo server on 3000/2222/3443, GitHub clones in `~/workspace`) | filesystem, TCP/TLS or ssh, the git program | None | **Another machine.** Forgejo data sits on a TPM-sealed gocryptfs; moving it is an operator step (section 8) |
| U5 | Compilers and the test judge (`rustc`, `cargo`, `gcc`, `ld`, QEMU; Phase 3 made the Spark the only judge, `472ed37`) | a POSIX userland and toolchain on aarch64, or an aarch64 Linux host elsewhere | None | **Another machine.** Needs an aarch64 Linux judge (the QEMU gates need one). UNKNOWN whether the hub qualifies. This is a decision, not a build item |
| U6 | Docker and the seven containers, Prometheus, Grafana, Vaultwarden, uptime-kuma, waitlist, authelia, conduit, OliveTin, spark-mail, cortex-rs, translate, cockpit | Linux containers or each service rebuilt | `cortex-store` exists as a Store-backed library (`svc/cortex_store.c`); nothing else | **Another machine** for all of them. Cortex is the one candidate to run natively later (it already builds freestanding) |
| U7 | UPS monitoring (`nut`), power sensors (`spark-sensors`) | USB HID power class driver, a metrics path out | None | Keep on another machine reading the UPS, or the native kernel reports watts over UDP. Low priority |
| U8 | Yubikey (CCID/FIDO) | USB CCID/FIDO class driver | None. Owner key ceremony is designed around its own files (`docs/OWNER_KEY_CEREMONY.md`); which flow here uses the Yubikey: UNKNOWN | Decide when the owner-key flow is chosen |
| U9 | Firmware updates (`fwupd`), package updates, apache2, cups, snapd | Linux | None | Not replaced. Firmware updates become an operator step done from a recovery stick or Linux before cutover |
| U10 | Logging, metrics, journals | `journald`, `rsyslog`, `rasdaemon`, `smartmontools` | Kernel log into the Store (`core/klog.c`, `klog_slot.c`), reports over UDP on iMac | Native logging exists in QEMU; SMART and RAS (memory error) reporting do not |

### Class P: platform and policy

| # | Item | State |
| --- | --- | --- |
| P1 | Trust chain for physical boots | Secure Boot is **off** by owner decision (2026-10-01). Gates record it. Release builds must have it on. The TRUST-1 owner key ceremony (`docs/OWNER_KEY_CEREMONY.md`) is BLOCKED_OPERATOR |
| P2 | Rollback and recovery | Boot0004 recovery stick procedure exists (`docs/RECOVERY_MEDIA_MACHINE1.md`). A/B slots and rollback: QEMU gates; C loader `M0_ROLLBACK` NOT_RUN (D1) |
| P3 | Storage of record | Ext4 root with 2.5T used. Store v1 is the AienOS format (frozen, ADR 0015). No importer from ext4. Atlas data is gocryptfs under a TPM-released key |
| P4 | Plan authority | Whole-system order is owned by `aien-spark/aien-architecture/CURRENT_EXECUTION_PLAN.md`; the dependency map records an open order decision (#163: operator input first, or trust and network first). This document does not resolve it |

## 6. Where each thing runs after Linux is off (blunt)

| Work | Runs on | Why |
| --- | --- | --- |
| The 27B model, its cache, its scheduler | The Spark, on AienOS | The point of the project |
| A small request gateway and TLS front | Another machine (hub or Mac) | AienOS has no TCP or TLS; clients keep working over HTTPS to the gateway, which talks Aienux's signed transport to the Spark |
| Claude and Codex agents, git, Forgejo, GitHub work | Another machine | They are Linux programs with no native counterpart and no plan for one |
| Compiling AienOS, QEMU gates, the test judge | Another aarch64 Linux machine | Same; Phase 3 decision (`472ed37`) must be revisited, recorded as a Drake decision |
| Containers and dashboards | Another machine | Same |
| Tailscale | Another machine | Same |
| Install, update, recovery | A recovery stick plus an attended operator | No Linux left to do it |

INFERRED (high): "Linux 100% replaced on the Spark" cannot also mean "everything that runs on the Spark today keeps running on the Spark". It means the Spark runs only AienOS code. UNKNOWN: whether a second aarch64 Linux machine with enough memory for the QEMU gates and the build exists (the hub is a Linux peer on the tailnet; its hardware was not inspected).

## 7. Shortest ordered path

Notation: **[H]** host or QEMU only, a builder can do it now; **[A]** needs an attended boot of the Spark (Linux stays the default, one-time `BootNext`); **[C]** needs the real chip; **[O]** Drake only; **[X]** another machine.

### To the first useful native boot

"Useful" here: the Spark boots the C kernel from its own disk, shows the console on the HDMI screen, reads its Store from the NVMe, answers over Ethernet, takes a keyboard, and returns to Linux. No GPU, no model.

1. [H] Re-run the Spark-shape SMMU host case and the QEMU gates on current main, and correct the stale README paragraph (D3), so the physical boot starts from a recorded green baseline (evidence: test names and exit codes).
2. [H] Realtek RTL8127 driver against a chip model, plus a UDP report sender like the iMac's (`feat/imac-net-report`). Find out whether Realtek publishes a register document first; if not, record DOCS SILENT and use the Linux `r8127` source as read-only reference (the manual-guard hook requires the quote before any chip code).
3. [H] NVMe queue depth above 1 and direct DMA in QEMU, on a 512-byte disk image (D4).
4. [H] TPM2 CRB driver against swtpm (D9) and the wall-clock read (D10).
5. [A] Attended boot #1: C kernel, `kernel: alive`, device list matching section 3, GOP console photo, serial if present. Pass criterion: Linux returns with the 10 invariants of `verify_native_rollback.sh` unchanged. Reuses `scripts/stage_one_time_boot_ck.sh`; uses `BootNext` only.
6. [A] Attended boot #2: NVMe read-only (GPT and Store superblock), network report out, keyboard. Uses the report-over-Ethernet pattern from the iMac so no photograph is needed; the UEFI PXE entry (`Boot0003`) is an alternative way to deliver a kernel without touching the ESP (INFERRED, medium; the iMac netboot lane `feat/imac-netboot` is the model).
7. [A] Attended boot #3: Store write, flush, clean NVMe shutdown, crash recovery on the real disk, 512-byte path (D4).

### To a useful native model run (CPU first, then GPU)

8. [H] Cut A work (platform services layer, handle types, freestanding link target) in Omega and PHYSICS (G6).
9. [H] L192 and L193 finish (G2, G3). Then a simulated-device end-to-end: GSP boot bytes, MMU, channel, vector add on the model.
10. [A] Native CPU inference on the Spark with the existing `aienos-infer` path (small model first, G7). Reports speed against the memory-bandwidth ceiling (not measured; the Spark's ceiling has to be measured, not assumed).
11. [A] GB10 discovery by the kernel with ownership of BAR0 (G1).
12. [C][O] GSP firmware boot on the chip (G2). Operator step, section 8.
13. [C] First compute: integer vector add, teardown and reopen (G3, cut D).
14. [C] Cut E in Omega order: matmul, attention, numeric launchers, session, resident runtime (G6), each with receipts against the Linux reference (kept as the reference per the dependency map).
15. [C] Load the 27B weights, KV on the GPU, token stream (G7), then the request path (G8).

### To Linux fully off

16. [X] Stand up the gateway, git, agents, judge and dashboards on other machines (section 6). Move Forgejo and the Atlas data off the TPM-sealed mounts. Retire or relocate each service in section 4.
17. [H] A rollback-safe installer: A/B slot of the kernel on the ESP, Store v1 on its own partition (the existing `native/boot` slot code and `DISK_LAYOUT` gate).
18. [O] Drake decides the cutover and runs it: firmware update first (if wanted), then boot-order change, with the recovery stick present. The doctrine today forbids touching `BootOrder` (`docs/NATIVE_BOOT_ONE_TIME.md` invariant 2); this plan needs that invariant lifted for exactly one change, in writing.
19. [A][O] A soak: weeks on AienOS with Linux still on the disk and one boot-order change away.
20. [O] Only then wipe or repartition. Not before.

## 8. Operator steps (Drake only)

None of these is authorized by this document.

- Plug in an HDMI monitor, a plain wired USB keyboard in a rear USB-C port, and the `AIENOSRECOV` stick before each attended boot; stage and boot per `docs/NATIVE_BOOT_ONE_TIME.md`.
- Key ceremony: owner keys and machine id for the hardware image (`docs/OWNER_KEY_CEREMONY.md`); the TEST fixture keys are refused for the hardware image.
- Photograph or capture the console at each boot and hand it to the orchestrator.
- Allow the first firmware load and GSP handoff on the chip, and every later GPU use under AienOS.
- A reset or power cycle to test recovery.
- Look at the unit (no reboot): is a ConnectX port or a serial header physically there. Or confirm the unit's spec.
- Decide where the build and agent work lives after cutover (section 6), and approve the Phase 3 judge move.
- Re-seal or migrate the Atlas data before any Linux shutdown that changes what the TPM measures. Secure Boot on or off changes PCR 7 and previously locked the stores (`docs/NATIVE_BOOT_ONE_TIME.md`).
- Lift or amend the `BootOrder` invariant for the one cutover change.

## 9. What is tracked where

| Item | Owner / place |
| --- | --- |
| This map | L191, `docs/SPARK_REPLACE_LINUX.md` |
| GPU boot protocol (host side) | L192, `feat/gb10-gsp-host` |
| GPU page tables and channel (simulated) | L193, `feat/gb10-gpu-mmu-sim` |
| Network report and live kernel swap pattern | L142, `feat/imac-gpu-bench`, `feat/imac-net-report`, `feat/imac-netboot` |
| Cut A through E definitions | `docs/GB10_NATIVE_DEPENDENCY_MAP.md` |
| Whole-system order | `aien-spark/aien-architecture/CURRENT_EXECUTION_PLAN.md` |

## 10. Limits of this document

- The Linux side is a snapshot of one afternoon. Services change daily.
- Aienux claims come from source and docs at `18e2765`; no test was run for this document. Gate names and results are quoted from `ROADMAP.md`, `native/kernel/GATES.md` and `native/kernel/README.md`, not re-run here, and those pages are partly older than main. Where they disagree with a newer commit (D3), the row says so.
- The ConnectX finding is a negative `lspci` result on this unit, nothing more.
- No time, effort or speed estimate appears here because none was measured.
- Reuse search: origin/main and `git branch -r` (519 branches), the in-flight worktrees for L192 and L193, the ledger, and org-check on `r8127 realtek` and `tpm crb` (0 hits each in 98 repos, 2026-10-20T15:56Z). `PHASES.md` named in the brief does not exist in this repo; `ROADMAP.md` was used.
