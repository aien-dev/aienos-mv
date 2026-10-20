//! The x86_64 machine of the own VMM (candidate B of ledger L261): everything that is not
//! the hypervisor call itself, so it builds and is unit-tested on every host.
//!
//! The VMM boots the AienOS x86 kernel (BOOTX64.EFI) without firmware code. It plays the
//! part of UEFI with a small shim: guest memory holds a system table, boot services whose
//! entries are 16-byte stubs that trap to the VMM with one `out` instruction, ACPI tables
//! and an identity map; the VMM answers the handful of calls the kernel makes
//! (native/kernel/x86/efi_x86.c: SetWatchdogTimer, LocateHandleBuffer for GOP,
//! GetMemoryMap, ExitBootServices). After ExitBootServices the kernel runs on the machine
//! below: COM1 16550 (polled), HPET, I/O APIC registers, ACPI PM1a (S5 power off), reset
//! ports; the local APIC is the hypervisor's (WHPX xAPIC emulation).
//!
//! - [`mem`]: guest RAM and the physical layout
//! - [`boot`]: GDT, TSS, page tables, PE loading, initial register state
//! - [`acpi`]: RSDP, XSDT, FADT, DSDT (\_S5), MADT, HPET tables
//! - [`uefi`]: the boot-services shim
//! - [`devices`]: the port and MMIO device model
//! - [`esp`]: reads EFI/BOOT/BOOTX64.EFI out of a raw or qcow2 disk image

pub mod acpi;
pub mod boot;
pub mod devices;
pub mod esp;
pub mod mem;
pub mod uefi;
