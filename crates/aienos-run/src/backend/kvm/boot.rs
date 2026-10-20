//! What the VMM puts in guest memory before the first instruction: the machine layout,
//! ACPI tables, the identity page tables UEFI would have left, a UEFI-format memory map,
//! the direct-boot record (native/boot/direct_boot.h) and the kernel image itself.
//! Pure functions over a byte slice, so every table is unit-tested without /dev/kvm.
//!
//! Table layouts follow ACPI 6.5 (sections 5.2.5 RSDP, 5.2.6 SDT header, 5.2.8 XSDT,
//! 5.2.9 FADT, 5.2.12 MADT with GICC/GICD/GICR) and Microsoft's SPCR spec; the kernel's
//! own parsers (native/kernel/core/acpi.c) define which fields it reads.

/// Guest physical layout (the QEMU `virt` addresses, so the same kernel sees familiar
/// devices; the kernel itself takes every address from ACPI).
pub const GICD_BASE: u64 = 0x0800_0000;
pub const GICD_SIZE: u64 = 0x1_0000;
pub const GICR_BASE: u64 = 0x080a_0000;
/// One redistributor (RD + SGI frames) per vCPU.
pub const GICR_STRIDE: u64 = 0x2_0000;
pub const UART_BASE: u64 = 0x0900_0000;
pub const UART_SIZE: u64 = 0x1000;
pub const RAM_BASE: u64 = 0x4000_0000;

/// Offsets inside RAM.
pub const ACPI_OFF: u64 = 0x0;
pub const ACPI_SIZE: u64 = 0x1_0000;
pub const RECORD_OFF: u64 = 0x1_0000;
pub const RECORD_SIZE: u64 = 0x1_0000;
pub const PT_OFF: u64 = 0x2_0000;
pub const PT_SIZE: u64 = 0x1_0000;
pub const STACK_OFF: u64 = 0x10_0000;
pub const STACK_SIZE: u64 = 0x10_0000;
pub const IMAGE_OFF: u64 = 0x20_0000;

pub const PSTATE_EL1H_DAIF: u64 = 0x3c5;
/// MAIR: attr0 Device-nGnRnE, attr1 Normal write-back RW-allocate.
pub const MAIR: u64 = 0xff << 8;
/// SCTLR_EL1: RES1 bits (11, 20, 22, 23, 28, 29) + M (MMU) + C (data cache) + I (icache).
pub const SCTLR: u64 = (1 << 0) | (1 << 2) | (1 << 12) | (1 << 11) | (1 << 20) | (1 << 22)
    | (1 << 23) | (1 << 28) | (1 << 29);
/// CPACR_EL1.FPEN = 0b11: FP/SIMD not trapped (as UEFI leaves it).
pub const CPACR: u64 = 3 << 20;

/// TCR_EL1 for a 4 KiB-granule identity map of the low 512 GiB through TTBR0:
/// T0SZ=25 (39-bit VA, walk starts at level 1), IRGN0/ORGN0 write-back, SH0 inner,
/// TG0 4K, EPD1 (no TTBR1 walks), IPS from `pa_range` (ID_AA64MMFR0_EL1.PARange).
pub fn tcr(pa_range: u64) -> u64 {
    let t0sz = 25u64;
    let irgn0 = 1 << 8;
    let orgn0 = 1 << 10;
    let sh0 = 3 << 12;
    let epd1 = 1 << 23;
    let ips = (pa_range.min(5)) << 32;
    t0sz | irgn0 | orgn0 | sh0 | epd1 | ips
}

/// Level-1 table (39-bit VA): 512 entries of 1 GiB blocks. GiB 0 (devices) is
/// Device-nGnRnE and never executable; every other GiB up to `top` is Normal memory.
/// Returns the table bytes for TTBR0_EL1.
pub fn identity_l1(top: u64) -> [u8; 4096] {
    let mut t = [0u8; 4096];
    let gib = 1u64 << 30;
    let n = top.div_ceil(gib).min(512);
    for i in 0..n {
        let pa = i * gib;
        let af = 1u64 << 10;
        let desc = if i == 0 {
            // Block, AttrIndx 0 (device), AF, PXN | UXN.
            pa | 0b01 | af | (1 << 53) | (1 << 54)
        } else {
            // Block, AttrIndx 1 (normal WB), SH inner, AF.
            pa | 0b01 | (1 << 2) | (3 << 8) | af
        };
        t[i as usize * 8..i as usize * 8 + 8].copy_from_slice(&desc.to_le_bytes());
    }
    t
}

fn put16(b: &mut [u8], at: usize, v: u16) {
    b[at..at + 2].copy_from_slice(&v.to_le_bytes());
}
fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}
fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

fn checksum(b: &[u8]) -> u8 {
    0u8.wrapping_sub(b.iter().fold(0u8, |a, x| a.wrapping_add(*x)))
}

/// An ACPI table: 36-byte header + body, checksum filled.
fn sdt(sig: &[u8; 4], revision: u8, body: &[u8]) -> Vec<u8> {
    let mut t = vec![0u8; 36];
    t[0..4].copy_from_slice(sig);
    t[8] = revision;
    t[10..16].copy_from_slice(b"AIENOS");
    t[16..24].copy_from_slice(b"AIENRUN ");
    put32(&mut t, 24, 1);
    t[28..32].copy_from_slice(b"AIEN");
    put32(&mut t, 32, 1);
    t.extend_from_slice(body);
    let len = t.len() as u32;
    put32(&mut t, 4, len);
    t[9] = checksum(&t);
    t
}

/// MADT: one GICC per vCPU (MPIDR as KVM reports it), the GICD and one GICR range.
pub fn madt(mpidrs: &[u64]) -> Vec<u8> {
    let mut body = vec![0u8; 8]; // Local Interrupt Controller Address, Flags
    for (i, mpidr) in mpidrs.iter().enumerate() {
        let mut e = [0u8; 80];
        e[0] = 0x0b;
        e[1] = 80;
        put32(&mut e, 4, i as u32); // CPU interface number
        put32(&mut e, 8, i as u32); // ACPI processor UID
        put32(&mut e, 12, 1); // flags: enabled
        put32(&mut e, 20, 23); // performance interrupt GSIV (PMU PPI 7)
        put32(&mut e, 56, 25); // VGIC maintenance interrupt
        put64(&mut e, 68, *mpidr & 0xff00ff_ffff);
        body.extend_from_slice(&e);
    }
    let mut d = [0u8; 24];
    d[0] = 0x0c;
    d[1] = 24;
    put64(&mut d, 8, GICD_BASE);
    d[20] = 3; // GIC version
    body.extend_from_slice(&d);
    let mut r = [0u8; 16];
    r[0] = 0x0e;
    r[1] = 16;
    put64(&mut r, 4, GICR_BASE);
    put32(&mut r, 12, (GICR_STRIDE * mpidrs.len() as u64) as u32);
    body.extend_from_slice(&r);
    sdt(b"APIC", 5, &body)
}

/// FADT (276 bytes, revision 6): hardware-reduced ACPI, PSCI over HVC, X_DSDT.
pub fn fadt(dsdt: u64) -> Vec<u8> {
    let mut body = vec![0u8; 276 - 36];
    let o = |abs: usize| abs - 36;
    put32(&mut body, o(112), 1 << 20); // Flags: HW_REDUCED_ACPI
    put16(&mut body, o(129), 0b11); // ARM_BOOT_ARCH: PSCI_COMPLIANT | PSCI_USE_HVC
    body[o(131)] = 5; // FADT minor version (6.5)
    put64(&mut body, o(140), dsdt); // X_DSDT
    sdt(b"FACP", 6, &body)
}

/// SPCR (revision 2, 80 bytes): ARM PL011 at UART_BASE, 32-bit MMIO.
pub fn spcr() -> Vec<u8> {
    let mut body = vec![0u8; 80 - 36];
    let o = |abs: usize| abs - 36;
    body[o(36)] = 0x03; // interface type: ARM PL011
    body[o(40)] = 0; // GAS: system memory
    body[o(41)] = 32; // bit width
    body[o(43)] = 3; // access size: dword
    put64(&mut body, o(44), UART_BASE);
    body[o(52)] = 0x08; // interrupt type: GIC
    put32(&mut body, o(54), 33); // GSI: SPI 1
    body[o(58)] = 7; // 115200 baud (informational)
    body[o(61)] = 0;
    put16(&mut body, o(64), 0xffff);
    put16(&mut body, o(66), 0xffff);
    sdt(b"SPCR", 2, &body)
}

pub fn dsdt() -> Vec<u8> {
    sdt(b"DSDT", 2, &[])
}

/// Lays out RSDP, XSDT, FADT, DSDT, MADT and SPCR in `buf` (the ACPI area, physical
/// address `base`). Returns the RSDP's physical address.
pub fn build_acpi(buf: &mut [u8], base: u64, mpidrs: &[u64]) -> Result<u64, String> {
    let mut at = 0usize;
    let mut place = |buf: &mut [u8], t: &[u8]| -> Result<u64, String> {
        at = (at + 15) & !15;
        if at + t.len() > buf.len() {
            return Err("ACPI tables do not fit".into());
        }
        buf[at..at + t.len()].copy_from_slice(t);
        let pa = base + at as u64;
        at += t.len();
        Ok(pa)
    };
    let rsdp_pa = place(buf, &[0u8; 36])?;
    let dsdt_pa = place(buf, &dsdt())?;
    let fadt_pa = place(buf, &fadt(dsdt_pa))?;
    let madt_pa = place(buf, &madt(mpidrs))?;
    let spcr_pa = place(buf, &spcr())?;
    let mut ents = Vec::new();
    for pa in [fadt_pa, madt_pa, spcr_pa] {
        ents.extend_from_slice(&pa.to_le_bytes());
    }
    let xsdt_pa = place(buf, &sdt(b"XSDT", 1, &ents))?;
    let r = (rsdp_pa - base) as usize;
    let rsdp = &mut buf[r..r + 36];
    rsdp[0..8].copy_from_slice(b"RSD PTR ");
    rsdp[9..15].copy_from_slice(b"AIENOS");
    rsdp[15] = 2; // revision: ACPI 2.0+
    put32(rsdp, 20, 36);
    put64(rsdp, 24, xsdt_pa);
    rsdp[8] = checksum(&rsdp[0..20]);
    rsdp[32] = checksum(rsdp);
    Ok(rsdp_pa)
}

/// UEFI memory types (UEFI 2.10 table 7.10).
pub const EFI_LOADER_CODE: u32 = 1;
pub const EFI_LOADER_DATA: u32 = 2;
pub const EFI_CONVENTIONAL: u32 = 7;
pub const EFI_ACPI_RECLAIM: u32 = 9;
pub const EFI_MEMORY_WB: u64 = 0x8;
pub const DESC_SIZE: usize = 40;

/// The memory map: ACPI reclaim, loader data (record, page tables, stack), loader code
/// (the image), conventional for the rest of RAM.
pub fn memory_map(ram_bytes: u64, image_bytes: u64) -> Vec<u8> {
    let image_pages = image_bytes.div_ceil(4096);
    let image_end = IMAGE_OFF + image_pages * 4096;
    let regions = [
        (EFI_ACPI_RECLAIM, ACPI_OFF, ACPI_SIZE),
        (EFI_LOADER_DATA, RECORD_OFF, IMAGE_OFF - RECORD_OFF),
        (EFI_LOADER_CODE, IMAGE_OFF, image_end - IMAGE_OFF),
        (EFI_CONVENTIONAL, image_end, ram_bytes - image_end),
    ];
    let mut m = Vec::new();
    for (ty, off, len) in regions {
        let mut d = [0u8; DESC_SIZE];
        put32(&mut d, 0, ty);
        put64(&mut d, 8, RAM_BASE + off);
        put64(&mut d, 24, len / 4096);
        put64(&mut d, 32, EFI_MEMORY_WB);
        m.extend_from_slice(&d);
    }
    m
}

pub const DIRECT_BOOT_MAGIC: u64 = 0x4252_4944_4e45_4941; // "AIENDIRB"

/// The 56-byte direct boot record (native/boot/direct_boot.h, version 1).
pub fn direct_record(rsdp: u64, map_pa: u64, map_size: u64, vendor_pa: u64) -> [u8; 56] {
    let mut r = [0u8; 56];
    put64(&mut r, 0, DIRECT_BOOT_MAGIC);
    put32(&mut r, 8, 1);
    put32(&mut r, 12, 1);
    put64(&mut r, 16, rsdp);
    put64(&mut r, 24, map_pa);
    put64(&mut r, 32, map_size);
    put64(&mut r, 40, DESC_SIZE as u64);
    put64(&mut r, 48, vendor_pa);
    r
}

/// A loaded PE32+ image: entry offset and the bytes to place at IMAGE_OFF.
pub struct PeImage {
    pub entry: u64,
    pub size: u64,
}

/// Loads a PE32+ AArch64 EFI application into `dst` (zeroed, at least SizeOfImage):
/// headers, then each section at its virtual address, the rest zero (BSS). The image
/// relocates itself (native/boot/efi_entry.S), so no base relocations are applied.
pub fn load_pe(file: &[u8], dst: &mut [u8]) -> Result<PeImage, String> {
    let rd16 = |o: usize| file.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]));
    let rd32 = |o: usize| {
        file.get(o..o + 4)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    };
    if file.get(0..2) != Some(b"MZ") {
        return Err("not a PE image (no MZ)".into());
    }
    let pe = rd32(0x3c).ok_or("truncated DOS header")? as usize;
    if file.get(pe..pe + 4) != Some(b"PE\0\0") {
        return Err("no PE signature".into());
    }
    let machine = rd16(pe + 4).ok_or("truncated COFF header")?;
    if machine != 0xaa64 {
        return Err(format!("PE machine 0x{machine:x} is not AArch64 (0xaa64)"));
    }
    let nsec = rd16(pe + 6).ok_or("truncated")? as usize;
    let opt_size = rd16(pe + 20).ok_or("truncated")? as usize;
    let opt = pe + 24;
    if rd16(opt) != Some(0x20b) {
        return Err("not PE32+".into());
    }
    let entry = rd32(opt + 16).ok_or("truncated")? as u64;
    let size_of_image = rd32(opt + 56).ok_or("truncated")? as u64;
    let size_of_headers = rd32(opt + 60).ok_or("truncated")? as usize;
    if size_of_image as usize > dst.len() {
        return Err(format!("image needs {size_of_image} bytes, slot has {}", dst.len()));
    }
    if entry >= size_of_image || size_of_headers > file.len() {
        return Err("PE header values out of range".into());
    }
    dst[..size_of_headers].copy_from_slice(&file[..size_of_headers]);
    let sec0 = opt + opt_size;
    for i in 0..nsec {
        let s = sec0 + i * 40;
        let vsize = rd32(s + 8).ok_or("truncated section table")? as usize;
        let va = rd32(s + 12).ok_or("truncated")? as usize;
        let raw_size = rd32(s + 16).ok_or("truncated")? as usize;
        let raw_ptr = rd32(s + 20).ok_or("truncated")? as usize;
        // Raw data beyond VirtualSize is file-alignment padding.
        let n = if vsize == 0 { raw_size } else { raw_size.min(vsize) };
        if va.checked_add(vsize.max(n)).is_none_or(|e| e as u64 > size_of_image) {
            return Err(format!("section {i} lies outside SizeOfImage"));
        }
        let src = file
            .get(raw_ptr..raw_ptr + n)
            .ok_or(format!("section {i} raw data outside the file"))?;
        dst[va..va + n].copy_from_slice(src);
    }
    Ok(PeImage {
        entry,
        size: size_of_image,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sum_ok(t: &[u8]) -> bool {
        t.iter().fold(0u8, |a, x| a.wrapping_add(*x)) == 0
    }

    #[test]
    fn acpi_tables_link_and_checksum() {
        let mut buf = vec![0u8; ACPI_SIZE as usize];
        let base = RAM_BASE;
        let rsdp = build_acpi(&mut buf, base, &[0x8000_0000, 0x8000_0001]).unwrap();
        let r = &buf[(rsdp - base) as usize..][..36];
        assert_eq!(&r[0..8], b"RSD PTR ");
        assert!(sum_ok(&r[0..20]) && sum_ok(r));
        let xsdt = u64::from_le_bytes(r[24..32].try_into().unwrap());
        let x = &buf[(xsdt - base) as usize..];
        assert_eq!(&x[0..4], b"XSDT");
        let xlen = u32::from_le_bytes(x[4..8].try_into().unwrap()) as usize;
        assert!(sum_ok(&x[..xlen]));
        let mut sigs = Vec::new();
        for i in 0..(xlen - 36) / 8 {
            let pa = u64::from_le_bytes(x[36 + i * 8..44 + i * 8].try_into().unwrap());
            let t = &buf[(pa - base) as usize..];
            let len = u32::from_le_bytes(t[4..8].try_into().unwrap()) as usize;
            assert!(sum_ok(&t[..len]), "{:?}", &t[0..4]);
            sigs.push(t[0..4].to_vec());
            if &t[0..4] == b"APIC" {
                // 44 header+fixed, 2 GICC, GICD, GICR.
                assert_eq!(len, 44 + 2 * 80 + 24 + 16);
                assert_eq!(t[44], 0x0b);
                assert_eq!(u64::from_le_bytes(t[44 + 68..44 + 76].try_into().unwrap()), 0x8000_0000 & 0xff00ff_ffff);
                let gicr = &t[44 + 160 + 24..];
                assert_eq!(gicr[0], 0x0e);
                assert_eq!(u32::from_le_bytes(gicr[12..16].try_into().unwrap()) as u64, 2 * GICR_STRIDE);
            }
            if &t[0..4] == b"FACP" {
                assert!(len >= 131);
                assert_eq!(u16::from_le_bytes([t[129], t[130]]), 3, "PSCI over HVC");
                let dsdt = u64::from_le_bytes(t[140..148].try_into().unwrap());
                assert_eq!(&buf[(dsdt - base) as usize..][..4], b"DSDT");
            }
            if &t[0..4] == b"SPCR" {
                assert_eq!(t[36], 3);
                assert_eq!(u64::from_le_bytes(t[44..52].try_into().unwrap()), UART_BASE);
            }
        }
        assert_eq!(sigs, vec![b"FACP".to_vec(), b"APIC".to_vec(), b"SPCR".to_vec()]);
    }

    #[test]
    fn memory_map_covers_ram_exactly() {
        let ram = 256u64 << 20;
        let m = memory_map(ram, 0x12345);
        assert_eq!(m.len() % DESC_SIZE, 0);
        let mut next = RAM_BASE;
        for d in m.chunks(DESC_SIZE) {
            let start = u64::from_le_bytes(d[8..16].try_into().unwrap());
            let pages = u64::from_le_bytes(d[24..32].try_into().unwrap());
            assert_eq!(start, next, "contiguous");
            next = start + pages * 4096;
        }
        assert_eq!(next, RAM_BASE + ram);
        let code = &m[2 * DESC_SIZE..3 * DESC_SIZE];
        assert_eq!(u32::from_le_bytes(code[0..4].try_into().unwrap()), EFI_LOADER_CODE);
        assert_eq!(u64::from_le_bytes(code[24..32].try_into().unwrap()), 0x13);
    }

    #[test]
    fn identity_map_shape() {
        let t = identity_l1(RAM_BASE + (1 << 30));
        let d0 = u64::from_le_bytes(t[0..8].try_into().unwrap());
        let d1 = u64::from_le_bytes(t[8..16].try_into().unwrap());
        let d2 = u64::from_le_bytes(t[16..24].try_into().unwrap());
        assert_eq!(d0 & 0b11, 0b01);
        assert_eq!((d0 >> 2) & 7, 0, "GiB 0 is device memory");
        assert_eq!((d1 >> 2) & 7, 1, "RAM is normal memory");
        assert_eq!(d1 & !0xfff & ((1 << 48) - 1), 1 << 30);
        assert_ne!(d2, 0);
        assert_eq!(u64::from_le_bytes(t[24..32].try_into().unwrap()), 0);
        assert_eq!(tcr(2) & 0x3f, 25);
        assert_eq!((tcr(2) >> 32) & 7, 2);
    }

    #[test]
    fn record_layout_matches_direct_boot_h() {
        let r = direct_record(0x4000_0000, 0x4001_0040, 160, 0x4001_0100);
        assert_eq!(&r[0..8], b"AIENDIRB");
        assert_eq!(u32::from_le_bytes(r[8..12].try_into().unwrap()), 1);
        assert_eq!(u64::from_le_bytes(r[16..24].try_into().unwrap()), 0x4000_0000);
        assert_eq!(u64::from_le_bytes(r[40..48].try_into().unwrap()), 40);
        assert_eq!(u64::from_le_bytes(r[48..56].try_into().unwrap()), 0x4001_0100);
    }

    fn tiny_pe() -> Vec<u8> {
        let mut f = vec![0u8; 0x400];
        f[0..2].copy_from_slice(b"MZ");
        put32(&mut f, 0x3c, 0x40);
        f[0x40..0x44].copy_from_slice(b"PE\0\0");
        put16(&mut f, 0x44, 0xaa64);
        put16(&mut f, 0x46, 1);
        put16(&mut f, 0x54, 240);
        let opt = 0x58;
        put16(&mut f, opt, 0x20b);
        put32(&mut f, opt + 16, 0x200);
        put32(&mut f, opt + 56, 0x2000);
        put32(&mut f, opt + 60, 0x200);
        let s = opt + 240;
        f[s..s + 5].copy_from_slice(b".text");
        put32(&mut f, s + 8, 0x1000);
        put32(&mut f, s + 12, 0x200);
        put32(&mut f, s + 16, 0x200);
        put32(&mut f, s + 20, 0x200);
        f[0x200] = 0xaa;
        f[0x3ff] = 0xbb;
        f
    }

    #[test]
    fn pe_loader() {
        let f = tiny_pe();
        let mut dst = vec![0u8; 0x2000];
        let img = load_pe(&f, &mut dst).unwrap();
        assert_eq!((img.entry, img.size), (0x200, 0x2000));
        assert_eq!(dst[0x200], 0xaa);
        assert_eq!(dst[0x3ff], 0xbb);
        assert_eq!(&dst[0..2], b"MZ");
        let mut small = vec![0u8; 0x1000];
        assert!(load_pe(&f, &mut small).is_err());
        let mut x86 = f.clone();
        put16(&mut x86, 0x44, 0x8664);
        assert!(load_pe(&x86, &mut dst).is_err());
        assert!(load_pe(b"hello", &mut dst).is_err());
    }
}
