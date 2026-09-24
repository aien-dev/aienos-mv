# Fix chain: SMMUv3 xHCI DMA confinement (branch fix/m3-smmuv3-ste-cd-layout)

Base: 675d8c4 (forensic snapshot, branch wip/m3-smmu-c-bad-ste-635111c).
This note is appended to per commit; the original snapshot evidence is left
untouched in wip_m3_smmuv3_c_bad_ste_2026-09-24.md.

## Commit 1: fix(smmu): runtime-align linear stream table for QEMU SMMUv3

### Build blocker (root cause)

The snapshot tree did not build for aarch64-unknown-uefi. `cargo check` passes,
but the real `cargo build` traps rustc (SIGTRAP) in the COFF object writer.
A/B on the static alignment attribute:

    align(8192)    BUILD OK
    align(16384)   SIGTRAP
    align(262144)  SIGTRAP

COFF/PE cannot encode section alignment above 8192, so `repr(C, align(262144))`
on SMMU_TABLES was unbuildable. This is why the alignment fix had never been
re-run; the reported C_BAD_STE run came from the older align(4096) image.

### Change

- Removed `repr(C, align(262144))` from SMMU_TABLES.
- Allocate the tables at runtime: `Layout::from_size_align(size_of::<SmmuTables>(),
  262144)`, zeroed, cached in a shared `AtomicUsize`; one accessor
  `smmu_tables()` feeds both the configure path and the fault-dump path.
- Kept 4096 stream table entries.
- Added explicit geometry asserts: table size == 262144, LOG2SIZE == 12,
  SID < 4096, and the aligned base % 262144 == 0.

### Verification

    cargo fmt --check                                        OK
    cargo build --release -p aienos-boot --target aarch64-unknown-uefi \
        --features usb-keyboard                              OK

### QEMU result (AIENOS_QEMU_SMMU=1)

    smmu_ste: base=0xbbf40000 cfg=0xc sid=0x10 addr=0xbbf40400 words=[0xbbf8030b,0x0]

- base 0xbbf40000 is 256 KiB aligned; cfg 0xc is LOG2SIZE=12, FMT=0 (linear).
- STE address == base + 0x10 * 64. STE is valid (V=1, Config=5, S1Fmt=0,
  CTXPTR=0xbbf80300).
- C_BAD_STE (0x04) is eliminated. The next event is C_BAD_CD (0x0a): the STE is
  accepted, the CD is fetched, and QEMU rejects the CD.

### Acceptance progression

    C_BAD_STE gone              PROVEN
    C_BAD_CD gone               pending (commit 2)
    CD accepted                 not yet proven
    stage-1 PT walk reached     not yet proven
    DMA translation succeeds    not yet proven
    xHCI Enable Slot completes  not yet proven
    keyboard attaches           not yet proven
