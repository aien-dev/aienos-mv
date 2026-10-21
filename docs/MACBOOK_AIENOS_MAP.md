# AienOS on the MacBook Air (Apple Silicon M2): gap map and ordered path (L208)

Status: read-only inventory plus documentation and source reading. No reboot, no boot-order change, no firmware load and no live device access were done to write this. The MacBook was not touched by this work; its inventory was collected earlier on the Mac itself and copied to the Spark. Every claim is tagged OBSERVED (read from the Mac's dumps, from this repo at the pinned commit, or from a cited document or source file), INFERRED (with the reasoning), or UNKNOWN (with the experiment that would resolve it). No time, effort or speed estimate appears because none was measured.

Pins: this repo at `18e2765` (origin/main, 2026-10-19). Machine: MacBook Air (13-inch, 2022), `Mac14,2`, board `J413AP`, chip Apple M2 (`T8112`), macOS 26.6.2 (build 25G83), read on 2026-10-20. Raw dumps: `~/handoffs/macbook-inventory-2026-10-20/` (`system_profiler.txt`, `ioreg-devicetree.txt` = the Apple Device Tree, `ioreg.txt`, `sysctl-hw.txt`, `sw_vers.txt`, `csrutil.txt`, `bputil.txt`).

Corrections (L372, 2026-10-21): section 4.3 firmware question resolved, licence dates added, GPU trace plan corrected (G3 step 3, step 14, section 7 rows); each is marked "L372" in place. Family-wide view: `docs/APPLE_SILICON_FAMILY_MAP.md`.

Names (Drake, 2026-10-20): Aienux is the kernel, AienOS is the system. Older files still say AIENOS for both. Sibling maps: `docs/SPARK_REPLACE_LINUX.md` (L191, the Spark, on branch `feat/spark-replace-linux-map`, `bb2399f`); the iMac, Pi and PC maps are in flight and not on main yet.

## 1. Plain-English summary

The MacBook is a different kind of computer from everything AienOS has booted so far. The Spark, the Pi and the iMac all start through a firmware that speaks UEFI and describes the machine in standard tables (ACPI), and they all use standard chips (PCI devices, a standard interrupt controller, standard NVMe disks). The M2 MacBook uses none of that. Apple built its own boot chain, its own interrupt controller, its own memory-protection unit for devices, its own disk controller and its own GPU. Almost every device on the board sits behind a small helper processor that runs Apple firmware, and the main processor talks to those helpers through a mailbox.

Three facts decide the plan:

1. **Apple allows a third-party kernel to boot, officially but unsupported.** Apple's Platform Security Guide describes a "Permissive Security" setting that accepts "locally Secure Enclave-signed boot objects", for example a custom kernel. It is set per operating-system install, from recovery, with the owner's password. The Asahi Linux project uses exactly this to boot Linux. Nothing about it requires breaking Apple's security; it does require changing the boot setup of one new install, never the existing macOS.
2. **Asahi Linux has already reverse engineered almost every chip on this machine, and published it.** That makes most rows "documented by a third party", with working open-source drivers to read. Two rows are the exception: the GPU is documented only by Asahi's own notes (unfinished) and its driver, and a few helper-processor protocols are known only from Asahi's code.
3. **AienOS's existing aarch64 kernel carries over for the processor itself** (page tables, exceptions, timer, threads, capabilities, the Store) but needs a new "platform layer": a device-tree reader instead of ACPI, Apple's interrupt controller beside the two ARM ones it already supports, a different way to wake the other cores, 16 KiB device memory pages, and a different disk and network path.

Shortest path in one line: boot Aienux **tethered**, which means the Spark sends the kernel over a USB-C cable into the MacBook's RAM each time, so nothing about AienOS is stored on the MacBook; first printing a line to the screen, then reading the disk read-only, then network, then keyboard, then everything else. The only change to the MacBook is a one-time setup from recovery mode (section 10), and that is Drake's decision.

## 2. How the live machine was read

All collected on the Mac, read-only, then copied. Nothing was run by this work on the Mac.

| File | Content |
| --- | --- |
| `system_profiler.txt` | `system_profiler` hardware, graphics, NVMe, Thunderbolt, network, Wi-Fi, Bluetooth, audio, power, memory |
| `ioreg-devicetree.txt` | the Apple Device Tree (ADT) as macOS sees it: every chip block, its address, interrupts, compatible string |
| `ioreg.txt` | the full I/O Registry (driver view, 31171 lines; not decoded here beyond headers) |
| `sysctl-hw.txt` | CPU features, cache sizes, cluster layout |
| `bputil.txt`, `csrutil.txt` | `bputil` output was "The tool requires running as root" (no policy read); `csrutil`: "System Integrity Protection status: enabled." |

Gap in the inventory: the current boot policy was not read (needs root). The ADT `/chosen` node does carry security state (section 4). Re-collect with `bputil -d` as root if wanted; that is also read-only.

## 3. The hardware, exactly (OBSERVED, `system_profiler.txt` and `ioreg-devicetree.txt`)

| Part | Observed | Where in the ADT (address = physical, from the node's `reg`) |
| --- | --- | --- |
| Model | MacBook Air `Mac14,2`, `MLXX3LL/A`, `compatible = "J413AP","Mac14,2","AppleARM"`, `platform-name t8112`, firmware `18000.161.10` (iBoot stage 1 and 2 both `mBoot-18000.161.10`) | root, `/chosen` |
| CPU | 8 cores: 4 Avalanche performance (`apple,avalanche`, L1i 192 KiB, L1d 128 KiB, L2 16 MiB shared by 4), 4 Blizzard efficiency (`apple,blizzard`, L1i 128 KiB, L1d 64 KiB, L2 4 MiB shared by 4) | `cpu0..cpu7`; `sysctl-hw.txt` |
| CPU features | CRC32, LSE and LSE2 atomics, AES, PMULL, SHA1/256/512/3, RDM, DotProd, FHM, BF16, I8MM, FCMA, JSCVT, PAuth/PAuth2/FPAC, BTI, SSBS, DIT, ECV, CSV2/CSV3, DPB/DPB2. **No** SVE, SME, MTE; no `FEAT_RNG` key appears in `sysctl-hw.txt` | `sysctl-hw.txt` |
| Memory | 8 GiB LPDDR5 (`dram-size` 0x2_0000_0000), Micron | `/chosen`, `system_profiler.txt` |
| Interrupt controller | Apple Interrupt Controller, generation 2 (`aic,2`), 8 main CPUs, `reg` 0x2_3B0C_0000 length 0x2_0000 (calc: shell `printf %x 9580576768`), `aic-iack-offset` 0x8000 | `aic@3B0C0000` |
| Timer | ARM generic timer, `clock-frequency` 24,000,000 Hz (bytes `00366e01` little endian 0x016E3600) | `device-tree` root |
| DART IOMMUs | 25 `dart,t8110` instances (OBSERVED, `grep -c`), one per device block (USB, PCIe, display, ISP, ANE, ANS, SEP, ...). `page-size` property on `dart-apcie0` = 0x4000 (16 KiB) | `dart-*` nodes |
| Disk | Apple SSD AP0512Z, 500.28 GB, controller **ANS2** behind a helper processor (`ans@77400000`, `iop,ascwrap-v4`), plus `sart-ans@7BC50000` (`sart,coastguard`). ADT keys `nvme-queue-entries` 0x40 (64), `nvme-linear-sq`, `nvme-vdma-wa`, interrupt index 4. Volumes: `iSCPreboot` 524 MB, `Macintosh HD - Data` 494 GB (40.9 GB free), `Recovery` 5.4 GB (APFS container) | `ans@`, `sart-ans@` |
| Wi-Fi | Broadcom BCM4387, PCI id `14e4:4387`, over Apple PCIe, firmware string `wl0: Jan 20 2026 ... version 20.140.13.0.8.7.224` (loaded by macOS from its own files) | `apcie@90000000/pci-bridge0/wlan` |
| Bluetooth | Broadcom BCM4387, USB product id 0x4A09 reported as `Transport: PCIe`, firmware `23.5.636.5343` | `bluetooth-pcie` (function 1 of the same chip) |
| PCIe | Apple PCIe root `apcie,t8112` at 0x6_9000_0000, four interrupts, DART `dart-apcie0` | `apcie@90000000` |
| USB-C / USB4 | two Thunderbolt/USB4 ports (40 Gb/s each), each: ATC PHY (`atc-phy,t8112`), DWC3-type USB controller (`usb-drd,t8112`), DART, ACIO (USB4 tunnel engine, its own helper), CD3217 port controllers (`usbc,cd3217`) on I2C | `atc-phy0/1`, `usb-drd0/1`, `acio0/1`, `hpm0/1` |
| Display | internal 2560x1664 Liquid Retina; display controller `disp0,t8112` with helper `dcp@31C00000`, a second external controller `dispext0` with `dcpext`; Parade `DP855` timing controller on SPI; two DisplayPort crossbars (`atc0/1-dpxbar`). The inventory was taken with an LG UltraFine 4K attached | `disp0@30000000`, `dispext0@70000000`, `dp855` |
| GPU | Apple M2 GPU, 10 cores, `sgx@4000000`, `compatible gpu,t8112`, MMIO 0x2_0400_0000 (64 MiB) and 0x2_04D0_0000, five interrupts, its own helper `gfx-asc@6400000`. Metal 4 under macOS | `sgx@`, `gfx-asc@` |
| Keyboard and trackpad | **Not SPI.** The ADT has no HID node on any SPI bus (`spi1` = SPI NOR, `spi2` = `biosensor,mesa` Touch ID, `spi4` = `parade,DP855`). Input is the **MTP** (Multi-Touch Processor): `mtp@4E400000` (`iop,ascwrap-v4`), `dockchannel-mtp@4EB00000`, `hid-transport,mux` | `mtp@`, `dockchannel-mtp@` |
| Audio | `mca`/`mca-switch` I2S-style clusters at 0x2_3840_0000, speakers on `audio-control,sn012776` amplifiers (I2C), headphone codec `cs42l84`, AOP microphone path (`aop-audio`, `audio-pdm2`), HDMI path via the external display | `mca*`, `audio-*`, `aop@4A400000` |
| System controller | `smc@3E400000` (`iop,ascwrap-v4`) with `smc-pmu`, charger, temperature sensors; PMIC over SPMI (`pmu-main` Stowe, `pmu-aux` Vale); battery gauge `bq40z651` | `smc@`, `nub-spmi*` |
| Power, clocks | `pmgr@3B700000` (`pmgr1,t8112`), `clpc`, `soc-tuner`; watchdog `wdt@3D2B0000`; GPIO blocks (`gpio@3C100000`, `aop-gpio`, `smc-gpio`, `nub-gpio`) | `pmgr@`, `wdt@`, `gpio@` |
| Security | Secure Enclave `sep@5E400000` (`iop-sep,ascwrap-v4`), Touch ID sensor `mesa`, AES engine `aes@3500C000`, Apple's own `error-handler`, `mcc` memory controller | `sep@`, `aes@`, `mcc@` |
| Other engines | ISP/camera (`isp`), Neural Engine (`ane@6A000000`), video encode/decode (`ave2`, `avd`), JPEG, scaler, NFC `stockholm` | various |
| UART | Samsung-style `uart0@35200000` (debug UART, 0x2_3520_0000); `dockchannel-uart@3D128000` | `uart0@`, `dockchannel-uart@` |
| Security state | `effective-security-mode-ap = 1`, `effective-security-mode-sep = 1`, `certificate-security-mode = 1`, `security-downgradable = 1`, `external-boot = 0`, `debug-enabled = 0`, `cs-system-policy = "default"`, `allowed-boot-args = "trace,trace_wake,kperf,-x,-v,..."`, `boot-manifest-hash` present; macOS reports SIP enabled and Activation Lock enabled | `/chosen`; `csrutil.txt` |
| Network | Wi-Fi `en0`, Tailscale VPN (utun4 `100.64.0.1`), a USB Ethernet dongle (AX88179B), a phone tether | `system_profiler.txt` |

Every Linux-side address above was cross-checked against the open `t8112.dtsi` (section 4 reference list): AIC 0x2_3B0C_0000, SART 0x2_7BC5_0000, ANS 0x2_7740_0000, PCIe 0x6_9000_0000, SGX asc 0x2_0640_0000 all agree with what this machine reported (OBSERVED, `printf %x` conversions of the dump's decimal addresses).

## 4. The boot chain and how a foreign kernel is allowed to boot

### 4.1 What Apple documents (OBSERVED, cited)

- Source A: Apple Platform Security Guide, "Startup Disk security policy control for a Mac with Apple silicon", `support.apple.com/guide/security/sec7d92dc49f`. Quotes: "Full Security is the default, and it behaves like iOS and iPadOS." "Reduced Security is similar to Medium Security behavior on an Intel-based Mac with a T2 chip." "Permissive Security is for users who accept the risk of putting their Mac into a much more insecure state." "...setting the policy to Permissive signals to iBoot that it should accept locally Secure Enclave-signed boot objects." Custom boot object example: "a user-generated Boot Kernel Collection built from a custom XNU kernel." "Apple doesn't provide or support custom XNU kernels." Loading one makes "some decryption keys become unavailable."
- Source B: Asahi Linux, "Apple Silicon Platform Security", `asahilinux.org/docs/platform/security/`. Permissive "allows the enrollment of macOS kernelcaches that are not signed by Apple into a Boot Policy" and disables the sealed system volume (SSV). Demoting a container to Permissive "can only be done from 1TR", which "can only ever be entered from a cold boot by holding the physical power button, and authenticating with Machine Owner credentials." Per-container: "Apple's policies for other containers do not affect the Asahi volume", and the Asahi project commits to never altering other OS volumes. Asahi "uses Apple's tooling to set its security to Permissive and enroll m1n1 as its signed boot object."
- Source C: Asahi Linux, m1n1 User Guide, `asahilinux.org/docs/sw/m1n1-user-guide/`. Install: "`kmutil configure-boot -c m1n1-stage1.bin --raw --entry-point 2048 --lowest-virtual-address 0 -v <path to your OS volume>`", run from the recovery of that OS volume ("Each OS has its own recovery OS"). "Payloads are simply concatenated after the initial m1n1 binary." Supported payloads: device trees (optionally compressed), initramfs, and kernel images in Linux ARM64 format. "If given no payloads, or if booting the payloads fails, m1n1 will fall back to proxy mode."
- Source D: Asahi Linux, tethered boot, `asahilinux.org/docs/sw/tethered-boot/`. m1n1 stage 1 has a "backdoor proxy mode" for tethered boot, enabled from 1TR with machine authentication ("requires machine authentication for security"), controlled by the SIP-disable flag on the Asahi volume (per-OS, "macOS installs are unaffected"); a USB-C cable to a Thunderbolt port; host loads the kernel and device tree with `linux.py`; the m1n1 virtual serial port is enough, "a real serial port is only needed for low-level SoC debug UART access."
- Source E: Asahi Linux, boot process guide, `asahilinux.org/docs/alt/boot-process-guide/`. The chain: "[Apple stuff] → m1n1 stage 1 → m1n1 stage 2 → DT + U-Boot → GRUB → Linux". Stage 1 "is signed by an internal machine-specific key in the process (as part of Apple's secure boot policy) and can be considered immutable." Stage 2 "is in charge of initializing (more) hardware, choosing the appropriate DT for this platform" and loading U-Boot, which "provides UEFI services" and loads `EFI/BOOT/BOOTAA64.EFI`. "Linux needs DT data to bring up hardware." The guide warns that m1n1, U-Boot, the kernel and the device trees have version interdependencies.

### 4.2 The chain on this machine (INFERRED from A to E and the ADT, not traced live)

1. SecureROM in the chip, then iBoot (stage 1 `mBoot-18000.161.10`, stage 2 the same tag), verified by Apple's signatures. The Secure Enclave (SEP) holds the boot policy (OBSERVED: `sep@5E400000`, `boot-manifest-hash`, `security-domain = 1`).
2. For a Permissive-Security OS container, iBoot accepts a boot object signed locally by the SEP with the owner's approval instead of an Apple-signed kernelcache. That boot object is m1n1 stage 1 (Source B, C).
3. m1n1 stage 1 runs at the highest exception level the machine gives it (INFERRED, not verified: m1n1 provides a "bare-metal hypervisor that can boot Linux or macOS", `asahilinux.org/docs/platform/open-os-interop/`, which needs EL2). It starts other code in the Linux arm64 boot-protocol convention: MMU off, device tree address in a register. Our kernel's current boot code already handles "firmware calls `_start` at its own EL (EL2 on QEMU...)", `native/boot/efi_entry.S:2` (OBSERVED), and drops from EL2 to EL1; whether the same works here is UNKNOWN until run.
4. Two ways for Aienux to be started from there:
   - **Path T (tethered, first choice):** the Spark runs m1n1's Python tools over USB-C and pushes the kernel and a device tree into RAM (Source D). Nothing of AienOS lives on the Mac. A power cycle returns to macOS.
   - **Path U (standalone, later):** m1n1 stage 2 and U-Boot on the Mac's own EFI partition load an EFI binary (Source E). Aienux's kernel is already a UEFI application (`native/boot/efi_main.c`), so this path reuses it, but needs the device-tree table in place of ACPI.
5. macOS stays. Asahi's rule is one new APFS container per extra OS, with its own stub, recovery and policy (Source B). The existing macOS container keeps Full Security.

### 4.3 Honest limits of this section

- RESOLVED (L372, 2026-10-21, OBSERVED in source): **macOS 26.6.2 / system firmware 18000.161.10 is known to m1n1.** m1n1's firmware table has `{V26_6_2, "26.6.2", {26, 6, 2}, 3, "mBoot-18000.161.10"}` (`src/firmware.c`, added in commit `3e354a2` on 2026-09-30, contained in tags v1.9.9 and v2.0.0; read at m1n1 `7cde7d1`). The Asahi installer (`src/main.py` at `e93743d`) checks only **minimum** versions (`MIN_MACOS_VERSION = "13.5"`, chip minimum `0x8112: "12.4"`, and per-IPSW minimum iBoot/SFR); it has no maximum, so a newer system firmware is accepted. The new container gets its **own** boot firmware from the installer's IPSW list (macOS 13.5 for the M2; 14.8.3 is for M3 boards only), separate from the 26.6.2 that the main macOS keeps. No firmware change on the Mac is needed. Details: `docs/APPLE_SILICON_FAMILY_MAP.md` section 4.
- UNKNOWN: the Mac's current policy (not read; needs root). Experiment: `bputil -d` from the Mac's recovery or an administrator shell, read-only.
- OBSERVED risk: this Mac holds **Activation Lock** and is the machine Drake chose for the owner-key ceremony (`docs/OWNER_KEY_CEREMONY.md`, "MacBook, Wi-Fi off"; mind map note 2026-10-12). Adding a Permissive container does not change the macOS container's policy according to Source B, but it is the kind of change that belongs on a spare Mac, not the key machine. Section 10 puts this to Drake.
- Brief corrections (OBSERVED): (a) the brief lists keyboard/trackpad as SPI; on this M2 they are on the MTP helper (section 3), no SPI HID node exists. (b) The brief says m1n1 is MIT. The m1n1 README says "m1n1 was made available under the terms of the MIT license until version 1.9.9. Starting from version 2.0.0, m1n1 is licensed under the terms of the GNU General Public License version 2 (or later)" (github.com/AsahiLinux/m1n1). Current m1n1 is GPL-2.0-or-later with MIT, BSD, zlib and CC0 third-party parts. AienOS **runs** m1n1 as a separate program (nothing is linked or copied), so the licence does not touch our kernel, but its source is read-only reference, never copied into Aienux. Dates (L372, OBSERVED with `git log` on the tags): v1.9.9, the last MIT release, and v2.0.0, the first GPL release, were both tagged on 2026-10-02, and v1.9.9 already contains the macOS 26.6.2 firmware entry.

## 5. What AienOS already has for aarch64, and what changes (OBSERVED, repo at `18e2765`)

| Piece | Today in `native/kernel` | On the MacBook |
| --- | --- | --- |
| MMU and page tables | `mm/mmu.c`, `mm/pt.c`: stage-1 AArch64, **4 KiB granule**, 48-bit VA (`mm/pt.h:1`). QEMU PASS (`AIENOS_CK_M1`) | Carries over, with a granule decision. Apple's DARTs use 16 KiB pages (OBSERVED `page-size` 0x4000 on `dart-apcie0`; Asahi: "Its DART IOMMU hardware only supports 16K pages", alpha release notes, `asahilinux.org/2022/03/asahi-linux-alpha-release/`; Linux driver refuses a domain when the DART page size exceeds the kernel page size, `drivers/iommu/apple-dart.c`). Two options: switch the kernel granule to 16 KiB (touches `pt.c`, `mmu.c`, frames, heap), or keep 4 KiB and give DART-backed buffers 16 KiB-aligned 16 KiB blocks. Whether the M2 cores accept a 4 KiB EL1 granule is UNKNOWN: read `ID_AA64MMFR0_EL1.TGran4` through m1n1's proxy (read-only). |
| Exceptions, EL2 to EL1, threads, preemption, EL0, capabilities, IPC | `arch/exception.c`, `vectors.S`, `m3.S`, `core/sched.c`, `m3.c`: QEMU PASS | Carries over. Apple timer interrupts arrive as FIQ, not IRQ (below), so the vector table needs an FIQ path (the Pi/Spark path uses IRQ). |
| Timer | `arch/timer.c`: ARM generic timer through the GIC | The generic timer registers are the same; delivery changes (below). 24 MHz (OBSERVED). |
| Interrupt controller | `arch/gic.c`: **GICv3** (Spark, QEMU) and **GICv2** (Pi 5 GIC-400), from the ACPI MADT | **A third controller: Apple AIC2.** Table below. |
| Boot handoff | UEFI image (`native/boot/efi_main.c`), hardware description from **ACPI** (`core/acpi.c`, `smp.c:5`: "AAVMF gives ACPI, not a device tree") | Apple hardware has no ACPI. Hardware is described by a device tree. A device-tree reader is new work (host-testable). `native/boot/diag_main.c:126` already knows the EFI Device Tree table GUID. |
| Secondary cores | `core/smp.c`: **PSCI** `CPU_ON` over the conduit the FADT names | No PSCI on Apple. The open `t8112.dtsi` marks all eight cores `enable-method = "spin-table"` (OBSERVED, line 67 onward); m1n1 parks the others. A spin-table start is new code; resetting and powering off go through the SMC chip (`smc-reboot` in `t8112.dtsi`), not PSCI. |
| IOMMU | `core/smmu.c`: ARM SMMUv3 | Replace with a DART service. DART2 (`t8110`) register map documented in Linux `apple-dart.c` (below). |
| Entropy | `arch/rndr.c`, `core/entropy.c`: **RNDR or refuse** (`FEAT_RNG`, `ID_AA64ISAR0_EL1` bits 63:60) | `sysctl-hw.txt` lists many `FEAT_*` keys and no `FEAT_RNG` (OBSERVED). INFERRED: the M2 has no RNDR. Then the kernel refuses to start its key hierarchy. Sources of entropy to evaluate: the SEP, and iBoot's `random-seed` property in `/chosen` (present but all zeros in the dump, INFERRED redacted). UNKNOWN; see row F7. |
| Disk | `native/disk/disk_nvme.c`: standard NVMe (QEMU PASS, `QEMU_NVME*`, `M4_NVME`), one command in flight | The disk is **ANS2**, a non-standard NVMe: needs the ANS helper firmware booted through RTKit, SART, a linear submission queue and NVMMU tags (row D4). The Store (`native/store`) and the M5 crypto sit above the block layer and carry over unchanged. |
| Network | virtio-net, Pi 5 GEM, iMac BCM57766 transmit-only report; stack ARP, IPv4, ICMP, UDP | The MacBook has no built-in wired NIC (OBSERVED: only Wi-Fi plus USB dongles). First network is USB Ethernet (AX88179B is on this Mac) or Wi-Fi. |
| Screen | UEFI GOP framebuffer console (`native/boot/efi_gop.c`, `core/fbcon.c`) | iBoot leaves a framebuffer described in the ADT (OBSERVED: `framebuffer-width`, `framebuffer-height` properties exist on the active display node; the dump shows 4096 x 2160 for the external 4K display). Using it needs no display driver, only the address and format. UNKNOWN until read from a live m1n1 boot: the internal panel's framebuffer address. |
| Store, M5 keys, signed slots, recovery | `native/store`, `native/m5`, `native/boot/slot_*.c`, `crates/aienos-*` | Carry over. They depend on a random source and a clock (rows F7, D10). |

### GICv3 versus Apple AIC: the difference (OBSERVED, cited)

| | GICv3 (Spark) | Apple AIC2 (M2) |
| --- | --- | --- |
| Description | ACPI MADT lists distributor and redistributors | ADT node `aic@3B0C0000`, `compatible aic,2` (OBSERVED); Linux `t8112.dtsi`: core registers 0x8000 bytes at 0x2_3B0C_0000, **event register** at 0x2_3B0C_8000 |
| Structure | distributor + one redistributor per core + CPU interface system registers (`ICC_*`), up to 1019 SPIs | one MMIO block; Linux `irq-apple-aic.c` header: "896 level-triggered hardware IRQs", "2 per-CPU IPIs"; no redistributors, no ITS, no message-signalled path in the block itself |
| Acknowledge | read `ICC_IAR1_EL1` (system register), write `ICC_EOIR1_EL1` | read the event register: "Reading `AIC_EVENT` acknowledges and masks the IRQ automatically"; event word fields die [31:24], type [23:16] (FIQ 0, IRQ 1, IPI 4), number [15:0]; end of interrupt = unmask |
| Routing | `GICD_IROUTER` affinity | AIC2: per-IRQ config word, target in bits 3:0, delay index in bits 7:5, at `AIC2_IRQ_CFG` 0x2000 (AIC3 at 0x10000); per-die blocks of config, SW set/clear, mask set/clear, HW state |
| IPIs | SGIs | two per-CPU IPIs ("self" and "other"), plus on AIC2/M-series "fast IPI" system registers `IMP_APL_IPI_*` (Linux `irq-apple-aic.c`); Linux funnels all kernel IPIs into one |
| Timer | PPI through the same controller | the ARM timers are not routed through AIC IRQs: "wired straight to FIQ" (Asahi AIC page); FIQ numbers in Linux's binding: 0 physical HV timer, 1 virtual HV timer, 2 physical guest timer, 3 virtual guest timer |
| Enable | `GICD_CTLR`, `GICR_WAKER` | `AIC2_CONFIG` (0x0014) bit 0 = enable |
| Documentation | Arm IHI0069 (public; not in the local `~/docs/arm` set, which holds IHI0070) | Asahi docs `asahilinux.org/docs/hw/soc/aic/` (offsets AIC1: info 0x0004, event 0x2004, target 0x3000, SW_GEN_SET 0x4000, MASK_SET 0x4100, MASK_CLR 0x4180, IPI 0x2008/0x200c/0x2024/0x2028); the AIC2 offsets and FIQ routing are in the Linux driver (GPL-2.0-or-later, SPDX line quoted) |

UNKNOWN and needed before writing the driver: the exact AIC2 register map on T8112 beyond what `irq-apple-aic.c` shows (die count, per-die block strides). Experiment: read the info registers (`AIC2_INFO1/3`) through the m1n1 proxy; compare with the driver's arithmetic.

## 6. The gap table

Columns: row; what the device is here (OBSERVED unless tagged); documentation status; reference for UNKNOWN parts with its licence; Aienux today; what is missing; host-only now? **Doc status** key: **PUBLIC** = vendor or standards document exists; **REVENG** = no vendor document, documented by Asahi's reverse engineering (reference given); **SOURCE** = only Asahi's driver source (or its notes marked unfinished); **DOCS SILENT** is the exact phrase the manual-guard hook records when no public document exists.

Reference licences (OBSERVED, SPDX lines read from the files on 2026-10-20): `rtkit.c`, `sart.c`, `mailbox.c`, `macsmc.c`, `pmgr-pwrstate.c`, `apple_wdt.c`, `clk-apple-nco.c`, `hci_bcm4377.c`, Asahi's `dcp.c`, `iomfb.c`, `dockchannel.c`, `drm/asahi/*.rs` and the device trees (`t8112.dtsi`, `t8112-j413.dts`: "GPL-2.0+ OR MIT") are **GPL-2.0-only OR MIT**. `irq-apple-aic.c`: GPL-2.0-or-later; `apple.c` (NVMe): GPL-2.0; `apple-dart.c`: GPL-2.0-only; `pcie-apple.c`, `spmi-apple-controller.c`, `samsung_tty.c`, `tipd/core.c`: GPL-2.0; `pinctrl-apple-gpio.c`: GPL-2.0-only; `sound/soc/apple/mca.c`, `cs42l84.c`: GPL-2.0-only; `phy/apple/atc.c`: GPL-2.0 OR BSD-2-Clause; `brcmfmac/pcie.c`: ISC. Rule from the brief: **read, then write ours; never copy GPL code into the kernel.** The MIT option on the dual-licensed files is noted, not relied on: our rule is still to write our own driver from the register facts, and to keep an attribution line naming the file read.

### Class D: rows that need drivers or kernel services

| # | Device and what it does | Doc status and reference | Aienux today | Missing | Host-only now? |
| --- | --- | --- | --- | --- | --- |
| D1 | **Boot handoff** (m1n1 to kernel, device tree) | PUBLIC: Linux arm64 boot protocol (kernel.org `Documentation/arch/arm64/booting.rst`); REVENG: Asahi m1n1 guide and boot guide (GPL-2.0-or-later, read-only). | UEFI app `efi_main.c`, handoff `native/boot/handoff.h` | Raw `Image`-format entry (or U-Boot UEFI path), flattened-device-tree parser, boot-args from DT; EL2 to EL1 verified on M2. | Yes (QEMU with a synthetic DT; compare against the real `t8112-j413.dts`) |
| D2 | **CPU bring-up**: 8 cores, two clusters, spin-table | REVENG: Asahi SMP spinup `asahilinux.org/docs/hw/cpu/smp/`; `t8112.dtsi` `enable-method = "spin-table"` | PSCI bring-up (`core/smp.c`, gate `SMP`, NOT_RUN on the Spark) | Spin-table core start; cluster/IMPDEF registers if needed (`acc-impl-reg`, `cpu-impl-reg`, `reg-private` are in the ADT, OBSERVED, meaning UNKNOWN) | Partly (synthetic) |
| D3 | **Interrupts: AIC2 + FIQ timer + fast IPI** | REVENG: Asahi AIC page; Linux `irq-apple-aic.c` (GPL-2.0-or-later). Section 5. | GICv3 and GICv2 only | AIC2 driver, FIQ vector path, IPI path, FIQ-to-timer mapping. Host model of the AIC register block is feasible. | Yes (a chip model plus tests) |
| D4 | **Disk: ANS2 NVMe** behind the ANS helper (SSD AP0512Z) | REVENG. Linux `drivers/nvme/host/apple.c` (GPL-2.0, read for facts): one admin and one I/O queue sharing one tag space (max 0x40 tags, depth 64 on T8103-family; the ADT says 64 here, OBSERVED), linear submission queue enabled by `APPLE_ANS_LINEAR_SQ_CTRL` 0x24908, doorbells `0x2490c`/`0x24910`, NVMMU TCB array at 0x28100 to 0x28120 (128-byte TCB per tag), boot status 0xde71ce55 at 0x1300; the helper firmware is started through RTKit and its shared buffers are allowed through SART. Commands cannot use arbitrary PRPs for vendor opcode 0xD8. | `disk_nvme.c` standard NVMe | RTKit client (D5), SART (D6), ANS2 queue, NVMMU TCBs; 16 KiB page effects; the ADT has `sart-ans` and no `dart-ans` node (OBSERVED, 0 matches), so the ANS is confined by SART only | Partly: needs an ANS2 chip model first (no QEMU model exists: UNKNOWN until org-check/QEMU search) |
| D5 | **Helper processors and RTKit** (ANS, SMC, DCP, GFX, MTP, AOP, SIO, PMP, SEP, ACIO): every device above is behind an Apple "ASC" helper that runs closed firmware and is spoken to through a mailbox and a message protocol (RTKit v2 here: ADT `iop-nub,rtbuddy-v2`, OBSERVED) | REVENG: Asahi ASC page `asahilinux.org/docs/hw/soc/asc/`; Linux `rtkit.c`, `mailbox.c`, `sart.c` (GPL-2.0-only OR MIT) | None | The RTKit client: mailbox, endpoint management, syslog/crashlog endpoints, shared-memory map. This one piece unlocks D4, D8, D9, D12, G1. | Yes (a mailbox model plus endpoint tests; the firmware blobs are Apple's and are loaded by iBoot, not by us: see row F4) |
| D6 | **DART IOMMU** and **SART** (ANS address filter) | REVENG: Linux `apple-dart.c` (GPL-2.0-only): T8110 map TCR 0x1000, TTBR 0x1400, TLB command 0x80, error 0x100, params at 0x00/0x04/0x08, `PARAMS4` stream count, protect/lock bit at 0x200; 16 KiB pages, 4-level tables when the input range exceeds 36 bits; `sart.c` for SART | `smmu.c` (SMMUv3) | DART2 service: attach, map, unmap, invalidate, fault report. Strongly security-relevant: DMA confinement is a doctrine requirement (`docs/GB10_DMA_ROUTE_INVENTORY.md` pattern) | Yes (model plus tests) |
| D7 | **Wired network** (none built in); USB Ethernet is the first | AX88179B (observed on this Mac) is a USB device; Realtek/ASIX register docs are vendor-only: DOCS SILENT on the AX88179B in this repo today. | iMac BCM57766 report only | USB host first (D10), then a CDC-ECM or vendor class driver | After D10 |
| D8 | **Keyboard and trackpad**: MTP helper + dockchannel + HID | SOURCE: Asahi tree `dockchannel.c` and `dockchannel-hid.c` (GPL-2.0 OR MIT); feature table `asahilinux.org/docs/platform/feature-support/m2/`: keyboard and trackpad are "linux-asahi" (out of tree). No Asahi doc page. | `dev/usb_kbd.c` (USB HID) | MTP RTKit endpoint, dockchannel FIFO, HID report parsing, trackpad multitouch format | Partly |
| D9 | **System controller (SMC)**: reset, power off, battery, thermal, lid, RTC | REVENG: Asahi SMC page `/hw/soc/smc/`; Linux `macsmc.c` (GPL-2.0-only OR MIT), `rtc-macsmc.c` | PSCI reset/off (QEMU) | SMC protocol over RTKit: key read/write, reboot/off, RTC (this is also the wall clock) | Yes (model) |
| D10 | **USB-C / USB4 ports**: PD controller CD3217 (I2C), ATC PHY, DWC3-based USB controller, DART, ACIO helper | REVENG: Asahi USB PD and ATC notes (`/hw/soc/usb-pd/`); Linux `tipd/core.c` (GPL-2.0, `apple,cd321x`), `phy/apple/atc.c` (GPL-2.0 OR BSD-2-Clause), DWC3 glue in `t8112.dtsi` (`apple,t8112-dwc3` at 0x3_8228_0000). Standard xHCI-like register set: PUBLIC (xHCI spec 1.2) once the DWC3 core is in host mode. | `dev/xhci_fence.c`, `usb_kbd.c`, `usb_hid.c`, platform walker (QEMU PASS) | PD policy, PHY lane mux, DART setup, DWC3 host mode. The two ports are the machine's only wired I/O: keyboard, network and the m1n1 debug link all depend on them. | Partly |
| D11 | **Wi-Fi: Broadcom BCM4387 over PCIe** | REVENG: Linux `brcmfmac/pcie.c` (ISC licence, permissive); Asahi feature table: Wi-Fi upstream from 6.1. DOCS SILENT on the Broadcom register interface (no public datasheet). Needs Broadcom firmware, NVRAM text and calibration (CLM, txcap, board files) extracted from macOS; these are proprietary and not redistributable. | None | APCIe controller (D12), the BCM4387 PCIe protocol, a WPA2/WPA3 supplicant (not available natively), regulatory data. See row F4. | Chip model: no |
| D12 | **PCIe (APCIe)** for Wi-Fi and Bluetooth (and the Mac mini-style ports on other machines) | REVENG: Asahi APCIe page `/hw/soc/apcie/`; Linux `pcie-apple.c` (GPL-2.0): not standard ECAM only; per-port setup, MSI via the AIC-adjacent block, DART per port | None (standard ECAM and Spark PCI walker only) | APCIe port bring-up, tunables from the ADT, MSI | Partly |
| D13 | **Bluetooth: BCM4387 over PCIe** | SOURCE: Linux `hci_bcm4377.c` (GPL-2.0-only OR MIT) | None | After D12; firmware from macOS (F4) | No |
| D14 | **Display, simple path**: the iBoot framebuffer | PUBLIC for pixel formats; REVENG for finding it (Asahi ADT page `/fw/adt/`) | UEFI GOP console | Read `/chosen/framebuffer` (device-tree property), map it with the right attributes; no driver, no firmware | Yes (synthetic) |
| D15 | **Display, real path**: DCP helper, mode setting, brightness, external monitors, the Parade DP855 timing controller | SOURCE: Asahi `drivers/gpu/drm/apple/dcp.c`, `iomfb.c` (GPL-2.0-only OR MIT): firmware-version-specific protocol (IOMFB) with Apple's closed DCP firmware. Asahi feature table: main display "linux-asahi". | None | A full RTKit client plus the version-specific IOMFB command set. Not needed for first boots. | No |
| D16 | **Audio**: MCA clusters, `sn012776` speaker amplifiers (I2C), `cs42l84` headphone codec, AOP microphone | REVENG: Linux `sound/soc/apple/mca.c` (GPL-2.0-only), `cs42l84.c` (GPL-2.0-only); amplifier and AOP paths are Asahi-tree only. Public datasheet for the Cirrus part: not checked (DOCS SILENT recorded for this repo). | None | MCA, I2C, amplifiers (safety-critical: speaker over-drive can damage drivers; Asahi's docs describe the protection model, `asahilinux.org/docs/sw/audio-userspace/`), AOP | After D5 |
| D17 | **Power and clocks**: `pmgr`, power domains, clocks, `clpc` performance control, SPMI PMIC, battery | REVENG: Linux `pmgr-pwrstate.c` (GPL-2.0-only OR MIT), `spmi-apple-controller.c` (GPL-2.0), `apple_wdt.c`; Asahi WDT page | None | Power domains must be on before any device works (the Linux DT lists `power-domains` for ANS, USB, ...). Frequency control (`clpc`) is a separate, larger unknown. | Partly (model) |
| D18 | **Serial and debug console**: Samsung-style UART at 0x2_3520_0000; reachable only through m1n1's USB proxy or a special Type-C debug cable | REVENG: Asahi USB-debug page `/hw/soc/usb-debug/`; Linux `samsung_tty.c` (GPL-2.0). | `ck_console_set_uart` (SPCR) | UART driver (simple), but physical access is the cable. The m1n1 hypervisor's virtual UART is the practical console (Source D). | Yes |
| D19 | **Clock and RTC, watchdog, entropy** | RTC through SMC (D9); watchdog `wdt@3D2B0000` (Asahi WDT); entropy see F7 | `arch/timer.c` QEMU | SMC RTC read, WDT feed, entropy source | Partly |
| D20 | **Touch ID (mesa on SPI2), camera ISP, Neural Engine, video codecs, NFC, JPEG** | Asahi feature table: Touch ID TBA, camera linux-asahi, Neural Engine "out of tree" | None | Not needed for the project goal; each is a separate helper-processor protocol. Named so the list is complete. | No |

### Class G: the GPU (AGX G14G)

| # | Row | Doc status | Today | Missing |
| --- | --- | --- | --- | --- |
| G1 | **Discovery and helper boot**: `sgx@4000000` (MMIO 0x2_0400_0000, 64 MiB) and `gfx-asc@6400000`: the GPU's own ASC helper boots from iBoot; the OS talks to it through a mailbox | REVENG, partly. Asahi AGX page `asahilinux.org/docs/hw/soc/agx/`; Asahi `drivers/gpu/drm/asahi/*.rs` (GPL-2.0-only OR MIT) | None | RTKit client (D5), GPU DART, the mailbox endpoints |
| G2 | **GPU MMU ("UAT")**: "essentially the ARM64 MMU, and uses identical page tables", 16 KiB pages, 40-bit GPU virtual addresses | REVENG: AGX page (the quoted lines) | None. The Spark GPU MMU simulator (L193, `aienos-gb10-gpu-mmu-sim`) is a pattern, not shared code | UAT context tables, TLB invalidation, kernel/shared/user VA split as in Asahi's `mmu.rs` |
| G3 | **Firmware structures and channels**: init data blob, device/pipe channels, work queues, microsequences | SOURCE, marked by Asahi as "(unfinished)", with TODOs and "Reader beware, here be dragons." Firmware is Apple's and treated as fixed: "Talking to Apple's firmware to make this GPU work is considered non-negotiable." | None | Everything; this is the largest unknown on the machine |
| G4 | **Compute dispatch** (the first useful thing for a model) | **DOCS SILENT on a compute-only path.** The AGX page covers the TA (vertex/tiler) and 3D channels in detail; a compute channel (`CP`) is named but undocumented in what was read (INFERRED, medium; the page text was summarised, not read end to end). No public Apple document describes the instruction set or the command stream. | None | The shader instruction set (Asahi's userspace compiler work; not read), the compute command format |

Honest limit (OBSERVED and INFERRED): there are no public Apple GPU documents. Asahi's AGX notes are unfinished and their driver is the only reference for the firmware interface; the instruction set is documented only by Asahi's userspace reverse engineering. Nothing here says the chip cannot compute; it says nobody outside Apple has published how, and the only way to learn is by reading Asahi's work and running experiments on the chip.

**What a first compute on AGX would need** (INFERRED; order of dependencies):

1. A kernel that owns the GPU's MMIO, its DART (D6) and its helper (RTKit, D5), with the helper's firmware already running (iBoot starts it; OBSERVED: `gfx-asc` is a node and the OS driver in macOS talks to it).
2. The GPU MMU (UAT, G2): create a context, map a command buffer and a data buffer, invalidate.
3. The firmware init message and the shared structures (G3) copied from Asahi's `initdata.rs` facts: the exact layouts are firmware-version-specific. Corrected by L372 (2026-10-21): the published Asahi GPU driver already carries the layouts for the macOS 12.3 and 13.5 firmware interfaces (`V12_3` ... `V13_5`, `drivers/gpu/drm/asahi`), and the Asahi container on this M2 runs the 13.5 boot firmware (section 4.3), so the first step is **host-only**: read those 13.5 layouts. A **record, do not send** trace is a second, optional step, and it cannot run against this Mac's own macOS 26.6.2: the m1n1 hypervisor supports only pinned guests, "macOS 13.5 (M1 and M2 series)" and "macOS 14.8.3 (M1-M3 series)", installed as a second copy of macOS on its own volume (Asahi `docs/sw/m1n1-hypervisor.md`). Newer guests need SPTM, whose hypervisor support exists only on m1n1's main branch after v2.0.0 (commits 2026-10-11 to 10-18, untagged).
4. A work queue and one compute job: the smallest useful one is an integer vector add. The shader binary needs the AGX instruction encoding (Asahi userspace work).
5. A completion path: event stamps and an interrupt through AIC (D3).
6. Teardown and a second run, to prove the state is clean.

This is also where the Spark's L193 (GB10 page tables and channels on a simulated device) and Omega's launch layer (cut A) matter: the **abstractions** (channel, allocator, context, completion) can be shared even though no register is.

### Class F: firmware, policy and platform

| # | Row | Detail |
| --- | --- | --- |
| F1 | Security boundary | The SEP stays. Our M5 keys do not use the SEP in v1. UNKNOWN whether loading a custom boot object blocks features we want (Apple: "some decryption keys become unavailable", which concerns macOS's FileVault keys). |
| F2 | Secure boot of our own chain | Stage 1 is signed by the Mac (Source E). Our kernel is not signed by Apple; our own signed slots (`docs/NATIVE_IMAGE_SLOTS.md`, owner root, ADR 0037) can verify the kernel, but a chain from stage 2 to the kernel is not designed yet. m1n1 documents a future "signed stage 2" (Open OS Interop page). |
| F3 | Anti-rollback | No TPM here. Candidates: the SEP, or an owner-held counter on the network. UNKNOWN; the Spark TPM work (L200) does not carry over. |
| F4 | **Firmware blobs from macOS** | Wi-Fi, Bluetooth, the display controller, the GPU and the Neural Engine run Apple or Broadcom firmware that lives in macOS's own volumes. Asahi extracts it at install time. Using it is a **firmware load**, which the hard limits for this task forbid; it is also an Apple/Broadcom licence question: redistribution is not permitted, extraction from the owner's own machine is how Asahi handles it. Drake's decision (section 10). |
| F5 | Updates | macOS updates can change the system firmware and invalidate Asahi's version-specific data (Source E: "version interdependencies"). Rule for the operator: no macOS updates between map and first boot. |
| F6 | Battery and thermals | A laptop. Charging, cell protection and thermal limits live in the SMC and PMIC. Under tethered boot macOS is not running; the SMC keeps its own safety limits (INFERRED, not verified). Run on AC power, with the charger attached (OBSERVED: 67 W adapter connected). |
| F7 | Entropy | See section 5 (RNDR absent, INFERRED). Experiment: read `ID_AA64ISAR0_EL1` through m1n1 proxy; if RNDR is absent, choose between the SEP's random service (protocol UNKNOWN) and a seed from the Spark over the tether for the first boots only (explicitly labeled **test only**; never for keys). |
| F8 | Userland | Same as the Spark map section 6: TCP, TLS, ssh and agents do not run natively; they run on other machines. |

## 7. What has to be reverse engineered, and what is already reverse engineered

Already reverse engineered by Asahi and **published with open-source drivers** (read-only reference for us): AIC, DART, SART, RTKit/mailbox, ANS2 NVMe, SMC, PMGR, PCIe, USB-C PD and ATC, Wi-Fi/Bluetooth over PCIe, DCP display protocol, audio, watchdog, keyboard/trackpad over MTP.

Not published as a document, only as Asahi's driver source or notes marked unfinished (UNKNOWN rows with their experiments):

| UNKNOWN | Resolving experiment (all read-only, host side) |
| --- | --- |
| ~~Is `macOS 26.6.2` / firmware `18000.161.10` supported by the current m1n1~~ RESOLVED (L372): yes, m1n1 `src/firmware.c` lists `mBoot-18000.161.10` (since `3e354a2`, in v1.9.9 and v2.0.0); the container uses its own 13.5 boot firmware | Done, section 4.3 |
| M2 EL1 4 KiB granule support | Read `ID_AA64MMFR0_EL1` through m1n1 proxy |
| RNDR present | Read `ID_AA64ISAR0_EL1` through m1n1 proxy |
| AIC2 info registers (die count, strides) | Read through m1n1 proxy; compare with `irq-apple-aic.c` |
| EL at m1n1 handoff and what state is left (caches, MMU, SMP parked) | m1n1 `proxyclient` `shell` prints `CurrentEL`, `SCTLR_EL1` before any kernel starts |
| `random-seed` value (zeros in the dump, INFERRED redacted by the collection script) | Re-collect without redaction, or read `/chosen` from m1n1 |
| Internal panel framebuffer address and stride | m1n1 proxy: read `/chosen/framebuffer` from the ADT m1n1 hands over |
| ANS firmware tags and TCB layout on T8112 vs T8103 | Compare `apple.c` constants with `ans@77400000` reg ranges (OBSERVED: five MMIO ranges) |
| GPU compute channel and instruction set | Corrected by L372: first read the 13.5 layouts in the Asahi GPU driver (host only). Trace only as a second step, with m1n1's hypervisor running a **second macOS install pinned at 13.5** (the hypervisor doc's procedure: a new APFS volume, macOS 13.5, its kernelcache under `run_guest.py`) on a spare M1/M2 Mac, Drake attended; never Drake's 26.6.2 install. Plus Asahi's compiler notes |
| MTP protocol details | Read Asahi's `dockchannel-hid` driver first (host only); if a trace is still needed, the same pinned-13.5 hypervisor session (m1n1 ships `proxyclient/hv/trace_keyboard.py`) |
| Safe sound output | Asahi's speaker protection model; **no sound** until read in full |

## 8. Where it fits in the fleet (Drake, 2026-10-20: "across the network: iMac, Raspberry Pi, MacBook ... the Spark")

| Machine | Processor / interrupts | Boots through | Network today | Map |
| --- | --- | --- | --- | --- |
| Spark (DGX GB10) | aarch64, GICv3 | UEFI + ACPI | Realtek 10GbE (no driver yet) | `docs/SPARK_REPLACE_LINUX.md` |
| iMac 2017 | x86-64, APIC | UEFI | BCM57766 (transmit-only report) | in flight |
| Raspberry Pi 5 | aarch64, GICv2 | UEFI + ACPI/DT | GEM | in flight |
| MacBook Air M2 | aarch64, **Apple AIC2** | m1n1 (tethered) or m1n1 + U-Boot (UEFI) | none native (USB Ethernet first) | this document |
| Ballentine PC | x86-64 (Ryzen 5 7600X) + NVIDIA RTX 5070 | UEFI | in flight | in flight (L209) |

Shared with the Spark map and the others: the Store, M5 crypto, signed slots, recovery, the network report pattern (a UDP report instead of a photograph), the capability model and the platform-services layer that Omega cut A (L202) defines. Not shared: the interrupt controller (three aarch64 variants now), the IOMMU (SMMUv3 versus DART), the disk path (standard NVMe versus ANS2), and the boot handoff (ACPI versus device tree).

Nothing here changes the Spark's order. The MacBook is not a replacement for any Spark function: the 27B model needs the GB10. What it adds is a second aarch64 machine where Aienux can run, and a test of whether Aienux's abstractions are really portable (Milestones Phase 7: "generic ARM64, AMD accelerators, and Apple silicon").

## 9. Shortest ordered path

Notation as in the Spark map: **[H]** host or QEMU only, a builder can do it now; **[T]** needs an attended tethered boot of the MacBook; **[C]** needs the real chip beyond a simple read; **[O]** Drake only.

### To a first attended boot (tethered; macOS stays; nothing stored on the Mac by AienOS)

1. [H] Settle the open reading questions that need no Mac: supported-version check (section 4.3), Asahi m1n1 build on the Spark from a pinned commit (GPL tool run as a separate program; nothing copied), and a pinned copy of `t8112-j413.dts` (GPL-2.0+ OR MIT) as a test input only.
2. [H] A device-tree parser (flattened device tree, `core/dtb.c`-style, host-tested against `t8112.dtsi` compiled with `dtc`) and a boot-info reader for the ADT-derived tree m1n1 hands over. Needed by every later row.
3. [H] Raw-`Image` entry for the C kernel (Linux arm64 boot protocol header), QEMU `-M virt` booted from it with a DT instead of ACPI; EL2 to EL1 retained.
4. [H] AIC2 chip model and driver; FIQ vector; timer through FIQ; fast-IPI path. Tests on the model; the register map is checked against the live read in step 8.
5. [H] RTKit client, SART and DART service on mailbox/DART models (D5, D6), because D4, D9, D10 all stand on them.
6. [O] Decide whether the MacBook is the machine for this (section 10) and, if yes, perform the one-time recovery setup (installing m1n1 stage 1 as a new Permissive-Security OS container).
7. [T] **Boot #1: proxy only, read-only.** Spark connects over USB-C, enters m1n1 proxy mode, reads `CurrentEL`, `ID_AA64MMFR0_EL1`, `ID_AA64ISAR0_EL1`, AIC info registers and the ADT (this resolves the UNKNOWN rows in section 7 with a pass/fail per row). Nothing is executed on the chip beyond m1n1's own reads.
8. [T] **Boot #2:** load Aienux into RAM through the tether; first line on the m1n1 virtual UART: `kernel: alive` plus the device list. Pass: line seen; power cycle returns to macOS unchanged.
9. [T] **Boot #3:** framebuffer text (D14), keyboard through MTP (D8) only if the MTP protocol is read in full; otherwise the tether console is the input.
10. [T] **Boot #4:** disk read-only through ANS2 (D4): list the partition table, read, never write. The macOS APFS container is never mounted or written; Aienux reads only its own test region or a USB disk instead (a Drake decision: preferred is a separate USB SSD).

### Toward a useful system

11. [T] Network through USB Ethernet (D10 host mode, D7), then a UDP report as on the iMac.
12. [H][T] Store on a USB disk, then crash tests on it; internal-disk writes only after Drake says so.
13. [H] Wi-Fi/Bluetooth path (D11, D12, D13) only after the firmware-blob decision (F4).
14. [H][C] GPU: read the 13.5 firmware layouts from the Asahi GPU driver first (host only); an optional record-only trace needs a second macOS install pinned at 13.5 under the m1n1 hypervisor (not this Mac's 26.6.2, which is not a supported guest; L372 correction, see G3 step 3); then the UAT and the firmware init layouts, then one integer vector add (section 6, G1 to G4). Reports measured speed against the chip's published memory bandwidth, which is **not measured here**.
15. [O] A standalone boot (Path U) and any permanent change, only after a soak of tethered boots.

## 10. Operator steps (Drake only)

None of these is authorized by this document. Nothing here has been done.

- **Decide which MacBook.** This one is the owner-key ceremony machine with Activation Lock on and 752 battery cycles (OBSERVED); a spare Apple-silicon Mac would avoid any risk to it. (Section 4.3.)
- **The one-time recovery setup, changes the Mac:** from recovery mode (power button held on a cold boot, owner password), create a new boot container for the experiment and install m1n1 stage 1 into it with Apple's `kmutil configure-boot`, setting that container to Permissive Security. This adds a new OS entry to the boot picker and uses disk space (40.9 GB free is available); it does not alter the macOS container (Source B). It can be undone from recovery by deleting the container and re-enabling full security (Asahi's uninstall guide, not yet read here).
- A **USB-C cable** from the Spark to a Thunderbolt/USB4 port of the MacBook, the charger on the other port, and power on.
- Choose whether to allow **extracting firmware** from macOS for Wi-Fi/Bluetooth/GPU/display later (F4).
- Say whether a **separate USB SSD** may be used for Store tests (step 10, 12).
- Photograph or capture screens at each attended boot, or read the UDP report once the network exists.
- No macOS updates between now and the first boot.

## 11. What is tracked where

| Item | Owner / place |
| --- | --- |
| This map | L208, `docs/MACBOOK_AIENOS_MAP.md` on `feat/aienos-macbook-map` |
| Spark map | L191, `docs/SPARK_REPLACE_LINUX.md` |
| Platform-services layer (shared) | L202 (Omega cut A) |
| GPU channel/MMU abstractions on a simulator | L193 (`feat/gb10-gpu-mmu-sim`) |
| Key ceremony machine | `docs/OWNER_KEY_CEREMONY.md` |
| Whole-system order | `aien-spark/aien-architecture/CURRENT_EXECUTION_PLAN.md` |

## 12. Limits of this document, and what was searched

- The ADT is a snapshot of one afternoon under macOS 26.6.2. Property meanings marked UNKNOWN were not decoded.
- Asahi pages were read through a page summariser, not downloaded end to end; quotes are as returned and should be re-read at the source before a driver is written. The Asahi page for APCIe, SMC, ASC, WDT, SMP, ADT, memory map and USB PD was **named but not read**; those rows rest on the Linux driver headers and the feature table.
- No chip register code was written, so the manual-guard `docs-read` was not required for this work. Before any driver, record `docs-read macbook "<source>" "<quoted line | DOCS SILENT on X>"` per the repo hook.
- No test was run; this document changes no code. Quoted gate names come from the repo's own docs at `18e2765`.
- Reuse search: `git fetch`; origin/main and all 525 remote branches (`git branch -r | grep -ciE 'asahi|m1n1|apple|macbook'` = 0); `git grep` on origin/main for `asahi`, `m1n1`, `t8112`, `ANS2` (0 hits outside vendored text; `apple silicon` appears in `docs/MILESTONES.md` Phase 7 only); sibling worktrees `~/workspace/aienos-*` (the iMac/Pi/PC/program map worktrees have no map document yet; no Apple-silicon work found); `~/handoffs` (this task's own files only); the ledger (L208 only); the mind map (the owner-key-on-MacBook decision note). `org-check` 2026-10-20: "asahi" 16 hits in 98 repos (all unrelated: ARM Trusted Firmware SynQuacer commits, edk2 platform commits, `t3code` test fixtures), "m1n1" 2 hits (unrelated text), "t8112" 1 hit (a toolchain log under `omega-mv` evidence), "apple silicon" 148 hits (build and benchmark notes about running tools on the Mac, not OS work). Conclusion: nothing in the organisation implements any Apple-silicon boot or driver; what Asahi has built is the existing work to read.
