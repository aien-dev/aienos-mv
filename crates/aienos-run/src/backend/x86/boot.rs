//! The state a UEFI application expects at entry (UEFI 2.10 section 2.3.4, x64): long
//! mode, identity-mapped paging, a flat GDT, interrupts off, the image loaded and
//! relocated, RCX = ImageHandle, RDX = SystemTable, a 16-byte-aligned stack (RSP+8) with
//! the return address on top. [`prepare`] writes all of it into guest memory and returns
//! the register values for the hypervisor.

use super::acpi;
use super::mem::{
    GuestMem, GDT_BASE, IMAGE_BASE, LOW_RAM_MAX, PAGE, PT_BASE, PT_PAGES, STACK_TOP, TSS_BASE,
};
use super::uefi::{self, Uefi, APP_RETURNED, IMAGE_HANDLE};

pub const SEL_CODE: u16 = 0x08;
pub const SEL_DATA: u16 = 0x10;
pub const SEL_TSS: u16 = 0x18;
const GDT_ENTRIES: u64 = 5; // null, code, data, TSS (two slots)

/// Segment register with its hidden part. `attr` uses the hypervisor layout (WHPX
/// WHV_X64_SEGMENT_REGISTER, VMX access rights): type 0..3, S 4, DPL 5..6, P 7, AVL 12,
/// L 13, D/B 14, G 15.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Seg {
    pub base: u64,
    pub limit: u32,
    pub selector: u16,
    pub attr: u16,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Dtr {
    pub base: u64,
    pub limit: u16,
}

/// Every register the VMM sets before the first instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CpuState {
    pub rip: u64,
    pub rsp: u64,
    pub rcx: u64,
    pub rdx: u64,
    pub rflags: u64,
    pub cr0: u64,
    pub cr3: u64,
    pub cr4: u64,
    pub efer: u64,
    pub cs: Seg,
    pub ds: Seg,
    pub es: Seg,
    pub fs: Seg,
    pub gs: Seg,
    pub ss: Seg,
    pub tr: Seg,
    pub ldtr: Seg,
    pub gdtr: Dtr,
    pub idtr: Dtr,
}

pub const ATTR_CODE64: u16 = 0xa09b; // execute/read accessed, S, P, L, G
pub const ATTR_DATA: u16 = 0xc093; // read/write accessed, S, P, D/B, G
pub const ATTR_TSS64_BUSY: u16 = 0x008b;
pub const ATTR_LDT: u16 = 0x0082;
pub const CR0_PE_MP_ET_NE_WP_PG: u64 = 0x8001_0033;
pub const CR4_PAE_OSFXSR_OSXMMEXCPT: u64 = 0x620;
pub const EFER_LME_LMA: u64 = 0x500;

impl CpuState {
    pub fn long_mode(rip: u64, rsp: u64, rcx: u64, rdx: u64, cr3: u64) -> CpuState {
        let data = Seg {
            base: 0,
            limit: 0xffff_ffff,
            selector: SEL_DATA,
            attr: ATTR_DATA,
        };
        CpuState {
            rip,
            rsp,
            rcx,
            rdx,
            rflags: 0x2,
            cr0: CR0_PE_MP_ET_NE_WP_PG,
            cr3,
            cr4: CR4_PAE_OSFXSR_OSXMMEXCPT,
            efer: EFER_LME_LMA,
            cs: Seg {
                selector: SEL_CODE,
                attr: ATTR_CODE64,
                ..data
            },
            ds: data,
            es: data,
            fs: data,
            gs: data,
            ss: data,
            tr: Seg {
                base: TSS_BASE,
                limit: 0x67,
                selector: SEL_TSS,
                attr: ATTR_TSS64_BUSY,
            },
            ldtr: Seg {
                base: 0,
                limit: 0xffff,
                selector: 0,
                attr: ATTR_LDT,
            },
            gdtr: Dtr {
                base: GDT_BASE,
                limit: (GDT_ENTRIES * 8 - 1) as u16,
            },
            idtr: Dtr { base: 0, limit: 0 },
        }
    }
}

/// The GDT: the descriptors behind the hidden segment state of [`CpuState::long_mode`].
pub fn gdt() -> [u64; GDT_ENTRIES as usize] {
    let tss_lo = 0x67 | (TSS_BASE & 0xff_ffff) << 16 | 0x8b << 40 | (TSS_BASE >> 24 & 0xff) << 56;
    [
        0,
        0x00af_9b00_0000_ffff,
        0x00cf_9300_0000_ffff,
        tss_lo,
        TSS_BASE >> 32,
    ]
}

/// Identity map of [0, max(4 GiB, top)) with 2 MiB pages; returns CR3.
pub fn page_tables(mem: &mut GuestMem) -> Result<u64, String> {
    let gib = mem.top().max(1 << 32).div_ceil(1 << 30);
    if 2 + gib > PT_PAGES {
        return Err(format!(
            "{gib} GiB of address space needs more page-table pages"
        ));
    }
    let (pml4, pdpt) = (PT_BASE, PT_BASE + PAGE);
    mem.write(PT_BASE, &vec![0u8; (PT_PAGES * PAGE) as usize])?;
    mem.write_u64(pml4, pdpt | 0x3)?;
    for g in 0..gib {
        let pd = PT_BASE + (2 + g) * PAGE;
        mem.write_u64(pdpt + 8 * g, pd | 0x3)?;
        for i in 0..512 {
            mem.write_u64(pd + 8 * i, (g << 30 | i << 21) | 0x83)?; // P, W, PS
        }
    }
    Ok(pml4)
}

#[derive(Debug)]
pub struct Loaded {
    /// Guest address of the entry point.
    pub entry: u64,
    pub size: u64,
}

/// Loads a PE32+ x86_64 UEFI application at `base`: headers, sections at their RVAs (the
/// rest zero), then the base relocations (DIR64, HIGHLOW) for the distance from the
/// linked ImageBase, as firmware does (PE/COFF spec, "The .reloc Section").
pub fn load_pe(mem: &mut GuestMem, file: &[u8], base: u64) -> Result<Loaded, String> {
    let rd16 = |o: usize| file.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]));
    let rd32 = |o: usize| {
        file.get(o..o + 4)
            .map(|b| u32::from_le_bytes(b.try_into().expect("4 bytes")))
    };
    let rd64 = |o: usize| {
        file.get(o..o + 8)
            .map(|b| u64::from_le_bytes(b.try_into().expect("8 bytes")))
    };
    let t = "truncated PE header";
    if file.get(..2) != Some(b"MZ") {
        return Err("not a PE image (no MZ)".into());
    }
    let pe = rd32(0x3c).ok_or(t)? as usize;
    if file.get(pe..pe + 4) != Some(b"PE\0\0") {
        return Err("no PE signature".into());
    }
    let machine = rd16(pe + 4).ok_or(t)?;
    if machine != 0x8664 {
        return Err(format!("PE machine 0x{machine:x} is not x86_64 (0x8664)"));
    }
    let nsec = rd16(pe + 6).ok_or(t)? as usize;
    let opt_size = rd16(pe + 20).ok_or(t)? as usize;
    let opt = pe + 24;
    if rd16(opt) != Some(0x20b) {
        return Err("not PE32+".into());
    }
    let entry = u64::from(rd32(opt + 16).ok_or(t)?);
    let image_base = rd64(opt + 24).ok_or(t)?;
    let size_of_image = u64::from(rd32(opt + 56).ok_or(t)?);
    let size_of_headers = rd32(opt + 60).ok_or(t)? as usize;
    let subsystem = rd16(opt + 68).ok_or(t)?;
    if !(10..=12).contains(&subsystem) {
        return Err(format!("PE subsystem {subsystem} is not a UEFI image"));
    }
    if base + size_of_image > LOW_RAM_MAX || !mem.is_ram(base, size_of_image as usize) {
        return Err(format!(
            "image of {size_of_image} bytes does not fit in guest RAM"
        ));
    }
    if entry >= size_of_image
        || size_of_headers as u64 > size_of_image
        || size_of_headers > file.len()
    {
        return Err("PE header values out of range".into());
    }
    let img = mem
        .slice_mut(base, size_of_image as usize)
        .ok_or("image outside RAM")?;
    img.fill(0);
    img[..size_of_headers].copy_from_slice(&file[..size_of_headers]);
    let sec0 = opt + opt_size;
    for i in 0..nsec {
        let s = sec0 + i * 40;
        let ts = "truncated section table";
        let vsize = rd32(s + 8).ok_or(ts)? as usize;
        let va = rd32(s + 12).ok_or(ts)? as usize;
        let raw_size = rd32(s + 16).ok_or(ts)? as usize;
        let raw_ptr = rd32(s + 20).ok_or(ts)? as usize;
        let n = if vsize == 0 {
            raw_size
        } else {
            raw_size.min(vsize)
        };
        if va
            .checked_add(vsize.max(n))
            .is_none_or(|e| e as u64 > size_of_image)
        {
            return Err(format!("section {i} lies outside SizeOfImage"));
        }
        let src = raw_ptr
            .checked_add(n)
            .and_then(|e| file.get(raw_ptr..e))
            .ok_or(format!("section {i} raw data outside the file"))?;
        img[va..va + n].copy_from_slice(src);
    }
    // Base relocations: data directory 5.
    let ndirs = rd32(opt + 108).ok_or(t)?;
    let delta = base.wrapping_sub(image_base);
    if ndirs > 5 && delta != 0 {
        let rva = rd32(opt + 152).ok_or(t)? as usize;
        let size = rd32(opt + 156).ok_or(t)? as usize;
        let end = rva.checked_add(size).filter(|e| *e as u64 <= size_of_image);
        let end = end.ok_or("relocation directory outside the image")?;
        let mut blk = rva;
        while blk + 8 <= end {
            let page = u32::from_le_bytes(img[blk..blk + 4].try_into().expect("4")) as usize;
            let bsize = u32::from_le_bytes(img[blk + 4..blk + 8].try_into().expect("4")) as usize;
            if bsize < 8 || blk + bsize > end {
                return Err("bad relocation block".into());
            }
            for e in img[blk + 8..blk + bsize].to_vec().as_chunks::<2>().0 {
                let e = u16::from_le_bytes(*e);
                let at = page + usize::from(e & 0xfff);
                match e >> 12 {
                    0 => {}
                    10 if at + 8 <= img.len() => {
                        let v = u64::from_le_bytes(img[at..at + 8].try_into().expect("8"));
                        img[at..at + 8].copy_from_slice(&v.wrapping_add(delta).to_le_bytes());
                    }
                    3 if at + 4 <= img.len() => {
                        let v = u32::from_le_bytes(img[at..at + 4].try_into().expect("4"));
                        img[at..at + 4]
                            .copy_from_slice(&v.wrapping_add(delta as u32).to_le_bytes());
                    }
                    k => return Err(format!("relocation type {k} at rva 0x{at:x} not supported")),
                }
            }
            blk += bsize;
        }
    }
    Ok(Loaded {
        entry: base + entry,
        size: size_of_image,
    })
}

/// Everything the VMM needs to start the vCPU and answer UEFI calls.
pub struct Boot {
    pub cpu: CpuState,
    pub uefi: Uefi,
    pub rsdp: u64,
    pub image_size: u64,
}

pub fn prepare(mem: &mut GuestMem, efi: &[u8], cpus: u32) -> Result<Boot, String> {
    let g: Vec<u8> = gdt().iter().flat_map(|d| d.to_le_bytes()).collect();
    mem.write(GDT_BASE, &g)?;
    let mut tss = [0u8; 104];
    tss[102..104].copy_from_slice(&104u16.to_le_bytes()); // I/O map base: none
    mem.write(TSS_BASE, &tss)?;
    let cr3 = page_tables(mem)?;
    let rsdp = acpi::build(mem, cpus)?;
    let st = uefi::install(mem, rsdp)?;
    let img = load_pe(mem, efi, IMAGE_BASE)?;
    let rsp = STACK_TOP - 0x38; // 8 mod 16, as after a call; 32 bytes of shadow space above
    mem.write_u64(rsp, uefi::stub_addr(APP_RETURNED))?;
    let map = uefi::memory_map(&mem.ranges(), img.size);
    Ok(Boot {
        cpu: CpuState::long_mode(img.entry, rsp, IMAGE_HANDLE, st, cr3),
        uefi: Uefi::new(map),
        rsdp,
        image_size: img.size,
    })
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::backend::x86::mem::MIN_RAM;

    /// A minimal PE32+ x86_64 EFI application: one section of `code` at RVA 0x1000 and,
    /// when `reloc_at` is set, one DIR64 relocation at that RVA. Linked at `image_base`.
    pub(crate) fn tiny_pe(code: &[u8], image_base: u64, reloc_at: Option<u32>) -> Vec<u8> {
        let mut f = vec![0u8; 0x400];
        f[..2].copy_from_slice(b"MZ");
        f[0x3c..0x40].copy_from_slice(&0x80u32.to_le_bytes());
        let pe = 0x80;
        f[pe..pe + 4].copy_from_slice(b"PE\0\0");
        f[pe + 4..pe + 6].copy_from_slice(&0x8664u16.to_le_bytes());
        let nsec: u16 = if reloc_at.is_some() { 2 } else { 1 };
        f[pe + 6..pe + 8].copy_from_slice(&nsec.to_le_bytes());
        f[pe + 20..pe + 22].copy_from_slice(&240u16.to_le_bytes());
        let o = pe + 24;
        f[o..o + 2].copy_from_slice(&0x20bu16.to_le_bytes());
        f[o + 16..o + 20].copy_from_slice(&0x1000u32.to_le_bytes()); // entry
        f[o + 24..o + 32].copy_from_slice(&image_base.to_le_bytes());
        f[o + 56..o + 60].copy_from_slice(&0x3000u32.to_le_bytes()); // SizeOfImage
        f[o + 60..o + 64].copy_from_slice(&0x400u32.to_le_bytes()); // SizeOfHeaders
        f[o + 68..o + 70].copy_from_slice(&10u16.to_le_bytes()); // EFI application
        f[o + 108..o + 112].copy_from_slice(&16u32.to_le_bytes());
        let s = o + 240;
        // .text: vsize, va, raw size, raw ptr
        for (k, v) in [
            (8, code.len() as u32),
            (12, 0x1000),
            (16, code.len() as u32),
            (20, 0x400),
        ] {
            f[s + k..s + k + 4].copy_from_slice(&v.to_le_bytes());
        }
        f.extend_from_slice(code);
        if let Some(at) = reloc_at {
            f[o + 152..o + 156].copy_from_slice(&0x2000u32.to_le_bytes());
            f[o + 156..o + 160].copy_from_slice(&12u32.to_le_bytes());
            let raw = f.len() as u32;
            let s2 = s + 40;
            for (k, v) in [(8, 12u32), (12, 0x2000), (16, 12), (20, raw)] {
                f[s2 + k..s2 + k + 4].copy_from_slice(&v.to_le_bytes());
            }
            f.extend_from_slice(&(at & !0xfff).to_le_bytes());
            f.extend_from_slice(&12u32.to_le_bytes());
            f.extend_from_slice(&(0xa000u16 | (at & 0xfff) as u16).to_le_bytes());
            f.extend_from_slice(&0u16.to_le_bytes()); // ABSOLUTE padding
        }
        f
    }

    /// 4-level walk honouring 2 MiB pages.
    fn translate(m: &GuestMem, cr3: u64, va: u64) -> Option<u64> {
        let mut table = cr3 & !0xfff;
        for (level, shift) in [(4, 39), (3, 30), (2, 21), (1, 12)] {
            let e = m.read_u64(table + 8 * ((va >> shift) & 511))?;
            if e & 1 == 0 {
                return None;
            }
            if level == 2 && e & 0x80 != 0 {
                return Some((e & 0x000f_ffff_ffe0_0000) | (va & 0x1f_ffff));
            }
            table = e & 0x000f_ffff_ffff_f000;
        }
        Some(table | (va & 0xfff))
    }

    #[test]
    fn identity_map_covers_ram_and_the_apic_hole() {
        let mut m = GuestMem::new(MIN_RAM).unwrap();
        let cr3 = page_tables(&mut m).unwrap();
        for va in [
            0,
            0x1234,
            IMAGE_BASE + 5,
            0xfec0_0010,
            0xfee0_0300,
            (4 << 30) - 1,
        ] {
            assert_eq!(translate(&m, cr3, va), Some(va), "0x{va:x}");
        }
        assert_eq!(translate(&m, cr3, 4 << 30), None);
        let mut big = GuestMem::new(8 << 30).unwrap();
        let cr3 = page_tables(&mut big).unwrap();
        let top = big.top() - 1;
        assert_eq!(translate(&big, cr3, top), Some(top));
    }

    #[test]
    fn gdt_matches_the_hidden_segment_state() {
        let g = gdt();
        let c = CpuState::long_mode(0, 0, 0, 0, 0);
        let attr = |d: u64| ((d >> 40) & 0xf0ff) as u16;
        assert_eq!(attr(g[(c.cs.selector / 8) as usize]), c.cs.attr);
        assert_eq!(attr(g[(c.ss.selector / 8) as usize]), c.ss.attr);
        let t = g[(c.tr.selector / 8) as usize];
        assert_eq!(attr(t), c.tr.attr);
        let base = (t >> 16 & 0xff_ffff) | (t >> 56 & 0xff) << 24 | g[4] << 32;
        assert_eq!(base, c.tr.base);
        assert_eq!(u64::from(c.gdtr.limit) + 1, 8 * g.len() as u64);
        assert_eq!(c.cs.attr & 1 << 13, 1 << 13, "64-bit code");
    }

    #[test]
    fn pe_loads_at_base_and_relocates() {
        let mut m = GuestMem::new(MIN_RAM).unwrap();
        // code: 8 bytes holding the linked address of rva 0x1000 (ImageBase 0)
        let code = 0x1000u64.to_le_bytes();
        let f = tiny_pe(&code, 0, Some(0x1000));
        let l = load_pe(&mut m, &f, IMAGE_BASE).unwrap();
        assert_eq!(l.entry, IMAGE_BASE + 0x1000);
        assert_eq!(l.size, 0x3000);
        assert_eq!(m.slice(IMAGE_BASE, 2).unwrap(), b"MZ");
        assert_eq!(m.read_u64(IMAGE_BASE + 0x1000), Some(IMAGE_BASE + 0x1000));
        // linked where it is loaded: nothing to relocate
        let f = tiny_pe(&code, IMAGE_BASE, Some(0x1000));
        load_pe(&mut m, &f, IMAGE_BASE).unwrap();
        assert_eq!(m.read_u64(IMAGE_BASE + 0x1000), Some(0x1000));
    }

    #[test]
    fn pe_refusals() {
        let mut m = GuestMem::new(MIN_RAM).unwrap();
        let good = tiny_pe(&[0xc3], 0, None);
        let mut arm = good.clone();
        arm[0x84..0x86].copy_from_slice(&0xaa64u16.to_le_bytes());
        assert!(load_pe(&mut m, &arm, IMAGE_BASE)
            .unwrap_err()
            .contains("not x86_64"));
        assert!(load_pe(&mut m, b"ELF", IMAGE_BASE).is_err());
        let mut app = good.clone();
        app[0x80 + 24 + 68] = 2; // Windows GUI subsystem
        assert!(load_pe(&mut m, &app, IMAGE_BASE)
            .unwrap_err()
            .contains("subsystem"));
        let mut short = good.clone();
        short.truncate(0x400); // section data missing
        assert!(load_pe(&mut m, &short, IMAGE_BASE)
            .unwrap_err()
            .contains("outside the file"));
        assert!(
            load_pe(&mut m, &good, MIN_RAM - 0x1000).is_err(),
            "does not fit"
        );
    }

    #[test]
    fn prepare_enters_like_firmware() {
        let mut m = GuestMem::new(MIN_RAM).unwrap();
        let f = tiny_pe(&[0xf4], 0, None); // hlt
        let b = prepare(&mut m, &f, 1).unwrap();
        let c = b.cpu;
        assert_eq!(c.rip, IMAGE_BASE + 0x1000);
        assert_eq!(c.rcx, IMAGE_HANDLE);
        assert_eq!(c.rdx, uefi::ST);
        assert_eq!(c.rsp % 16, 8);
        assert_eq!(m.read_u64(c.rsp), Some(uefi::stub_addr(APP_RETURNED)));
        assert_eq!(translate(&m, c.cr3, c.rip), Some(c.rip));
        assert_eq!(c.efer & 0x500, 0x500);
        assert_eq!(c.cr0 & (1 << 31 | 1), 1 << 31 | 1);
        assert_eq!(c.rflags & 1 << 9, 0, "interrupts off");
        assert_eq!(m.read_u64(uefi::CONFIG + 16), Some(b.rsdp));
        assert_eq!(m.slice(b.rsdp, 8).unwrap(), b"RSD PTR ");
    }
}
