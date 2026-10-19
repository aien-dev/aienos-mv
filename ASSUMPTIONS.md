# AIENOS assumptions: what the tests do not prove

Idea borrowed from seL4's published proof assumptions
(https://sel4.systems/Verification/assumptions.html): say exactly which code
and which hardware behaviour the evidence does not cover, so human review goes
to a short list instead of the whole tree. No seL4 text or code is copied.

This page covers the C kernel (`native/kernel`, `native/boot`). It lists every
region that the host unit tests (`make -C native/kernel test`) do not cover,
or that is trusted without evidence. QEMU gates (`scripts/qemu_ck_*_test.sh`)
run much of this code, but QEMU is an emulator: it qualifies nothing physical,
and it does not model caches, TLB timing, DMA ordering or firmware bugs the way
silicon does. A row here is not a known bug. It is a place where a bug would
not be caught by the tests we have.

Line counts: `wc -l` (physical lines, comments and blank lines included; `cloc`
is not installed on the Spark), measured on branch `qual/assumptions-ktap` on
2026-10-19. The whole C kernel and boot stub, without host tests and
third-party code, is 53,052 lines (`find native/kernel native/boot -name
'*.[chS]' -not -path '*/tests/*' -not -path '*/third_party/*' | xargs wc -l`).

| # | Region | Files | Lines | Host tests | Why it is trusted | What would reduce it |
| --- | --- | --- | ---: | --- | --- | --- |
| A1 | AArch64 boot and exception assembly | `native/boot/efi_entry.S` 48, `native/kernel/arch/vectors.S` 265, `arch/m3.S` 239, `arch/smp_entry.S` 38 | 590 | none | Register save and restore, EL2 to EL1 drop, EL0 entry and secondary-core entry cannot run on the host; the QEMU M1 and M3 gates only show that the paths complete once under TCG. | A host-side checker that diffs the saved-register layout against the C `struct` offsets, plus fault-injection boots that return through every vector. |
| A2 | x86_64 boot and trap assembly (iMac port) | `native/boot/efi_entry_x86_64.S` 52, `native/kernel/x86/trap.S` 101, `x86/smp_x86.S` 101 | 254 | none | Same reason as A1; `scripts/qemu_ck_imac_kernel_test.sh` boots it in QEMU q35 with OVMF, which says nothing about the 2017 iMac itself. | Make the x86 kernel print the same KTAP block, so the QEMU gate and the iMac boots are read by one parser. |
| A3 | Inline assembly inside C | 101 lines containing `__asm__` across `arch/`, `core/`, `mm/`, `dev/`, `x86/`, `native/boot` | 101 | none | System-register reads and writes, barriers and cache or TLB instructions are compiled out of every host test build. | Keep them behind the accessors in `arch/arch.h` so the count shrinks and each one is reviewed once. |
| B | UEFI stub and boot handoff | `native/boot/efi_main.c` 217, `efi_model.c` 224, `efi_gop.c` 84, `image.ld` 114, `native/kernel/core/kmain.c` 400, `core/report.c` 115 | 1,154 | handoff shape only (`tests/test_handoff.c` over `core/handoff_check.c`) | The memory map, ExitBootServices retry, linker layout and the first kernel code run only under firmware; QEMU with AAVMF is the only automated check. | Record the handoff block in every receipt and host-test the parser against real handoffs captured from the Spark, iMac and Pi 5. |
| C | Early device init and drivers | `native/kernel/dev/pci.c` 677, `usb_kbd.c` 741, `xhci_fence.c` 426, `net_bind.c` 414, `nvme_bind.c` 327, `usb_hid.c` 279, `core/smp.c` 241, `mmio_window.c` 199, `devices.c` 183, `net_udp.c` 123, `core/acpi_platform.c` 93, `nvme_shutdown.c` 87, `virtio_net.c` 38 | 3,828 | none (the Pi 5 drivers `pi5_pcie.c`, `pi5_net.c`, `pi5_gem.c`, `pi5_rng.c` have host tests against models and are not counted here) | These talk to device registers, which exist only in QEMU's device models or on real boards; a QEMU model accepts sequences real controllers may reject. | Register-level host tests against recorded MMIO traces from real boots, and one hardware boot per driver change with the KTAP block in its receipt. |
| D | DMA confinement | `native/kernel/core/smmu_svc.c` 455 (no host test), `core/smmu.c` 455 (host-tested table builder, `tests/test_smmu.c`), `dev/dma_window.c` 197 (host-tested, Pi 5 software-only window) | 1,107 | table building and window arithmetic only | We assume devices issue DMA only through the SMMU stream tables we build, that the SMMU enforces them, and that no device does DMA before the kernel fences it. QEMU images may be built with `CK_QEMU_UNSAFE_DMA=1`, a TEST-ONLY unconfined bypass, so QEMU store gates do not prove confinement. `docs/P3_NVME_UNSAFE_ASSUMPTIONS.md` lists the NVMe-specific ones. | A negative hardware test: a device told to DMA outside its window must fault and be reported, on the Spark and the Pi 5. |
| E | Machine interface: MMU, cache, TLB, interrupts, timer, entropy | `native/kernel/mm/mmu.c` 511, `arch/gic.c` 190, `arch/exception.c` 102, `arch/timer.c` 83, `arch/rndr.c` 74, `arch/arch.h` 65 | 1,025 | the page-table and frame logic in `mm/pt.c` and `mm/frames.c` is host-tested; `mm/mmu.c` (the live switch and maintenance) is not; entropy selection is host-tested with a mocked RNDR | Cache and TLB maintenance, the TTBR switch, GIC programming and timer re-arm are trusted to follow the Arm ARM. QEMU TCG keeps caches coherent and TLBs exact, so a missing `dsb`, `isb`, `tlbi` or cache clean is invisible there (the GB10 cache lesson, 2026-10-03). | Hardware-only self tests in the KTAP block (for example a deliberate stale-TLB probe that must fault), run on the Spark and the Pi 5, never on QEMU alone. |
| F | Firmware | none in this tree: AAVMF/EDK2 in QEMU, the Spark's UEFI and Trusted Firmware (PSCI), the Pi 5 EEPROM firmware, the iMac EFI | 0 | the ACPI table parsers (`core/acpi.c`, `core/acpi_dev.c`) are host-tested for parsing only | We trust that firmware hands over a correct memory map and correct ACPI tables (MADT, SPCR, FADT, IORT), that PSCI CPU_ON and SYSTEM_RESET do what the spec says, and that firmware leaves no device doing DMA. | Print the firmware vendor and version and a hash of each ACPI table in the boot report, so a receipt names the exact firmware it ran on. |
| C2 | x86_64 port C code (iMac) | `native/kernel/x86/kmain_x86.c` 880, `tg3_x86.c` 687, `ahci_x86.c` 319, `pci_x86.c` 307, `report_x86.c` 144, `console_x86.c` 126, `x86.h` 123, `efi_x86.c` 93, `report_x86.h` 20 | 2,699 | the network report frames only (`tests/test_imac_report.c`, run by `scripts/ck_imac_report_test.sh`, not by `make test`) | Descriptor tables, local APIC, HPET, AHCI, NVMe and BCM57766 code talks to hardware that QEMU q35 models only roughly (QEMU has no BCM57766). | The same KTAP block from `kmain_x86.c`, and iMac receipts that embed it. |
| G | Toolchain and test oracle | gcc and binutils (`aarch64-linux-gnu-` on non-aarch64 hosts), QEMU `qemu-system-aarch64` | 0 | none | The compiler, linker and objcopy are trusted to translate the source faithfully, and QEMU is trusted as the reference for every QEMU gate. | Reproducible builds across two toolchains (`scripts/ck_repro_build.sh` today builds twice with one toolchain and compares digests) and every QEMU PASS paired with a hardware receipt before it counts physically. |
| H | Vendored code | `native/kernel/third_party/omega` (C and headers) | 992 | through the stage host tests that use it | Copied byte for byte from the omega commit pinned in `omega.lock` and checked by `scripts/omega_lock_check.sh`; its own tests live in the omega repo, not here. | Run omega's own tests against the pinned commit in this repo's CI. |

## How receipts use this list

Every receipt should say which rows its tested path relied on. A QEMU receipt
always relies on E, F and G (QEMU stands in for the machine interface and the
firmware). A hardware receipt relies on the rows of the code it ran, and its
KTAP block (`docs/qualification/KTAP_RECEIPTS.md`) shows which in-kernel self
tests passed on that machine.

## Keeping this page honest

When a change adds a file to one of these regions, or adds a host test that
covers one, update the row and its line count in the same pull request. Re-run
the counts with the `wc -l` commands above, never by hand.
