# Apple Silicon platform layer (Aienux, M1 to M4)

AienOS is the whole system; Aienux is its kernel. This page describes the part of Aienux that lets one kernel image start on any Apple Silicon Mac from the M1 to the M4, and say plainly "no" to a chip it does not know. Ledger L250. The chip facts come from the family map (`docs/APPLE_SILICON_FAMILY_MAP.md`, L372, branch `feat/apple-silicon-family`); this page is the build.

Nothing here has run on a Mac. Every result below comes from host tests against chip models and the pinned Apple device trees, or from QEMU, which is not a Mac. Real Apple hardware: NOT TESTED.

Tags: **OBSERVED** we ran it and saw it; **SOURCED** read in a named document or source file; **INFERRED** reasoned from sourced facts, not checked; **UNKNOWN** not known, with the experiment that settles it (section 6).

## 1. In short

- The kernel reads the device tree that m1n1 (the Asahi boot loader) hands it, finds the chip in a table of 14 Apple chips, and checks the tree agrees with the table. An unknown chip, or a tree that disagrees, gets one plain refusal line and no Apple driver starts. OBSERVED (host tests, QEMU gate).
- It has its own entry point in the Linux arm64 "Image" format m1n1 loads, drops from the hypervisor level (EL2) to the kernel level (EL1) the way Apple cores require, and runs on QEMU through the same path. OBSERVED (QEMU gate).
- It has drivers for all three Apple interrupt controller generations (AIC1 on M1, AIC2 on M1 Pro/Max/Ultra and M2, AICv3 on M3 and M4), the fast-interrupt path Apple uses for timers and core-to-core signals, and the spin-table start of the other cores. OBSERVED on chip models only.
- The regular UEFI kernel images (DGX Spark, Raspberry Pi 5) are byte-for-byte unchanged whether the Apple switch is on or off. OBSERVED.

## 2. What exists, and which family-map rows it covers

| Part | Files (`native/kernel/`) | Family-map row | State |
|---|---|---|---|
| Device-tree reader | `apple/fdt.c`, `apple/fdt.h` | D1 boot handoff | OBSERVED on 7 Apple trees, QEMU's tree, 20 malformed trees, 3000 random bit flips |
| Family table and platform probe | `apple/apple_family.c`, `.h` | section 1 (all rows M1 to M4), D3, D10 (USB-C kind), idle rule | OBSERVED on 7 Apple trees and 2 refusal trees |
| Serial console from `/chosen` | `apple/apple_console.c`, `.h` | D18 | OBSERVED (PL011 on QEMU); Apple's Samsung-style UART written from register facts, not run |
| Raw Image entry, EL2 to EL1, vectors | `apple/apple_entry.S`, `apple/apple_image.ld`, `apple/apple_main.c` | D1 | OBSERVED on QEMU at EL2, EL2 with E2H forced on, and EL1 |
| AIC1, AIC2, AICv3 driver; FIQ path; fast IPI send | `apple/aic.c`, `apple/aic.h`, `apple/aic_live.c` | D3 | OBSERVED on host chip models; not run in QEMU (QEMU has no AIC) |
| Spin-table start of other cores | `apple/apple_smp.c`, `.h`; `apple_secondary_entry` in `apple_entry.S` | D2 | Release logic OBSERVED on a host model; the same secondary entry code OBSERVED on QEMU (started there through PSCI, since QEMU has no spin table) |
| RTKit client, SART, DART | not started | D5, D6 | stretch, not started |

Build switch: `make -C native/kernel CK_APPLE=1` builds `$(OUT)/apple/Image` next to the normal images. Without the switch nothing Apple is compiled into any image.

## 3. The family table

One row per chip, keyed by the root `compatible` string of the device tree (`native/kernel/apple/apple_family.c`).

| Chip | SoC | Interrupt controller | Idle | WFIT/WFET | Extra cores | USB-C chip seen | CPUs |
|---|---|---|---|---|---|---|---|
| M1 | t8103 | AIC1 | WFI after retention set-up | allowed | start | CD3217 (I2C) | 8 |
| M1 Pro / Max | t6000 / t6001 | AIC2 | same | allowed | start | CD3217 | 10 |
| M1 Ultra | t6002 | AIC2, 2 dies | same | allowed | start | CD3217 | 20 |
| M2 | t8112 | AIC2 | same | allowed | start | CD3217 | 8 |
| M2 Pro / Max | t6020 / t6021 | AIC2 | same | allowed | start | CD3217 | 12 |
| M2 Ultra | t6022 | AIC2, 2 dies | same | allowed | start | CD3217 | 24 |
| M3 | t8122 | AICv3 | same | allowed | start | CD3217 or ACE3 (SPMI), per board | 8 |
| M3 Pro / Max | t6030 / t6031 / t6034 | AICv3 | same | allowed | start | ACE3 | 12 / 16 / 16 |
| M3 Ultra | t6032 | AICv3, 2 dies | same | allowed | start | none in the tree yet | 32 |
| M4 | t8132 | AICv3 | spin only | not allowed | refused until the CTRR range is reserved | none in the tree yet | 10 |

- Chip names, interrupt controller compatibles, CPU counts and USB-C chips: SOURCED from the Asahi Linux device trees at 94fb2334, compiled as test fixtures.
- AIC generation per chip and die counts: SOURCED (same trees; `apple,aic2` on t600x and t602x, `apple,t8122-aic3` from the M3 on).
- Idle rule, WFIT/WFET rule and the M4 CTRR range: SOURCED through the family map section 5 (m1n1 `kboot.c` and `chickens.c`).
- Every row starts its other cores through the spin table that m1n1 fills: SOURCED (family map section 1, "Start").
- M4 Pro/Max (t6040/t6041), M5 (t8142) and later are not rows: their interrupt controller is UNKNOWN or INFERRED in the family map, so the kernel refuses them by name (U5). The QEMU gate proves the refusal with a t8142 tree.
- DART page size is not in the table: the DART reports it in its own `PARAMS1` register (family map section 5, rule 1). It will be read there when D6 is built.

## 4. How each part behaves

**Device-tree reader.** Checks the header, block layout and every token before use, with fixed limits (32 levels, 8 MiB). Translates register addresses up through each bus's `ranges`, and refuses a bus with no `ranges` instead of guessing. OBSERVED.

**Platform probe.** Refusals, each one plain line ending "Apple drivers not started": not an Apple tree; an Apple chip not in the table; the interrupt controller missing or of another generation; CPU count outside the row; a CPU whose start method is not spin-table. OBSERVED.

**Entry.** The Image header follows Linux `Documentation/arch/arm64/booting.rst` (SOURCED). Apple cores force the hypervisor bit E2H on (SOURCED: Linux `el2_setup.h`, "Fruity CPUs ... have HCR_EL2.E2H implemented as RAO/WI"). The entry reads that bit back and takes the matching register layout. QEMU normally leaves it off, so a test-only image forces it on to run the Apple path in the gate. OBSERVED. A deliberately wrong timer-register layout under E2H makes the gate fail, as it must. OBSERVED.

**Interrupt controller (D3).**
- Reads the controller's own size registers.
- Refuses when the register block would run past the size the tree gives, or when the event register sits inside a die's block.
- Masks and clears every interrupt on every die, then switches the controller on.
- Delivers with a fixed budget per call, so a line that never goes quiet cannot hang the kernel.
- Register offsets: SOURCED from Linux `drivers/irqchip/irq-apple-aic.c`, read only, no code copied. OBSERVED on models.

**Fast-interrupt (FIQ) path.**
- Acknowledges a core-to-core signal before running its handler, so a signal sent during the handler is not lost.
- Masks a timer that is still firing after its handler and counts it, instead of re-entering forever.
- Switches off performance-counter FIQs nobody asked for.
- Never reads Apple-only registers on a non-Apple CPU.
- OBSERVED on models. The image wires the FIQ and IRQ vectors to these handlers once the controller is up, and keeps interrupts masked for now.

**Starting the other cores (D2).** Follows the spin-table rules in `booting.rst` (SOURCED). Every refusal writes nothing. It refuses when the core:
- is the boot core;
- has a start method other than spin-table;
- is on an M4-class chip (the CTRR range is not reserved yet);
- has no aligned release address;
- has a release address outside a `/memreserve/` region, or inside our own image;
- has a release slot that is not zero.

For a core that passes, it writes the entry address, flushes it to memory, signals with `sev`, and waits a bounded number of polls for the core to check in. The pinned Apple trees carry no release address (m1n1 adds it at boot), so every extra core there is refused, never guessed. OBSERVED.

**Idle.** A parked core spins. WFI is used only on rows that allow it and only after our own state-retention set-up runs, which does not exist yet. So every chip spins today. OBSERVED (the image prints the policy in force).

## 5. Tests and results (2026-10-21, branch `feat/apple-silicon-platform`)

All logs are in `evidence/apple-silicon-platform/`.

| Command | What it proves | Result |
|---|---|---|
| `make -C native/kernel apple-test` | device-tree reader, family table, AIC models, FIQ path, spin-table model; then the mutants | PASS: `test_apple_fdt` 356 checks, `test_apple_aic` 150, `test_apple_smp` 55; all 10 mutants turn their test red (`apple-test.log`) |
| `make -C native/kernel apple-sanitize` | the same tests under ASan and UBSan | PASS (`apple-sanitize.log`) |
| `scripts/qemu_ck_apple_boot_test.sh` | the Image boots in QEMU at EL2, at EL2 with E2H forced, and at EL1; refuses an unknown Apple chip (t8142) and a tree without its interrupt controller; cores 1 to 3 check in through the secondary entry on every row; the timer-layout mutant fails | PASS, 60 checks (`qemu_ck_apple_boot_test.log`, one serial log per row) |
| `scripts/ck_apple_flag_check.sh` | default and Pi 5 `BOOTAA64.EFI` are byte-identical to origin/main 18e27659 with `CK_APPLE` unset and `=1`, and carry no Apple symbol | PASS (`ck_apple_flag_check.log`) |
| `make -C native/kernel test` and `scripts/qemu_ck_boot_test.sh`, with `CK_APPLE` off and `CK_APPLE=1` | the existing gates are unaffected | PASS both ways (`existing-*.log`, `existing-gates-summary.txt`) |

**Mutants.** Each one is a deliberately broken version of the code. Each must turn its test red, and the kernel build refuses all of them.

| Mutant | What it breaks | Caught by |
|---|---|---|
| `CK_FDT_MUTANT_NAMEOFF` | property-name offset not bounds-checked | `test_apple_fdt` |
| `CK_FDT_MUTANT_RANGES_IDENTITY` | bus `ranges` ignored | `test_apple_fdt` |
| `CK_APPLE_MUTANT_GUESS` | unknown chip takes the first row | `test_apple_fdt` |
| `CK_AIC_MUTANT_DIE_STRIDE` | die blocks 4 bytes off | `test_apple_aic` |
| `CK_AIC_MUTANT_V3_OFFSET` | AICv3 read at the AIC2 offset | `test_apple_aic` |
| `CK_AIC_MUTANT_IPI_ACK_LATE` | core-to-core signal acknowledged after its handler | `test_apple_aic` |
| `CK_AIC_MUTANT_NO_ENABLE` | controller never switched on | `test_apple_aic` |
| `CK_SMP_MUTANT_NO_CLEAN` | release not flushed to memory | `test_apple_smp` |
| `CK_SMP_MUTANT_NO_SEV` | parked cores never woken | `test_apple_smp` |
| `CK_SMP_MUTANT_NO_RSV_CHECK` | release slot outside reserved memory accepted | `test_apple_smp` |

The chip models answer only at hand-written literal addresses taken from the register facts, never at addresses computed by the driver. A wrong offset therefore lands nowhere, or on the wrong die, and the test goes red. INFERRED: a model can only catch what it models. Real register values at reset are UNKNOWN (U1, U6).

## 6. UNKNOWN rows

| # | Unknown | Effect today | Experiment that settles it |
|---|---|---|---|
| U1 | Real AIC `INFO` values (interrupt count, die count) per chip | the driver reads them and refuses values that do not fit the tree | Read `INFO1`/`INFO3` once through the m1n1 proxy on each target (attended, read-only), or take the count from an Asahi Linux boot log |
| U2 | Whether m1n1 puts each `cpu-release-addr` inside a `/memreserve/` entry, as `booting.rst` requires | if it does not, every extra core is refused (safe: boot core only) | Save the device tree m1n1 hands to Linux (`/sys/firmware/fdt` on Asahi Linux) and list its reservation entries |
| U3 | Whether EL1 timer FIQs need `VM_TMR_FIQ_ENA_EL2` set by our EL2 code | the entry sets it on Apple CPUs only (INFERRED from `irq-apple-aic.c`) | On hardware: timer FIQ at EL1 with and without the bit |
| U4 | The M4-class CTRR range on extra cores (`CTRR_C_LWR/UPR_EL1`) | M4 extra cores refused | Read the register encodings in m1n1 `src/kboot.c` (MIT), read the range at boot, reserve it, then lift the refusal |
| U5 | Interrupt controller and CPU facts of M4 Pro/Max (t6040/t6041), M5 (t8142) and later | refused by name | Asahi device trees for those chips once published, or an ADT dump (family map section 7) |
| U6 | Whether AIC2/AICv3 deliver with `IRQ_CFG` left at its reset value | the driver does not program `IRQ_CFG` | On hardware: unmask one known interrupt and see it arrive |
| U7 | M5 Pro/Max add a core-type flag in MPIDR bit 18 (family map row D2) | the core table matches the full affinity, so the tree's `reg` must carry the same bit | Check an M5 Pro/Max device tree once published |
| U8 | Whether a parked core should spin or use `wfe` on rows that allow WFI | parked cores spin (safe on every row) | Decided with the state-retention idle work |

## 7. Limits

- No Mac booted, flashed or changed. Real Apple hardware: NOT TESTED.
- QEMU has no Apple interrupt controller and no spin table. The QEMU gate exercises the entry, the console, the vectors, the timer, the refusals, and the secondary-core entry path started through PSCI. The AIC and the spin-table release are proven on models only.
- Interrupts stay masked in the Image. The IRQ and FIQ handlers are wired but never reached in QEMU.
- RTKit, SART and DART (D5, D6) are not started.
- Sources were read only, never copied. Linux (GPL) `irq-apple-aic.c`, `el2_setup.h`, `head.S`, `booting.rst`, `serial_s3c.h` and `apple_m1_pmu.h`, from AsahiLinux/linux 94fb2334. The device-tree sources ("GPL-2.0+ OR MIT") are compiled into test fixtures under `native/kernel/tests/fixtures/apple/` only, never into the kernel (`SOURCES.md` there).
