//! ACPI tables for the own x86 VMM: RSDP 2.0, XSDT, FADT 6 (PM1a ports, reset register,
//! X_DSDT), DSDT holding only `Name(_S5, Package(4){5, 5, 0, 0})`, MADT (local APIC per
//! vCPU, one I/O APIC, IRQ0 -> GSI2) and HPET.
//!
//! Layouts: ACPI 6.5 sections 5.2.5 (RSDP), 5.2.6 (header), 5.2.8 (XSDT), 5.2.9 (FADT),
//! 5.2.12 (MADT), 7.4.2 (\_S5); IA-PC HPET 1.0a section 3.2.4. The kernel's readers
//! (native/kernel/core/acpi.c ck_acpi_lookup, native/kernel/x86/kmain_x86.c acpi_poweroff,
//! s5_types, madt_walk, tsc_measure) decide which fields matter; the tests below read the
//! tables back the same way.

use super::devices::{
    HPET_BASE, IOAPIC_BASE, LAPIC_BASE, PM1A_CNT, PM1A_EVT, RESET_PORT, RESET_VALUE, S5_TYP,
};
use super::mem::{GuestMem, ACPI_BASE, ACPI_END};

const RSDP_OFF: u64 = 0;
const XSDT_OFF: u64 = 0x40;
const FADT_OFF: u64 = 0x100;
const DSDT_OFF: u64 = 0x300;
const MADT_OFF: u64 = 0x400;
const HPET_OFF: u64 = 0x1000;

fn put16(b: &mut [u8], at: usize, v: u16) {
    b[at..at + 2].copy_from_slice(&v.to_le_bytes());
}
fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}
fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

/// The byte that makes `b` sum to zero.
pub fn checksum(b: &[u8]) -> u8 {
    0u8.wrapping_sub(b.iter().fold(0u8, |a, x| a.wrapping_add(*x)))
}

/// A table: 36-byte header (OEM "AIENOS", table id "AIENRUN ") then `body`.
fn sdt(sig: &[u8; 4], revision: u8, body: &[u8]) -> Vec<u8> {
    let mut t = vec![0u8; 36 + body.len()];
    t[0..4].copy_from_slice(sig);
    let len = t.len() as u32;
    put32(&mut t, 4, len);
    t[8] = revision;
    t[10..16].copy_from_slice(b"AIENOS");
    t[16..24].copy_from_slice(b"AIENRUN ");
    put32(&mut t, 24, 1);
    t[28..32].copy_from_slice(b"AIEN");
    put32(&mut t, 32, 1);
    t[36..].copy_from_slice(body);
    t[9] = checksum(&t);
    t
}

/// Generic Address Structure (12 bytes): space 0 = memory, 1 = I/O port.
fn gas(b: &mut [u8], at: usize, space: u8, bits: u8, access: u8, addr: u64) {
    b[at] = space;
    b[at + 1] = bits;
    b[at + 2] = 0;
    b[at + 3] = access;
    put64(b, at + 4, addr);
}

pub fn dsdt() -> Vec<u8> {
    // NameOp "_S5_" PackageOp PkgLength(8) NumElements(4) BytePrefix 5, BytePrefix 5, 0, 0
    let aml = [
        0x08, b'_', b'S', b'5', b'_', 0x12, 0x08, 0x04, 0x0a, S5_TYP, 0x0a, S5_TYP, 0x00, 0x00,
    ];
    sdt(b"DSDT", 2, &aml)
}

pub fn fadt(dsdt: u64) -> Vec<u8> {
    let mut b = vec![0u8; 276 - 36];
    let f = |off: usize| off - 36; // body offsets from the spec's table offsets
    put32(&mut b, f(40), dsdt as u32);
    put16(&mut b, f(46), 9); // SCI_INT
    put32(&mut b, f(56), PM1A_EVT as u32);
    put32(&mut b, f(64), PM1A_CNT as u32);
    b[f(88)] = 4; // PM1_EVT_LEN
    b[f(89)] = 2; // PM1_CNT_LEN
    put32(&mut b, f(112), 1 | 1 << 10); // WBINVD, RESET_REG_SUP
    gas(&mut b, f(116), 1, 8, 1, RESET_PORT as u64);
    b[f(128)] = RESET_VALUE;
    b[f(131)] = 5; // FADT minor version (ACPI 6.5)
    put64(&mut b, f(140), dsdt);
    gas(&mut b, f(148), 1, 32, 3, PM1A_EVT as u64);
    gas(&mut b, f(172), 1, 16, 2, PM1A_CNT as u64);
    sdt(b"FACP", 6, &b)
}

pub fn madt(cpus: u32) -> Vec<u8> {
    let mut b = Vec::new();
    b.extend_from_slice(&(LAPIC_BASE as u32).to_le_bytes());
    b.extend_from_slice(&1u32.to_le_bytes()); // PCAT_COMPAT
    for i in 0..cpus {
        // Processor Local APIC: uid, APIC id, flags (enabled)
        b.extend_from_slice(&[0, 8, i as u8, i as u8, 1, 0, 0, 0]);
    }
    // I/O APIC: id, reserved, address, GSI base
    b.extend_from_slice(&[1, 12, cpus as u8, 0]);
    b.extend_from_slice(&(IOAPIC_BASE as u32).to_le_bytes());
    b.extend_from_slice(&0u32.to_le_bytes());
    // Interrupt source override: ISA IRQ0 -> GSI 2, conforming polarity and trigger
    b.extend_from_slice(&[2, 10, 0, 0, 2, 0, 0, 0, 0, 0]);
    sdt(b"APIC", 5, &b)
}

pub fn hpet() -> Vec<u8> {
    let mut b = vec![0u8; 56 - 36];
    // Event timer block id: revision 1, 3 comparators, 64-bit counter, legacy route, Intel
    put32(&mut b, 0, 1 | 2 << 8 | 1 << 13 | 1 << 15 | 0x8086 << 16);
    gas(&mut b, 4, 0, 64, 0, HPET_BASE);
    put16(&mut b, 17, 0x80); // minimum clock tick
    sdt(b"HPET", 1, &b)
}

pub fn xsdt(tables: &[u64]) -> Vec<u8> {
    let body: Vec<u8> = tables.iter().flat_map(|a| a.to_le_bytes()).collect();
    sdt(b"XSDT", 1, &body)
}

pub fn rsdp(xsdt: u64) -> [u8; 36] {
    let mut r = [0u8; 36];
    r[0..8].copy_from_slice(b"RSD PTR ");
    r[9..15].copy_from_slice(b"AIENOS");
    r[15] = 2;
    put32(&mut r, 20, 36);
    put64(&mut r, 24, xsdt);
    r[8] = checksum(&r[..20]);
    r[32] = checksum(&r);
    r
}

/// Writes every table into the ACPI region; returns the RSDP address.
pub fn build(mem: &mut GuestMem, cpus: u32) -> Result<u64, String> {
    let at = |off: u64| ACPI_BASE + off;
    let tables: [(u64, Vec<u8>); 4] = [
        (FADT_OFF, fadt(at(DSDT_OFF))),
        (DSDT_OFF, dsdt()),
        (MADT_OFF, madt(cpus)),
        (HPET_OFF, hpet()),
    ];
    for (off, t) in &tables {
        if at(*off) + t.len() as u64 > ACPI_END {
            return Err("ACPI tables overflow their region".into());
        }
        mem.write(at(*off), t)?;
    }
    mem.write(
        at(XSDT_OFF),
        &xsdt(&[at(FADT_OFF), at(MADT_OFF), at(HPET_OFF)]),
    )?;
    mem.write(at(RSDP_OFF), &rsdp(at(XSDT_OFF)))?;
    Ok(at(RSDP_OFF))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rd32(b: &[u8], o: usize) -> u32 {
        u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
    }
    fn rd64(b: &[u8], o: usize) -> u64 {
        u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
    }
    fn sum_ok(b: &[u8]) -> bool {
        b.iter().fold(0u8, |a, x| a.wrapping_add(*x)) == 0
    }

    /// native/kernel/core/acpi.c ck_acpi_lookup, over guest memory.
    fn lookup<'a>(m: &'a GuestMem, rsdp: u64, sig: &[u8; 4]) -> Option<&'a [u8]> {
        let r = m.slice(rsdp, 36)?;
        if &r[..8] != b"RSD PTR " || !sum_ok(&r[..20]) || r[15] < 2 || !sum_ok(r) {
            return None;
        }
        let x = rd64(r, 24);
        let root = m.slice(x, rd32(m.slice(x, 8)?, 4) as usize)?;
        if &root[..4] != b"XSDT" || !sum_ok(root) {
            return None;
        }
        (36..root.len()).step_by(8).find_map(|o| {
            let a = rd64(root, o);
            let t = m.slice(a, rd32(m.slice(a, 8)?, 4) as usize)?;
            (&t[..4] == sig && sum_ok(t)).then_some(t)
        })
    }

    /// native/kernel/x86/kmain_x86.c s5_types.
    fn s5_types(d: &[u8]) -> Option<(u8, u8)> {
        let len = d.len();
        let mut i = 36;
        while i + 12 < len {
            if &d[i..i + 4] == b"_S5_" && d[i + 4] == 0x12 {
                let mut p = i + 5;
                p += 1 + (d[p] >> 6) as usize;
                p += 1;
                let mut v = [0u8; 2];
                for x in &mut v {
                    let op = d[p];
                    p += 1;
                    *x = match op {
                        0 | 1 => op,
                        0xff => 7,
                        0x0a => {
                            p += 1;
                            d[p - 1] & 7
                        }
                        _ => return None,
                    };
                }
                return Some((v[0], v[1]));
            }
            i += 1;
        }
        None
    }

    #[test]
    fn kernel_finds_every_table_and_the_s5_types() {
        let mut m = GuestMem::new(64 << 20).unwrap();
        let rsdp = build(&mut m, 1).unwrap();
        let fadt = lookup(&m, rsdp, b"FACP").expect("FADT");
        assert_eq!(fadt.len(), 276);
        assert_eq!(rd32(fadt, 64), PM1A_CNT as u32);
        assert_eq!(
            rd32(fadt, 48),
            0,
            "no SMI command port: ACPI mode is always on"
        );
        let dsdt_at = rd64(fadt, 140);
        assert_eq!(dsdt_at, rd32(fadt, 40) as u64);
        let dsdt = m
            .slice(dsdt_at, rd32(m.slice(dsdt_at, 8).unwrap(), 4) as usize)
            .unwrap();
        assert!(sum_ok(dsdt));
        assert_eq!(s5_types(dsdt), Some((S5_TYP, S5_TYP)));
        assert!(
            lookup(&m, rsdp, b"DSDT").is_none(),
            "DSDT is reached through the FADT"
        );

        let madt = lookup(&m, rsdp, b"APIC").expect("MADT");
        assert_eq!(rd32(madt, 36), LAPIC_BASE as u32);
        // madt_walk: one enabled local APIC, one I/O APIC at IOAPIC_BASE
        let (mut cpus, mut ioapics, mut off) = (0, 0, 44);
        while off + 2 <= madt.len() {
            let (ty, el) = (madt[off], madt[off + 1] as usize);
            assert!(el >= 2 && off + el <= madt.len());
            if ty == 0 && rd32(madt, off + 4) & 3 != 0 {
                cpus += 1;
            }
            if ty == 1 {
                ioapics += 1;
                assert_eq!(rd32(madt, off + 4), IOAPIC_BASE as u32);
            }
            off += el;
        }
        assert_eq!((cpus, ioapics, off), (1, 1, madt.len()));

        let hpet = lookup(&m, rsdp, b"HPET").expect("HPET");
        assert!(rd32(hpet, 4) >= 56 && hpet[40] == 0, "tsc_measure's checks");
        assert_eq!(rd64(hpet, 44), HPET_BASE);
    }

    #[test]
    fn checksums_and_rsdp() {
        let r = rsdp(0x1234_5678_9abc);
        assert!(sum_ok(&r[..20]) && sum_ok(&r));
        for t in [dsdt(), fadt(1), madt(4), hpet(), xsdt(&[1, 2])] {
            assert!(sum_ok(&t), "{:?}", &t[..4]);
            assert_eq!(rd32(&t, 4) as usize, t.len());
        }
        let m = madt(4);
        assert_eq!(m.len(), 44 + 4 * 8 + 12 + 10);
    }
}
