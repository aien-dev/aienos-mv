//! The UEFI shim: just enough of UEFI 2.10 for an AienOS kernel to start, answered by the
//! VMM instead of firmware code.
//!
//! Guest memory holds a system table, boot services (44 entries), runtime services (14),
//! a text console (ConOut, ConIn) and one configuration table entry (the ACPI 2.0 RSDP).
//! Every function pointer leads to a 16-byte stub
//!
//! ```text
//!     41 ba <id:u32>    mov r10d, id      ; R10 is volatile in the Microsoft x64 ABI
//!     e6 eb             out 0xeb, al      ; exits to the VMM
//!     c3                ret
//! ```
//!
//! On the `out` exit the VMM reads the call id from R10 and the arguments from RCX, RDX,
//! R8, R9 and the stack (the fifth at RSP+0x28: the stub has not moved RSP), writes the
//! result into RAX and steps over the `out`. Implemented: GetMemoryMap, ExitBootServices,
//! SetWatchdogTimer, Stall, AllocatePool/FreePool (an arena), CopyMem, SetMem,
//! CalculateCrc32, RaiseTPL/RestoreTPL, Exit, LocateHandleBuffer/LocateProtocol (nothing
//! is found: no GOP), ConOut.OutputString (to the console), ConIn.ReadKeyStroke (from
//! the keyboard) and ResetSystem. Everything else returns EFI_UNSUPPORTED and is listed
//! in [`Uefi::unsupported`]. Offsets: native/boot/efi.h (its static asserts) and UEFI
//! 2.10 sections 4.3 to 4.5, 7, 8, 12.3/12.4.

use super::devices::Serial;
use super::mem::{
    GuestMem, ACPI_BASE, ACPI_END, FW_BASE, IMAGE_BASE, LOW_FREE_END, PAGE, POOL_BASE, POOL_END,
    SHIM_BASE, STACK_TOP, STUB_BASE, STUB_END,
};
use std::collections::BTreeSet;

pub const CALL_PORT: u16 = 0xeb;
pub const STUB_BYTES: u64 = 16;

pub const BS_FIRST: u32 = 0;
pub const BS_COUNT: u32 = 44;
pub const RT_FIRST: u32 = 64;
pub const RT_COUNT: u32 = 14;
pub const CONOUT_FIRST: u32 = 96;
pub const CONOUT_COUNT: u32 = 9;
pub const CONIN_FIRST: u32 = 112;
pub const CONIN_COUNT: u32 = 2;
/// The return address efi_main finds on its stack: returning ends the run.
pub const APP_RETURNED: u32 = 127;
pub const STUBS: u32 = 128;

// Boot services (index into the table).
const BS_RAISE_TPL: u32 = 0;
const BS_RESTORE_TPL: u32 = 1;
const BS_GET_MEMORY_MAP: u32 = 4;
const BS_ALLOCATE_POOL: u32 = 5;
const BS_FREE_POOL: u32 = 6;
const BS_EXIT: u32 = 24;
const BS_EXIT_BOOT_SERVICES: u32 = 26;
const BS_STALL: u32 = 28;
const BS_SET_WATCHDOG: u32 = 29;
const BS_LOCATE_HANDLE_BUFFER: u32 = 36;
const BS_LOCATE_PROTOCOL: u32 = 37;
const BS_CALCULATE_CRC32: u32 = 40;
const BS_COPY_MEM: u32 = 41;
const BS_SET_MEM: u32 = 42;
const RT_RESET_SYSTEM: u32 = 10;
const CONOUT_OUTPUT_STRING: u32 = 1;
const CONOUT_QUERY_MODE: u32 = 3;
const CONIN_READ_KEY: u32 = 1;

pub const ST: u64 = SHIM_BASE;
pub const BS: u64 = SHIM_BASE + 0x100;
pub const RT: u64 = SHIM_BASE + 0x300;
pub const CONOUT: u64 = SHIM_BASE + 0x400;
pub const CONOUT_MODE: u64 = SHIM_BASE + 0x480;
pub const CONIN: u64 = SHIM_BASE + 0x4c0;
pub const CONFIG: u64 = SHIM_BASE + 0x500;
pub const VENDOR: u64 = SHIM_BASE + 0x600;
pub const IMAGE_HANDLE: u64 = SHIM_BASE + 0x700;
pub const CONSOLE_HANDLE: u64 = SHIM_BASE + 0x710;
pub const VENDOR_NAME: &str = "AienOS aienos-run";
pub const FIRMWARE_REVISION: u32 = 0x0001_0000;

const ERR: u64 = 1 << 63;
pub const SUCCESS: u64 = 0;
pub const INVALID_PARAMETER: u64 = ERR | 2;
pub const UNSUPPORTED: u64 = ERR | 3;
pub const BUFFER_TOO_SMALL: u64 = ERR | 5;
pub const NOT_READY: u64 = ERR | 6;
pub const OUT_OF_RESOURCES: u64 = ERR | 9;
pub const NOT_FOUND: u64 = ERR | 14;

pub const DESC_SIZE: u64 = 48;
pub const DESC_VERSION: u32 = 1;
pub const LOADER_CODE: u32 = 1;
pub const BOOT_SERVICES_DATA: u32 = 4;
pub const RUNTIME_SERVICES_CODE: u32 = 5;
pub const CONVENTIONAL: u32 = 7;
pub const ACPI_RECLAIM: u32 = 9;
const ATTR_WB: u64 = 0x8;
const ATTR_RUNTIME: u64 = 1 << 63;

const ACPI_20_GUID: [u8; 16] = [
    0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11, 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81,
];

pub fn stub_addr(id: u32) -> u64 {
    STUB_BASE + u64::from(id) * STUB_BYTES
}

pub fn stub(id: u32) -> [u8; 16] {
    let mut s = [0xcc; 16]; // int3 padding
    s[..2].copy_from_slice(&[0x41, 0xba]);
    s[2..6].copy_from_slice(&id.to_le_bytes());
    s[6..9].copy_from_slice(&[0xe6, CALL_PORT as u8, 0xc3]);
    s
}

/// The `out` instruction's length: RIP moves past it after a call.
pub const OUT_LEN: u64 = 2;

/// IEEE CRC-32 (the one UEFI table headers and CalculateCrc32 use).
pub fn crc32(data: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in data {
        c ^= u32::from(b);
        for _ in 0..8 {
            c = if c & 1 != 0 {
                c >> 1 ^ 0xedb8_8320
            } else {
                c >> 1
            };
        }
    }
    !c
}

/// A table with an EFI_TABLE_HEADER and `n` function pointers to stubs first..first+n.
fn service_table(sig: &[u8; 8], first: u32, n: u32) -> Vec<u8> {
    let mut t = vec![0u8; 24 + 8 * n as usize];
    t[..8].copy_from_slice(sig);
    t[8..12].copy_from_slice(&(2 << 16 | 100u32).to_le_bytes()); // UEFI 2.100 = 2.10
    let len = t.len() as u32;
    t[12..16].copy_from_slice(&len.to_le_bytes());
    for i in 0..n {
        let at = 24 + 8 * i as usize;
        t[at..at + 8].copy_from_slice(&stub_addr(first + i).to_le_bytes());
    }
    let crc = crc32(&t);
    t[16..20].copy_from_slice(&crc.to_le_bytes());
    t
}

fn system_table() -> Vec<u8> {
    let mut t = vec![0u8; 120];
    let put = |t: &mut Vec<u8>, at: usize, v: u64| t[at..at + 8].copy_from_slice(&v.to_le_bytes());
    t[..8].copy_from_slice(b"IBI SYST");
    t[8..12].copy_from_slice(&(2 << 16 | 100u32).to_le_bytes());
    t[12..16].copy_from_slice(&120u32.to_le_bytes());
    put(&mut t, 24, VENDOR);
    t[32..36].copy_from_slice(&FIRMWARE_REVISION.to_le_bytes());
    put(&mut t, 40, CONSOLE_HANDLE);
    put(&mut t, 48, CONIN);
    put(&mut t, 56, CONSOLE_HANDLE);
    put(&mut t, 64, CONOUT);
    put(&mut t, 72, CONSOLE_HANDLE);
    put(&mut t, 80, CONOUT);
    put(&mut t, 88, RT);
    put(&mut t, 96, BS);
    put(&mut t, 104, 1);
    put(&mut t, 112, CONFIG);
    let crc = crc32(&t);
    t[16..20].copy_from_slice(&crc.to_le_bytes());
    t
}

/// Writes the shim into guest memory (tables, protocols, stubs); returns the system table.
pub fn install(mem: &mut GuestMem, rsdp: u64) -> Result<u64, String> {
    mem.write(ST, &system_table())?;
    mem.write(BS, &service_table(b"BOOTSERV", BS_FIRST, BS_COUNT))?;
    mem.write(RT, &service_table(b"RUNTSERV", RT_FIRST, RT_COUNT))?;
    for i in 0..CONOUT_COUNT {
        mem.write_u64(CONOUT + 8 * u64::from(i), stub_addr(CONOUT_FIRST + i))?;
    }
    mem.write_u64(CONOUT + 8 * u64::from(CONOUT_COUNT), CONOUT_MODE)?;
    // Mode: MaxMode 1, Mode 0, Attribute 0x07, column 0, row 0, cursor visible
    for (i, v) in [1u32, 0, 7, 0, 0, 1].iter().enumerate() {
        mem.write_u32(CONOUT_MODE + 4 * i as u64, *v)?;
    }
    for i in 0..CONIN_COUNT {
        mem.write_u64(CONIN + 8 * u64::from(i), stub_addr(CONIN_FIRST + i))?;
    }
    mem.write_u64(CONIN + 16, 0)?; // WaitForKey: no events
    let mut cfg = ACPI_20_GUID.to_vec();
    cfg.extend_from_slice(&rsdp.to_le_bytes());
    mem.write(CONFIG, &cfg)?;
    let vendor: Vec<u8> = VENDOR_NAME
        .encode_utf16()
        .chain([0])
        .flat_map(u16::to_le_bytes)
        .collect();
    mem.write(VENDOR, &vendor)?;
    mem.write(IMAGE_HANDLE, b"aienos-run image")?;
    mem.write(CONSOLE_HANDLE, b"aienos-run cons\0")?;
    for id in 0..STUBS {
        mem.write(stub_addr(id), &stub(id))?;
    }
    if stub_addr(STUBS) > STUB_END {
        return Err("UEFI stubs overflow their region".into());
    }
    Ok(ST)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Desc {
    pub ty: u32,
    pub start: u64,
    pub pages: u64,
    pub attr: u64,
}

/// The memory map the guest sees, in address order (see [`super::mem`]).
pub fn memory_map(ram: &[(u64, u64)], image_bytes: u64) -> Vec<Desc> {
    let d = |ty, start: u64, end: u64| Desc {
        ty,
        start,
        pages: (end - start) / PAGE,
        attr: ATTR_WB
            | if ty == RUNTIME_SERVICES_CODE {
                ATTR_RUNTIME
            } else {
                0
            },
    };
    let image_end = IMAGE_BASE + image_bytes.div_ceil(PAGE) * PAGE;
    let mut m = vec![
        d(CONVENTIONAL, 0, LOW_FREE_END),
        d(BOOT_SERVICES_DATA, FW_BASE, STUB_BASE),
        d(RUNTIME_SERVICES_CODE, STUB_BASE, STUB_END),
        d(ACPI_RECLAIM, ACPI_BASE, ACPI_END),
        d(BOOT_SERVICES_DATA, POOL_BASE, STACK_TOP),
        d(LOADER_CODE, IMAGE_BASE, image_end),
    ];
    for &(lo, hi) in ram {
        let lo = lo.max(image_end);
        if hi > lo {
            m.push(d(CONVENTIONAL, lo, hi));
        }
    }
    m.retain(|x| x.pages > 0);
    m
}

/// Call arguments as the vCPU had them at the stub's `out`.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct CallRegs {
    pub id: u64,
    pub rax: u64,
    pub rcx: u64,
    pub rdx: u64,
    pub r8: u64,
    pub r9: u64,
    pub rsp: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    /// Put this in RAX and continue after the `out`.
    Return(u64),
    PowerOff,
    Reset,
    /// The UEFI application returned (or called Exit) with this status.
    AppReturned(u64),
}

#[derive(Debug)]
pub struct Uefi {
    pub map: Vec<Desc>,
    pub map_key: u64,
    pub exited: bool,
    pool_next: u64,
    /// Call ids answered with EFI_UNSUPPORTED.
    pub unsupported: BTreeSet<u32>,
}

impl Uefi {
    pub fn new(map: Vec<Desc>) -> Uefi {
        Uefi {
            map,
            map_key: 0x41_454e, // any value; changes would mean the map changed
            exited: false,
            pool_next: POOL_BASE,
            unsupported: BTreeSet::new(),
        }
    }

    fn map_bytes(&self) -> Vec<u8> {
        let mut b = vec![0u8; self.map.len() * DESC_SIZE as usize];
        for (i, d) in self.map.iter().enumerate() {
            let o = i * DESC_SIZE as usize;
            b[o..o + 4].copy_from_slice(&d.ty.to_le_bytes());
            b[o + 8..o + 16].copy_from_slice(&d.start.to_le_bytes());
            b[o + 16..o + 24].copy_from_slice(&d.start.to_le_bytes()); // VirtualStart
            b[o + 24..o + 32].copy_from_slice(&d.pages.to_le_bytes());
            b[o + 32..o + 40].copy_from_slice(&d.attr.to_le_bytes());
        }
        b
    }

    fn get_memory_map(&self, mem: &mut GuestMem, r: &CallRegs) -> Result<u64, String> {
        let dver_ptr = mem.read_u64(r.rsp + 0x28).ok_or("stack outside RAM")?;
        let size = mem.read_u64(r.rcx).ok_or("MemoryMapSize outside RAM")?;
        let bytes = self.map_bytes();
        let need = bytes.len() as u64;
        mem.write_u64(r.rcx, need)?;
        if size < need {
            return Ok(BUFFER_TOO_SMALL);
        }
        if r.rdx == 0 {
            return Ok(INVALID_PARAMETER);
        }
        mem.write(r.rdx, &bytes)?;
        mem.write_u64(r.r8, self.map_key)?;
        mem.write_u64(r.r9, DESC_SIZE)?;
        mem.write_u32(dver_ptr, DESC_VERSION)?;
        Ok(SUCCESS)
    }

    fn allocate_pool(&mut self, mem: &mut GuestMem, r: &CallRegs) -> Result<u64, String> {
        let size = r.rdx.div_ceil(16) * 16;
        if r.r8 == 0 {
            return Ok(INVALID_PARAMETER);
        }
        match self.pool_next.checked_add(size) {
            Some(end) if end <= POOL_END => {
                mem.write_u64(r.r8, self.pool_next)?;
                self.pool_next = end;
                Ok(SUCCESS)
            }
            _ => Ok(OUT_OF_RESOURCES),
        }
    }

    /// Answers one call. Guest pointers that leave RAM give EFI_INVALID_PARAMETER.
    pub fn call(&mut self, mem: &mut GuestMem, r: &CallRegs, con: &mut dyn Serial) -> Outcome {
        let id = r.id as u32;
        let res: Result<u64, String> = match id {
            APP_RETURNED => return Outcome::AppReturned(r.rax),
            BS_EXIT => return Outcome::AppReturned(r.rdx),
            _ if RT_FIRST + RT_RESET_SYSTEM == id => {
                return if r.rcx == 2 {
                    Outcome::PowerOff
                } else {
                    Outcome::Reset
                }
            }
            _ if self.exited && id < BS_FIRST + BS_COUNT => Ok(UNSUPPORTED),
            BS_RAISE_TPL => Ok(4), // the old TPL: TPL_APPLICATION
            BS_RESTORE_TPL | BS_SET_WATCHDOG | BS_FREE_POOL => Ok(SUCCESS),
            BS_GET_MEMORY_MAP => self.get_memory_map(mem, r),
            BS_ALLOCATE_POOL => self.allocate_pool(mem, r),
            BS_EXIT_BOOT_SERVICES => {
                if r.rdx == self.map_key {
                    self.exited = true;
                    Ok(SUCCESS)
                } else {
                    Ok(INVALID_PARAMETER)
                }
            }
            BS_STALL => {
                std::thread::sleep(std::time::Duration::from_micros(r.rcx.min(10_000_000)));
                Ok(SUCCESS)
            }
            BS_LOCATE_HANDLE_BUFFER => {
                // (SearchType, Protocol, SearchKey, &NoHandles, &Buffer): no handle has it
                let buf = mem.read_u64(r.rsp + 0x28).unwrap_or(0);
                mem.write_u64(r.r9, 0)
                    .and_then(|_| {
                        if buf != 0 {
                            mem.write_u64(buf, 0)
                        } else {
                            Ok(())
                        }
                    })
                    .map(|_| NOT_FOUND)
            }
            BS_LOCATE_PROTOCOL => mem.write_u64(r.r8, 0).map(|_| NOT_FOUND),
            BS_CALCULATE_CRC32 => match mem.slice(r.rcx, r.rdx as usize).map(crc32) {
                Some(c) => mem.write_u32(r.r8, c).map(|_| SUCCESS),
                None => Ok(INVALID_PARAMETER),
            },
            BS_COPY_MEM => match mem.slice(r.rdx, r.r8 as usize).map(<[u8]>::to_vec) {
                Some(src) => mem.write(r.rcx, &src).map(|_| SUCCESS),
                None => Ok(INVALID_PARAMETER),
            },
            BS_SET_MEM => match mem.slice_mut(r.rcx, r.rdx as usize) {
                Some(dst) => {
                    dst.fill(r.r8 as u8);
                    Ok(SUCCESS)
                }
                None => Ok(INVALID_PARAMETER),
            },
            _ if id == CONOUT_FIRST + CONOUT_OUTPUT_STRING => {
                let mut units = Vec::new();
                let mut a = r.rdx;
                while let Some(b) = mem.slice(a, 2) {
                    let u = u16::from_le_bytes([b[0], b[1]]);
                    if u == 0 || units.len() >= 4096 {
                        break;
                    }
                    units.push(u);
                    a += 2;
                }
                for ch in char::decode_utf16(units).map(|c| c.unwrap_or('\u{fffd}')) {
                    let mut buf = [0u8; 4];
                    for b in ch.encode_utf8(&mut buf).bytes() {
                        con.tx(b);
                    }
                }
                Ok(SUCCESS)
            }
            _ if id == CONOUT_FIRST + CONOUT_QUERY_MODE => mem
                .write_u64(r.r8, 80)
                .and_then(|_| mem.write_u64(r.r9, 25))
                .map(|_| if r.rdx == 0 { SUCCESS } else { UNSUPPORTED }),
            _ if (CONOUT_FIRST..CONOUT_FIRST + CONOUT_COUNT).contains(&id) => Ok(SUCCESS),
            _ if id == CONIN_FIRST => Ok(SUCCESS),
            _ if id == CONIN_FIRST + CONIN_READ_KEY => match con.rx() {
                Some(c) => {
                    let key = u32::from(if c == b'\n' { b'\r' } else { c }) << 16;
                    mem.write_u32(r.rdx, key).map(|_| SUCCESS)
                }
                None => Ok(NOT_READY),
            },
            _ => {
                self.unsupported.insert(id);
                Ok(UNSUPPORTED)
            }
        };
        Outcome::Return(res.unwrap_or(INVALID_PARAMETER))
    }
}

#[cfg(test)]
mod tests {
    use super::super::devices::tests::Fake;
    use super::*;
    use crate::backend::x86::mem::{LOW_RAM_MAX, MIN_RAM};

    fn rd(m: &GuestMem, a: u64) -> u64 {
        m.read_u64(a).unwrap()
    }

    fn setup() -> (GuestMem, Uefi) {
        let mut m = GuestMem::new(MIN_RAM).unwrap();
        install(&mut m, 0x11_8000).unwrap();
        let u = Uefi::new(memory_map(&m.ranges(), 0x5a000));
        (m, u)
    }

    /// Reads a stub's call id the way the CPU would run it.
    fn decode(m: &GuestMem, fnptr: u64) -> u32 {
        let s = m.slice(fnptr, 9).unwrap();
        assert_eq!(&s[..2], &[0x41, 0xba]);
        assert_eq!(&s[6..9], &[0xe6, CALL_PORT as u8, 0xc3]);
        u32::from_le_bytes(s[2..6].try_into().unwrap())
    }

    #[test]
    fn tables_at_the_offsets_efi_h_asserts() {
        let (m, _) = setup();
        let st = ST;
        assert_eq!(m.slice(st, 8).unwrap(), b"IBI SYST");
        let bs = rd(&m, st + 0x60);
        assert_eq!(bs, BS);
        // native/boot/efi.h _Static_asserts
        for (off, id) in [
            (0x28, 2),
            (0x38, BS_GET_MEMORY_MAP),
            (0x48, BS_FREE_POOL),
            (0x98, 16),
            (0xe8, BS_EXIT_BOOT_SERVICES),
            (0x100, BS_SET_WATCHDOG),
            (0x138, BS_LOCATE_HANDLE_BUFFER),
        ] {
            assert_eq!(
                decode(&m, rd(&m, bs + off)),
                id,
                "boot service at 0x{off:x}"
            );
        }
        assert_eq!(
            decode(&m, rd(&m, rd(&m, st + 88) + 0x68)),
            RT_FIRST + RT_RESET_SYSTEM
        );
        assert_eq!(
            decode(&m, rd(&m, rd(&m, st + 0x40) + 8)),
            CONOUT_FIRST + CONOUT_OUTPUT_STRING
        );
        // Configuration table: one entry, ACPI 2.0 GUID -> RSDP
        assert_eq!(rd(&m, st + 104), 1);
        let cfg = rd(&m, st + 0x70);
        assert_eq!(m.slice(cfg, 16).unwrap(), ACPI_20_GUID);
        assert_eq!(rd(&m, cfg + 16), 0x11_8000);
        // Vendor string, as efi_x86.c prints it
        let v: Vec<u16> = m
            .slice(rd(&m, st + 24), 2 * (VENDOR_NAME.len() + 1))
            .unwrap()
            .chunks(2)
            .map(|c| u16::from_le_bytes([c[0], c[1]]))
            .collect();
        assert_eq!(String::from_utf16(&v[..v.len() - 1]).unwrap(), VENDOR_NAME);
        assert_eq!(*v.last().unwrap(), 0);
        // header CRCs
        for (a, n) in [(ST, 120usize), (BS, 24 + 8 * BS_COUNT as usize)] {
            let mut t = m.slice(a, n).unwrap().to_vec();
            let crc = u32::from_le_bytes(t[16..20].try_into().unwrap());
            t[16..20].fill(0);
            assert_eq!(crc32(&t), crc);
        }
    }

    #[test]
    fn crc32_check_value() {
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
    }

    #[test]
    fn memory_map_types_and_order() {
        let map = memory_map(
            &[(0, LOW_RAM_MAX), (1 << 32, (1 << 32) + (1 << 30))],
            0x5a000,
        );
        assert!(map
            .windows(2)
            .all(|w| w[0].start + w[0].pages * PAGE <= w[1].start));
        let free: u64 = map
            .iter()
            .filter(|d| d.ty == CONVENTIONAL)
            .map(|d| d.pages * PAGE)
            .sum();
        assert_eq!(
            free,
            LOW_FREE_END + (LOW_RAM_MAX - IMAGE_BASE - 0x5a000) + (1 << 30)
        );
        let image = map.iter().find(|d| d.ty == LOADER_CODE).unwrap();
        assert_eq!((image.start, image.pages), (IMAGE_BASE, 0x5a));
        assert!(map
            .iter()
            .all(|d| d.ty == CONVENTIONAL || d.start >= FW_BASE));
        // nothing the shim or the stack uses is conventional (the kernel's frame source)
        for a in [FW_BASE, STUB_BASE, ACPI_BASE, POOL_BASE, STACK_TOP - 8] {
            let d = map
                .iter()
                .find(|d| d.start <= a && a < d.start + d.pages * PAGE)
                .unwrap();
            assert_ne!(d.ty, CONVENTIONAL, "0x{a:x}");
        }
    }

    /// The call sequence of native/kernel/x86/efi_x86.c efi_main.
    #[test]
    fn efi_main_sequence() {
        let (mut m, mut u) = setup();
        let mut con = Fake::default();
        let stack = 0x9_0000u64;
        let scratch = 0x8_0000u64;
        let mut call = |m: &mut GuestMem, u: &mut Uefi, id: u32, a: [u64; 4], fifth: u64| {
            m.write_u64(stack + 0x28, fifth).unwrap();
            let r = CallRegs {
                id: u64::from(id),
                rcx: a[0],
                rdx: a[1],
                r8: a[2],
                r9: a[3],
                rsp: stack,
                ..CallRegs::default()
            };
            u.call(m, &r, &mut con)
        };
        assert_eq!(
            call(&mut m, &mut u, BS_SET_WATCHDOG, [0; 4], 0),
            Outcome::Return(SUCCESS)
        );
        // ck_boot_gop_find: LocateHandleBuffer(ByProtocol, &gop, NULL, &n, &handles)
        m.write_u64(scratch, 7).unwrap();
        m.write_u64(scratch + 8, 7).unwrap();
        let r = call(
            &mut m,
            &mut u,
            BS_LOCATE_HANDLE_BUFFER,
            [2, 0x1000, 0, scratch],
            scratch + 8,
        );
        assert_eq!(r, Outcome::Return(NOT_FOUND));
        assert_eq!((rd(&m, scratch), rd(&m, scratch + 8)), (0, 0));
        // GetMemoryMap with too small a buffer, then a right-sized one
        let (size, buf, key, dsize, dver) = (
            scratch,
            scratch + 0x100,
            scratch + 8,
            scratch + 16,
            scratch + 24,
        );
        m.write_u64(size, 16).unwrap();
        let r = call(
            &mut m,
            &mut u,
            BS_GET_MEMORY_MAP,
            [size, buf, key, dsize],
            dver,
        );
        assert_eq!(r, Outcome::Return(BUFFER_TOO_SMALL));
        let need = rd(&m, size);
        assert_eq!(need, u.map.len() as u64 * DESC_SIZE);
        m.write_u64(size, 64 << 10).unwrap();
        let r = call(
            &mut m,
            &mut u,
            BS_GET_MEMORY_MAP,
            [size, buf, key, dsize],
            dver,
        );
        assert_eq!(r, Outcome::Return(SUCCESS));
        assert_eq!(rd(&m, size), need);
        assert_eq!(rd(&m, dsize), DESC_SIZE);
        assert_eq!(m.slice(dver, 4).unwrap(), &DESC_VERSION.to_le_bytes());
        assert_eq!(m.slice(buf, 4).unwrap(), &CONVENTIONAL.to_le_bytes());
        assert_eq!(rd(&m, buf + 24), LOW_FREE_END / PAGE);
        // ExitBootServices: a stale key is refused, the right one accepted
        let k = rd(&m, key);
        let r = call(
            &mut m,
            &mut u,
            BS_EXIT_BOOT_SERVICES,
            [IMAGE_HANDLE, k + 1, 0, 0],
            0,
        );
        assert_eq!(r, Outcome::Return(INVALID_PARAMETER));
        assert!(!u.exited);
        let r = call(
            &mut m,
            &mut u,
            BS_EXIT_BOOT_SERVICES,
            [IMAGE_HANDLE, k, 0, 0],
            0,
        );
        assert_eq!(r, Outcome::Return(SUCCESS));
        assert!(u.exited);
        let r = call(&mut m, &mut u, BS_SET_WATCHDOG, [0; 4], 0);
        assert_eq!(
            r,
            Outcome::Return(UNSUPPORTED),
            "no boot services after exit"
        );
        assert!(u.unsupported.is_empty());
    }

    #[test]
    fn console_pool_reset_and_unknown() {
        let (mut m, mut u) = setup();
        let mut con = Fake::default();
        let call = |m: &mut GuestMem, u: &mut Uefi, con: &mut Fake, id: u32, a: [u64; 4]| {
            let r = CallRegs {
                id: u64::from(id),
                rcx: a[0],
                rdx: a[1],
                r8: a[2],
                r9: a[3],
                rsp: 0x9_0000,
                ..CallRegs::default()
            };
            u.call(m, &r, con)
        };
        let s: Vec<u8> = "hi é\n\0"
            .encode_utf16()
            .flat_map(u16::to_le_bytes)
            .collect();
        m.write(0x8_0000, &s).unwrap();
        let out = CONOUT_FIRST + CONOUT_OUTPUT_STRING;
        assert_eq!(
            call(&mut m, &mut u, &mut con, out, [CONOUT, 0x8_0000, 0, 0]),
            Outcome::Return(SUCCESS)
        );
        assert_eq!(con.out, "hi é\n".as_bytes());
        con.keys.extend(b"x");
        let rk = CONIN_FIRST + CONIN_READ_KEY;
        assert_eq!(
            call(&mut m, &mut u, &mut con, rk, [CONIN, 0x8_0100, 0, 0]),
            Outcome::Return(SUCCESS)
        );
        assert_eq!(
            m.slice(0x8_0102, 2).unwrap(),
            &u16::from(b'x').to_le_bytes()
        );
        assert_eq!(
            call(&mut m, &mut u, &mut con, rk, [CONIN, 0x8_0100, 0, 0]),
            Outcome::Return(NOT_READY)
        );
        // pool: two allocations, distinct, inside the arena
        for _ in 0..2 {
            let r = call(
                &mut m,
                &mut u,
                &mut con,
                BS_ALLOCATE_POOL,
                [4, 100, 0x8_0200, 0],
            );
            assert_eq!(r, Outcome::Return(SUCCESS));
        }
        let a = rd(&m, 0x8_0200);
        assert_eq!(a, POOL_BASE + 112);
        let r = call(
            &mut m,
            &mut u,
            &mut con,
            BS_ALLOCATE_POOL,
            [4, 1 << 30, 0x8_0200, 0],
        );
        assert_eq!(r, Outcome::Return(OUT_OF_RESOURCES));
        // pointers outside RAM are refused, not a VMM crash
        let r = call(
            &mut m,
            &mut u,
            &mut con,
            BS_ALLOCATE_POOL,
            [4, 8, LOW_RAM_MAX, 0],
        );
        assert_eq!(r, Outcome::Return(INVALID_PARAMETER));
        // unknown services are listed
        let r = call(&mut m, &mut u, &mut con, 22, [0; 4]); // LoadImage
        assert_eq!(r, Outcome::Return(UNSUPPORTED));
        assert_eq!(u.unsupported.iter().copied().collect::<Vec<_>>(), [22]);
        // ResetSystem(EfiResetShutdown) powers off; returning from efi_main ends the run
        let rs = RT_FIRST + RT_RESET_SYSTEM;
        assert_eq!(
            call(&mut m, &mut u, &mut con, rs, [2, 0, 0, 0]),
            Outcome::PowerOff
        );
        assert_eq!(
            call(&mut m, &mut u, &mut con, rs, [0, 0, 0, 0]),
            Outcome::Reset
        );
        let r = CallRegs {
            id: u64::from(APP_RETURNED),
            rax: INVALID_PARAMETER,
            ..CallRegs::default()
        };
        assert_eq!(
            u.call(&mut m, &r, &mut con),
            Outcome::AppReturned(INVALID_PARAMETER)
        );
    }
}
