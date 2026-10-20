/* direct_boot.h -- the "direct boot" record aienos-run's own VMM (crates/aienos-run,
 * backend kvm) passes instead of a UEFI system table (docs/AIENOS_RUN_LINUX.md).
 *
 * The VMM plays the firmware's part itself, the way Firecracker boots a Linux kernel
 * without firmware: it loads this PE image flat into guest RAM, builds the ACPI tables
 * (RSDP/XSDT/MADT/GTDT/FADT/SPCR/DSDT) and a UEFI-format memory map, leaves the vCPU at
 * EL1h with an identity map and caches on (as UEFI leaves it), and calls the PE entry
 * point with x0 = CK_DIRECT_BOOT_MAGIC (never a valid EFI_HANDLE: bits 63..48 are
 * non-canonical) and x1 = the address of this record. efi_main then skips every boot
 * service and enters the kernel with the same CHANDOF3 handoff the UEFI path builds.
 * Version 1 is frozen; a new layout gets a new version number. */
#ifndef AIENOS_DIRECT_BOOT_H
#define AIENOS_DIRECT_BOOT_H
#include <stdint.h>
#include <stddef.h>

#define CK_DIRECT_BOOT_MAGIC 0x425249444e454941ull /* "AIENDIRB" little endian */
#define CK_DIRECT_BOOT_VERSION 1u

struct ck_direct_boot {
    uint64_t magic;        /* CK_DIRECT_BOOT_MAGIC */
    uint32_t version;      /* CK_DIRECT_BOOT_VERSION */
    uint32_t desc_version; /* EFI_MEMORY_DESCRIPTOR_VERSION (1) */
    uint64_t rsdp;         /* ACPI 2.0 RSDP physical address */
    uint64_t memory_map;   /* EFI_MEMORY_DESCRIPTOR array, physical address */
    uint64_t map_size;     /* bytes */
    uint64_t desc_size;    /* bytes per descriptor (>= 40) */
    uint64_t vendor;       /* NUL-terminated ASCII name of the VMM, or 0 */
};
_Static_assert(sizeof(struct ck_direct_boot) == 56, "ck_direct_boot size");
_Static_assert(offsetof(struct ck_direct_boot, rsdp) == 16, "ck_direct_boot.rsdp offset");
_Static_assert(offsetof(struct ck_direct_boot, vendor) == 48, "ck_direct_boot.vendor offset");
#endif
