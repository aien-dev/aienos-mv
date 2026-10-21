# AienOS on the Apple Silicon family (M1 to M6): what changes per chip (L372)

Status: documentation and source reading only. Nothing here booted, flashed or changed any Mac. Every claim is tagged **OBSERVED** (read by us: a source file at a pinned commit, or the M2 dumps), **SOURCED** (a public page, with URL and a short quote, read 2026-10-21), or **INFERRED** (reasoning given). An unknown is an **UNKNOWN** row with the read-only experiment that settles it.

Names: AienOS is the whole system, Aienux is the kernel. This map extends `docs/MACBOOK_AIENOS_MAP.md` (the M2 gap map, L208). It is the fact base for the platform layer that L250 is building (`feat/apple-silicon-platform`).

## 0. Plain-English summary

Apple has made six generations of Mac chips (M1 to M6), each in a small size and, from M1 on, bigger Pro, Max and Ultra sizes. Most of the parts AienOS needs work the same way across the family, with a different "model number" in the chip's own description of itself. A few parts changed in ways that matter:

1. **The interrupt controller** (the part that tells the processor "a device needs you") has three versions: version 1 on the M1, version 2 on the M1 Pro/Max/Ultra and the whole M2 family, version 3 from the M3 on.
2. **Sleeping cores lose their memory from the M4 on.** On M1 to M3 the boot loader can tell an idle core to keep its state while it naps. From the M4, Apple's own boot firmware locks that switch, so a napping core forgets everything and has to restart from scratch. Aienux must not let an M4 or newer core nap the ordinary way until it has a "save, nap, restore" path.
3. **The USB-C port chip changed** from a TI CD3217 on a two-wire bus (I2C) to a newer chip ("ACE3") on Apple's power-management bus (SPMI), starting with the M3 Pro/Max and the M3 MacBook Air.
4. **The disk controller changed on the M4**: new register window, new set-up steps, and a firmware change in macOS 15.
5. **Booting another OS still works on M4 and M5** in the same way as on the M2 (Asahi Linux runs there, with the disk working), but Apple's macOS kernel now needs a new security monitor (SPTM) that makes the Asahi "watch macOS drive the hardware" tool much harder to use on new chips.

So the platform layer can be written once: a table of chip facts keyed by the name the chip reports, plus a small number of driver variants (three interrupt-controller versions, two disk-controller versions, two USB-C port chips). A new Mac becomes a table row and, at most, a driver variant.

## 1. The family table (one row per chip)

Key to the columns, all from the sources pinned in section 8:

- **SoC / DT root**: chip id (m1n1 `src/soc.h`) and the root `compatible` of the Linux-style device tree that m1n1 hands the kernel (Asahi Linux `arch/arm64/boot/dts/apple/*.dtsi`). The Apple Device Tree (ADT) names the same chip `platform-name tNNNN` (OBSERVED on the M2).
- **MIDR E/P**: CPU part numbers, efficiency/performance core (M5 Pro/Max name their cores "M" and "P"; m1n1 `src/midr.h`).
- **AIC**: interrupt controller version and the Linux-DT compatible m1n1 passes on.
- **DART**: DMA guard variant (register layout family) and page size.
- **ANS**: disk controller.
- **USB-C PD**: port controller chip and bus.
- **ATCPHY**: USB-C/Thunderbolt physical layer compatible.
- **GPU / ABI**: GPU generation and the macOS version whose firmware interface Asahi targets.
- **WFI**: whether a napping core keeps its state.
- **Start**: secondary-core start. Every row: device tree `enable-method = "spin-table"` written by m1n1, which itself starts cores through the per-core reset vector (RVBAR) and a PMGR "CPU start" register at the listed offset (m1n1 `src/smp.c`). No chip has PSCI firmware (no EL3).
- **SPTM**: whether Apple's security monitor is in macOS's boot chain, by macOS version. Third-party claims are marked.
- **m1n1**: first m1n1 release tag that names the chip id (OBSERVED, `git tag --contains` on the commit adding the `#define`; "names the id" is not "fully supported").
- **Asahi today**: from the Asahi feature-support pages (docs repo `0d1f891`, 2026-10-05).

| SoC | SoC id / DT root | MIDR E/P | AIC | DART (page) | ANS | USB-C PD | ATCPHY | GPU / ABI | WFI state | Start offset | SPTM in macOS chain | m1n1 first tag | Asahi today | Example Macs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| M1 | T8103 / `apple,t8103` | 0x22 / 0x23 | **AIC1** (`apple,t8103-aic`, `apple,aic`) | `t8103-dart` (T8020 regs); 16 KiB INFERRED | ANS2 `t8103-nvme-ans2` | CD3217 on I2C (`apple,cd321x`) | `t8103-atcphy` | G13G / macOS 12.3 and 13.5 | kept (m1n1 sets retention) | 0x54000 | never (third party) | v1.0 | supported, installer yes | MacBook Air M1, MacBook Pro 13" M1, Mac mini M1, iMac 24" M1 |
| M1 Pro / Max / Ultra | T6000 / T6001 / T6002, `apple,t600N` | 0x24/0x25 (Pro), 0x28/0x29 (Max); Ultra: no own entry, INFERRED Max parts | **AIC2** (`apple,t6000-aic`, `apple,aic2`), 2 dies on Ultra | `t6000-dart` (+ `t8110-dart` on some blocks) | ANS2 `t6000-nvme-ans2` | CD3217 I2C | `t6000-atcphy` | G13S / G13C / G13D, 13.5 | kept | 0x54000 | never (third party, "M1 base" only stated) | v1.0 (Ultra v1.0.3) | supported | MacBook Pro 14"/16" 2021, Mac Studio 2022 |
| **M2** (Drake's Mac) | T8112 / `apple,t8112` | 0x32 / 0x33 | **AIC2** (`apple,t8112-aic`; ADT `aic,2` OBSERVED) | `t8110-dart`, 16 KiB (ADT `page-size` 0x4000, OBSERVED) | ANS2 `t8112-nvme-ans2` | CD3217 I2C (ADT `usbc,cd3217`, OBSERVED) | `t8112-atcphy` | G14G / 13.5 | kept | 0x34000 | by macOS 15.0 (third party) | v1.1 | supported, installer yes | MacBook Air M2 (Mac14,2), MacBook Pro 13" M2, Mac mini M2 |
| M2 Pro / Max / Ultra | T6020 / T6021 / T6022, `apple,t602N` | 0x34/0x35, 0x38/0x39; Ultra INFERRED Max parts | AIC2 (`apple,t6020-aic`) | `t6020-dart` → `t8110-dart` | ANS2 `t6020-nvme-ans2` | CD3217 I2C | `t6020-atcphy` | G14S / G14C / G14D, 13.5 | kept | 0x28000 | INFERRED as M2 (not stated) | v1.2.7 (Ultra v1.3.3) | supported | MacBook Pro 14"/16" 2023, Mac mini M2 Pro, Mac Studio/Pro M2 Ultra |
| M3 | T8122 / `apple,t8122` | 0x42 / 0x43 | **AIC3** (`apple,t8122-aic3`) | `t8122-dart` → `t8110-dart` | ANS2 `t8122-nvme-ans2` | **split**: CD3217 I2C on iMac and MacBook Pro 14"; **ACE3 on SPMI** (`apple,sn201202x`) on MacBook Air 13"/15" (OBSERVED, `t8122-j613/j615.dts` include `t8122-usbpd-spmi.dtsi`) | `t8122-atcphy`, new tunables for TSMC N3 | next generation, GPU WIP / **14.8.3** | kept; adds a 1 GHz counter redirect (m1n1 `counter_redirect`) | 0x34000 | at macOS 26.0 only (third party) | v1.4.7 | installer yes; display (boot framebuffer only), GPU WIP | iMac 2023, MacBook Pro 14" M3, MacBook Air M3 |
| M3 Pro / Max / Ultra | T6030 / T6031, T6034 / T6032 | 0x44/0x45 (Pro), 0x48/0x49 (Max); T6034/T6032 INFERRED Max parts | AIC3 (`apple,t603N-aic3` → `t8122-aic3`) | `t603N-dart` → `t8110-dart` | Pro: **ANS3** `t6030-nvme-ans3` (falls back to `t8103-nvme-ans2`); Max: ANS2 | ACE3 on SPMI (`t603x-j514-j516.dtsi`) | `t603N-atcphy` → `t8122-atcphy` | GPU TBA / 14.8.3 | kept | Pro 0x34000; Max/Ultra 0x88000 | as M3 (third party) | v1.4.7 (Pro v1.5.0, Ultra v1.9.9) | MacBook Pro: installer yes; Mac Studio 2025 M3 Ultra: no | MacBook Pro 14"/16" late 2023, Mac Studio 2025 |
| M4 | T8132 / `apple,t8132` | 0x52 / 0x53 | AIC3 (`apple,t8132-aic3` → `t8122-aic3`) | UNKNOWN variant (no DART node in DT 7.4; feature page "TBA"); page UNKNOWN | **new**: `nvme-secure-bar`, register window 9, extra I/O-queue set-up, controller firmware change in the macOS 15.x bundle; SARTv4 | UNKNOWN (USB-PD "TBA"); INFERRED ACE3 | m1n1 reuses the T8122 tunables (`atc-phy,t8132`) | TBA / no ABI pinned | **lost** (mBoot locks the chicken bits) | 0x34000 | from launch (Asahi: "SPTM is mandatory for XNU on M4 and above") | v1.6.0-rc1 | DT, AICv3, watchdog, I2C, GPIO in linux-asahi 7.4; NVMe WIP; installer no | iMac 2024, Mac mini 2024, MacBook Pro 14" M4, MacBook Air 2025 |
| M4 Pro / Max | T6040 / T6041 | 0x54/0x55, 0x58/0x59 | AIC3 WIP | UNKNOWN | as M4 (INFERRED) | UNKNOWN | UNKNOWN | TBA | lost | 0x88000 | from launch | v1.6.0 / v1.6.1-rc1 | NVMe WIP, rest TBA | MacBook Pro 14"/16" M4 Pro/Max, Mac mini M4 Pro, Mac Studio M4 Max |
| A18 Pro | T8140 / `apple,t8140` | 0x60 / 0x61 | AIC3 (`apple,t8140-aic3`) | UNKNOWN | TBA | UNKNOWN | UNKNOWN | TBA | lost | 0x34000 | INFERRED as M4 | v1.6.0 | DT 7.4, AICv3, watchdog, I2C, GPIO; rest TBA | MacBook Neo (2026) |
| M5 | T8142 | 0x62 / 0x63 | UNKNOWN; INFERRED AIC3 (m1n1 has only the AIC3 path for new chips) | UNKNOWN | as M4 per the progress report ("working NVMe on M4 and M5") | UNKNOWN | UNKNOWN | TBA | lost | 0x34000 (confirmed on hardware, m1n1 `b72e664`) | from launch (third party) | v1.6.1-rc1 | all TBA; installer no | MacBook Pro 14" 2025, MacBook Air 2026 |
| M5 Pro / Max | T6050 / T6051 | 0x64/0x65, 0x68/0x69 ("M" and "P" cores) | UNKNOWN | UNKNOWN | UNKNOWN | UNKNOWN | UNKNOWN | TBA | lost | 0x88000 | INFERRED as M5 | v1.6.1-rc1 | all TBA | Mac mini 2026, Mac Studio 2026, MacBook Pro 14"/16" 2026 |
| M6 | T8152 | 0x72 / 0x73 | UNKNOWN | UNKNOWN | NVMe WIP | UNKNOWN | UNKNOWN | TBA | lost (shares the M4 feature set in m1n1) | 0x34000 | UNKNOWN | v1.9.9 | NVMe and video decoder WIP; rest TBA | Mac mini 2026 |

Notes on the table:

- **DART page size** is not a table fact in Linux: the driver reads it from the DART's own `PARAMS1` register at probe (`dart->pgsize = 1 << FIELD_GET(DART_PARAMS1_PAGE_SHIFT, ...)`, Asahi Linux `drivers/iommu/apple-dart.c:1369`, OBSERVED). On the M2 the ADT says 16 KiB (OBSERVED). For M1 and M3 the 16 KiB value is INFERRED (same driver, Asahi ships a 16 KiB kernel). For M4 and newer it is UNKNOWN until read; experiment in section 7.
- **WFI**: m1n1 sets "WFI mode up, enable WFI retention" through `SYS_IMP_APL_CYC_OVRD` only when `apple_sysregs_unlocked` is true, and that flag is true for the M1, M2 and M3 core types and false for every M4-class entry (M4, M4 Pro/Max, A18 Pro, M5, M5 Pro/Max, M6), with `sleep_mode = SLEEP_NONE` and the comment "XXX figure out what features are actually available on M4" (m1n1 `src/chickens.c`, OBSERVED). On those chips m1n1 appends `idle=nop arm64.nowfxt` to the Linux command line because "M4 and later lose architectural state in WFI and WFIT" (m1n1 `src/kboot.c`, commit `3a69c73`, 2026-09-29; condition fixed in `9095a5e`). Progress report 7.2: "calling WFI causes the core to lose its state and crash whatever was running on it" and "Apple now sets each core's chicken bits in mBoot".
- **M4-class memory hole**: on M4, A18 Pro and M5, "there is a CTRR instance left enabled only on the secondary (non-boot) cores", so the kernel must reserve that range or a secondary core faults when writing it (m1n1 `src/kboot.c`, commits `a735ea2`, `00b8814`, OBSERVED). The range is read at boot from `CTRR_C_LWR_EL1` / `CTRR_C_UPR_EL1`.
- **Asahi docs say M5 Pro and M5 Max are both T6050**; m1n1 defines T6051 for the Max with its own core parts (OBSERVED). The table follows m1n1 (code that runs on the hardware beats the codename page).
- **The ADT is the ground truth on the machine; the Linux-style tree is what Aienux receives.** m1n1 builds the tree it hands over from a device-tree source plus values it copies from the ADT (m1n1 `src/kboot.c`). On the tethered path that source is the Asahi `.dts` for the board, so Aienux must key on the Linux-DT compatibles above, not on ADT names like `aic,2`.

## 5. Platform-layer design rule for L250 (short spec)

Goal: one Apple platform layer for every row of section 1. A newer Mac must cost a table row, and a driver variant only when its registers really changed.

### 5.1 The rule

1. **Discover before you tabulate.** If the hardware or the handed-over tree says it, read it; do not copy it into a table. In order of trust:
   - registers that describe themselves (AIC `INFO` registers for interrupt count and die count; DART `PARAMS1` for page size and `PARAMS` for stream count; `MIDR_EL1` and `MPIDR_EL1` for core type and cluster; `ID_AA64*` for features; `CTRR_C_LWR/UPR_EL1` for the M4-class hole);
   - device-tree properties (`reg`, `interrupts`, `power-domains`, `apple,dma-range`, `enable-method`, `cpu-release-addr`, `/chosen` framebuffer, `/reserved-memory`, ADT-copied values like `sart-version`, `wdt-version`, `nvme-secure-bar`);
   - the per-SoC table, only for what neither of the above gives.
2. **Drivers bind by compatible list, most specific first, with fallbacks**, exactly as the tree lists them (`"apple,t6030-aic3", "apple,t8122-aic3"`). A driver names the base compatible it implements (`apple,t8122-aic3`), so a new chip that lists that base works with no code change.
3. **Unknown means stop, out loud.** If the root compatible is not in the table, or a device's compatible list matches no driver, Aienux prints one plain report line (`apple: unknown SoC apple,t8199, Apple drivers not started`) and continues on the boot framebuffer and the m1n1 console only. It never guesses a variant from a "nearby" chip id.
4. **Idle policy is a safety attribute, default the safe value.** `idle = SPIN` unless the table row says `WFI_RETAINED`. M4-class rows are `SPIN` and must also forbid WFIT/WFET in delay loops (Linux needs `arm64.nowfxt` for the same reason). A later `SAVE_WFI` value (save context, WFI, come back through RVBAR, restore) is a separate, tested feature; m1n1's secondary entry already handles "an RVBAR re-entry after deep wfi" (`src/smp.c`), which shows the shape (INFERRED).

### 5.2 The table (data, keyed by the DT root compatible `apple,tNNNN`)

One `const` row per SoC, no code in it. Fields and the value set for each:

| Field | Type / values | Source of each value |
| --- | --- | --- |
| `compat` | `"apple,t8112"` | DT root |
| `soc_id` | `0x8112` | section 1 |
| `family` | `M1`, `M2`, `M3`, `M4`, `A18`, `M5`, `M6` | section 1 |
| `size` | `BASE`, `PRO`, `MAX`, `ULTRA` | section 1 |
| `aic` | `AIC1`, `AIC2`, `AIC3` | section 1 (cross-checked against the AIC node compatible at boot; mismatch = refuse) |
| `idle` | `WFI_RETAINED`, `SPIN` (future `SAVE_WFI`) | section 1 WFI column; M4-class = `SPIN` |
| `start` | `SPIN_TABLE` (all rows today) | DT `enable-method` (cross-check) |
| `wfxt_ok` | bool: WFIT/WFET allowed | false on M4-class |
| `counter_1ghz` | bool: CNTFRQ scaled counter redirect set by m1n1 | true on M3 (m1n1 `counter_redirect`); read `CNTFRQ_EL0` anyway |
| `ctrr_hole` | bool: reserve the secondary-core CTRR range | true on M4-class |
| `usbpd` | `CD321X_I2C`, `ACE3_SPMI`, `UNKNOWN` | **per board, not per SoC** (the M3 has both): take it from the board's DT node compatible (`apple,cd321x` vs `apple,sn201202x`); the table holds only the default |
| `ans` | `ANS2`, `ANS2_SECURE_BAR` (M4-class), `UNKNOWN` | DT compatible plus presence of `nvme-secure-bar` |
| `status` | `MAPPED` (we have read sources for it), `ID_ONLY` (chip id known, blocks unknown) | section 1 |

Not in the table, on purpose: DART page size, interrupt counts, die count, MMIO addresses, CPU start offset when the tree gives a `cpu-release-addr`, SART version (all discovered, rule 1). The PMGR CPU-start offset (section 1 "Start offset") is needed only if Aienux starts cores itself instead of through m1n1's spin-table; keep it as an optional field `cpu_start_off` with the m1n1 values, used only by a future self-start path.

### 5.3 What stays per-family code (driver variants)

| Variant | Rows | Why it cannot be data |
| --- | --- | --- |
| AIC1 | M1 | different register layout from AIC2/3 (Linux `irq-apple-aic.c` binds `apple,aic` separately from `apple,aic2` and `apple,t8122-aic3`) |
| AIC2 / AIC3 | M1 Pro and later | one driver for both with per-version offsets is how m1n1 does it (`aic23_init(2, ...)` and `aic23_init(3, ...)`, `src/aic.c`); the differences are register offsets and strides, which can be a small per-version struct |
| DART T8020-style vs T8110-style | M1 vs M2 and later | different TCR/TTBR/TLB command registers (Linux `apple-dart.c` has two `apple_dart_hw` families); M4+ UNKNOWN |
| ANS2 vs ANS2 with secure bar | M1 to M3 vs M4-class | different register window and queue set-up (m1n1 `src/nvme.c` `NVME_T8132`) |
| USB-C PD: CD3217 over I2C vs ACE3 over SPMI | per board | different bus and protocol |
| Idle | `WFI_RETAINED` vs `SPIN` | safety: state loss |

L250 builds the table, the discovery reads, AIC2 and AIC3 (AIC1 if cheap) and spin-table start; the other variants are later rungs (D4, D6, D10). This section is the contract between L372 and L250: if L250 finds a fact that contradicts section 1, it notes L372 by `ledger note L372` and this map is corrected.

## 8. Sources (all read 2026-10-21)

Local read-only copies under `/tmp/l372-src` on the Spark, GPL code read for facts only, nothing copied into Aienux:

- m1n1: `github.com/AsahiLinux/m1n1` at `7cde7d1` (2026-10-19). Files: `src/soc.h`, `src/midr.h`, `src/smp.c`, `src/chickens.c`, `src/aic.c`, `src/dart.c`, `src/nvme.c`, `src/sart.c`, `src/kboot.c`, `src/kboot_atc.c`, `src/firmware.c`, `src/utils.c`, `src/hv/hv.h`, `proxyclient/m1n1/hv/__init__.py`, `proxyclient/tools/run_guest.py`. Tags and dates: v1.6.0 2026-06-25, v1.6.1 2026-08-08, v1.9.9 and v2.0.0 both 2026-10-02.
- Asahi docs (the source of asahilinux.org/docs): `github.com/AsahiLinux/docs` at `0d1f891` (2026-10-05): `docs/hw/soc/soc-codenames.md`, `docs/platform/feature-support/{m1..m6}.md`, `docs/sw/m1n1-hypervisor.md` (last changed `fd409d3`, 2026-07-31).
- Asahi installer: `github.com/AsahiLinux/asahi-installer` at `e93743d` (2026-10-02): `src/main.py`, `src/step2/step2.sh`.
- Asahi Linux: `github.com/AsahiLinux/linux` branch `asahi` at `94fb233` (tag `asahi-7.1.13-3`, 2026-09-12) and branch `apple-soc/dt-7.4` at `a8ac602` (2026-09-25): `arch/arm64/boot/dts/apple/*`, `drivers/iommu/apple-dart.c`, `drivers/irqchip/irq-apple-aic.c`, `drivers/gpu/drm/asahi/hw/mod.rs`, `drivers/gpu/drm/apple/*`.
- Asahi progress report 7.2, `asahilinux.org/2026/08/progress-report-7-2/`, dated 26 August 2026. Quotes used: "M3 series machines will target the ABI found in macOS 14.8.3"; "Starting with M3 Pro/Max, Apple has switched to ACE3"; "ACE3 instead uses the SPMI bus"; "requiring a new sequence of tunables at initialisation due to the shift to TSMC's N3 process node"; "calling WFI causes the core to lose its state and crash whatever was running on it"; "Apple now sets each core's chicken bits in mBoot"; "a breaking change in Apple's NVMe controller firmware in the macOS 15.x firmware bundle"; "so we now have working NVMe on M4 and M5"; "SPTM is mandatory for XNU on M4 and above"; "mBoot (formerly iBoot) starts it in EL2, it does its job, then jumps to whatever payload is attached to it"; "Not much else is working on M4 and M5 so we aren't quite ready to enable them in the Asahi Installer just yet"; "Sven has been working on implementing a UEFI Runtime Service based PSCI conduit".
- Third party, not Asahi: `github.com/antiapplefox/m5-linux` README (no commit date visible). Quotes: "M2 had SPTM in its boot chain by 15.0; M4 was born with it (15.2)"; "M3 only got SPTM at macOS 26.0"; "M1 base (`t8103`) never got it"; "iBoot's `load_kernelcache` rejects the image (panics) if it isn't `MH_FILESET`"; and, decisive for its weight: "No hardware bring-up yet." Every SPTM-by-version cell above that says "third party" rests on this source alone.
- The M2 dumps: `~/handoffs/macbook-inventory-2026-10-20/ioreg-devicetree.txt` (OBSERVED values for the M2 row).

Sections 2, 3, 4, 6 and 7 follow in later commits.
