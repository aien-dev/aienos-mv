//! Guest RAM and the guest physical layout.
//!
//! RAM is one or two page-aligned host allocations: [0, min(ram, 3 GiB)) and, for bigger
//! guests, the rest from 4 GiB (the 3..4 GiB hole holds the I/O APIC, HPET and local
//! APIC). The first 2 MiB hold what firmware would leave behind:
//!
//! | guest physical          | what                                  | UEFI memory type |
//! |-------------------------|---------------------------------------|------------------|
//! | 0 .. 0xA0000            | free (the kernel's SMP trampoline)    | conventional     |
//! | 1 MiB + 0x0000          | GDT and TSS                           | boot services data |
//! | 1 MiB + 0x1000          | identity page tables                  | boot services data |
//! | 1 MiB + 0x11000         | system table, services, protocols     | boot services data |
//! | 1 MiB + 0x13000         | call stubs                            | runtime services code |
//! | 1 MiB + 0x18000         | ACPI tables                           | ACPI reclaim     |
//! | 1 MiB + 0x20000         | AllocatePool arena                    | boot services data |
//! | 1 MiB + 0xE0000 .. 2 MiB| the UEFI application's stack          | boot services data |
//! | 2 MiB ..                | BOOTX64.EFI                           | loader code      |
//! | after the image         | free                                  | conventional     |
//!
//! The kernel takes frames only from conventional memory (native/kernel/mm/frames.c,
//! ck_frames_from_efi), so nothing here is reused while it still runs on these tables.

pub const PAGE: u64 = 4096;
pub const LOW_FREE_END: u64 = 0xA_0000;
pub const FW_BASE: u64 = 0x10_0000;
pub const GDT_BASE: u64 = FW_BASE;
pub const TSS_BASE: u64 = FW_BASE + 0x80;
pub const PT_BASE: u64 = FW_BASE + 0x1000;
pub const PT_PAGES: u64 = 16;
pub const SHIM_BASE: u64 = FW_BASE + 0x11000;
pub const SHIM_END: u64 = FW_BASE + 0x13000;
pub const STUB_BASE: u64 = FW_BASE + 0x13000;
pub const STUB_END: u64 = FW_BASE + 0x18000;
pub const ACPI_BASE: u64 = FW_BASE + 0x18000;
pub const ACPI_END: u64 = FW_BASE + 0x20000;
pub const POOL_BASE: u64 = FW_BASE + 0x20000;
pub const POOL_END: u64 = FW_BASE + 0xE0000;
pub const STACK_TOP: u64 = FW_BASE + 0x10_0000;
pub const IMAGE_BASE: u64 = 0x20_0000;
/// RAM below this; device MMIO between it and 4 GiB.
pub const LOW_RAM_MAX: u64 = 0xC000_0000;
pub const HIGH_BASE: u64 = 1 << 32;
pub const MIN_RAM: u64 = 64 << 20;
/// PML4 + PDPT + one page directory per GiB of address space must fit in PT_PAGES.
pub const MAX_RAM: u64 = 12 << 30;

struct Region {
    gpa: u64,
    ptr: *mut u8,
    len: usize,
}

/// Guest RAM. Host memory is page-aligned and zeroed, so it can be handed to the
/// hypervisor as is (WHvMapGpaRange needs page-aligned host addresses).
pub struct GuestMem {
    regions: Vec<Region>,
}

// The regions are plain memory owned by this value; the hypervisor and the vCPU thread
// access them through the same pointers.
unsafe impl Send for GuestMem {}
unsafe impl Sync for GuestMem {}

impl GuestMem {
    pub fn check_size(ram: u64) -> Result<(), String> {
        if !(MIN_RAM..=MAX_RAM).contains(&ram) || !ram.is_multiple_of(2 << 20) {
            return Err(format!(
                "guest RAM {} MiB: the own x86 VMM takes {}..{} MiB in 2 MiB steps",
                ram >> 20,
                MIN_RAM >> 20,
                MAX_RAM >> 20
            ));
        }
        Ok(())
    }

    pub fn new(ram: u64) -> Result<GuestMem, String> {
        GuestMem::check_size(ram)?;
        let low = ram.min(LOW_RAM_MAX);
        let mut m = GuestMem {
            regions: Vec::new(),
        };
        m.add(0, low)?;
        if ram > low {
            m.add(HIGH_BASE, ram - low)?;
        }
        Ok(m)
    }

    fn add(&mut self, gpa: u64, len: u64) -> Result<(), String> {
        let len = usize::try_from(len).map_err(|_| "guest RAM too big for this host")?;
        let ptr = os::map(len);
        if ptr.is_null() {
            return Err(format!("cannot allocate {} MiB of guest RAM", len >> 20));
        }
        self.regions.push(Region { gpa, ptr, len });
        Ok(())
    }

    /// (guest physical address, host pointer, bytes) of each RAM region.
    pub fn regions(&self) -> Vec<(u64, *mut u8, u64)> {
        self.regions
            .iter()
            .map(|r| (r.gpa, r.ptr, r.len as u64))
            .collect()
    }

    /// (start, end) of each RAM region.
    pub fn ranges(&self) -> Vec<(u64, u64)> {
        self.regions
            .iter()
            .map(|r| (r.gpa, r.gpa + r.len as u64))
            .collect()
    }

    /// One past the highest RAM byte.
    pub fn top(&self) -> u64 {
        self.ranges().iter().map(|r| r.1).max().unwrap_or(0)
    }

    fn find(&self, gpa: u64, len: usize) -> Option<(&Region, usize)> {
        self.regions.iter().find_map(|r| {
            let off = gpa.checked_sub(r.gpa)? as usize;
            (off.checked_add(len)? <= r.len).then_some((r, off))
        })
    }

    pub fn is_ram(&self, gpa: u64, len: usize) -> bool {
        self.find(gpa, len).is_some()
    }

    pub fn slice(&self, gpa: u64, len: usize) -> Option<&[u8]> {
        let (r, off) = self.find(gpa, len)?;
        // SAFETY: [off, off+len) lies inside the region's allocation.
        Some(unsafe { std::slice::from_raw_parts(r.ptr.add(off), len) })
    }

    pub fn slice_mut(&mut self, gpa: u64, len: usize) -> Option<&mut [u8]> {
        let (r, off) = self.find(gpa, len)?;
        // SAFETY: as above; &mut self makes the borrow unique on the host side.
        Some(unsafe { std::slice::from_raw_parts_mut(r.ptr.add(off), len) })
    }

    pub fn write(&mut self, gpa: u64, bytes: &[u8]) -> Result<(), String> {
        self.slice_mut(gpa, bytes.len())
            .ok_or_else(|| format!("guest write 0x{gpa:x}+{} outside RAM", bytes.len()))?
            .copy_from_slice(bytes);
        Ok(())
    }

    pub fn read_u64(&self, gpa: u64) -> Option<u64> {
        self.slice(gpa, 8)
            .map(|b| u64::from_le_bytes(b.try_into().expect("8 bytes")))
    }

    pub fn write_u64(&mut self, gpa: u64, v: u64) -> Result<(), String> {
        self.write(gpa, &v.to_le_bytes())
    }

    pub fn write_u32(&mut self, gpa: u64, v: u32) -> Result<(), String> {
        self.write(gpa, &v.to_le_bytes())
    }
}

impl Drop for GuestMem {
    fn drop(&mut self) {
        for r in &self.regions {
            os::unmap(r.ptr, r.len);
        }
    }
}

/// Guest RAM comes straight from the OS: page-aligned and zero-filled on first touch, so
/// a 1 GiB guest costs nothing until it uses the memory (the allocator would zero every
/// byte up front for page alignment).
mod os {
    #[cfg(unix)]
    pub fn map(len: usize) -> *mut u8 {
        // SAFETY: a fresh private anonymous mapping.
        let p = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                len,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANON,
                -1,
                0,
            )
        };
        if p == libc::MAP_FAILED {
            std::ptr::null_mut()
        } else {
            p.cast()
        }
    }

    #[cfg(unix)]
    pub fn unmap(p: *mut u8, len: usize) {
        // SAFETY: a mapping made by `map` with this length.
        unsafe { libc::munmap(p.cast(), len) };
    }

    #[cfg(windows)]
    pub fn map(len: usize) -> *mut u8 {
        use crate::win::{VirtualAlloc, MEM_COMMIT, MEM_RESERVE, PAGE_READWRITE};
        // SAFETY: a fresh private allocation.
        unsafe {
            VirtualAlloc(
                std::ptr::null_mut(),
                len,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE,
            )
            .cast()
        }
    }

    #[cfg(windows)]
    pub fn unmap(p: *mut u8, _len: usize) {
        use crate::win::{VirtualFree, MEM_RELEASE};
        // SAFETY: an allocation made by `map`.
        unsafe { VirtualFree(p.cast(), 0, MEM_RELEASE) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sizes_and_split() {
        assert!(GuestMem::new(32 << 20).is_err());
        assert!(GuestMem::new((64 << 20) + 4096).is_err());
        assert!(GuestMem::new(MAX_RAM + (2 << 20)).is_err());
        let m = GuestMem::new(128 << 20).unwrap();
        assert_eq!(m.ranges(), vec![(0, 128 << 20)]);
        assert_eq!(m.top(), 128 << 20);
        let r = m.regions();
        assert_eq!(
            r[0].1 as usize % PAGE as usize,
            0,
            "page aligned for the hypervisor"
        );
    }

    #[test]
    fn read_write_and_bounds() {
        let mut m = GuestMem::new(64 << 20).unwrap();
        m.write_u64(0x1000, 0x1122_3344_5566_7788).unwrap();
        assert_eq!(m.read_u64(0x1000), Some(0x1122_3344_5566_7788));
        assert_eq!(m.slice(0x1000, 1), Some(&[0x88u8][..]));
        assert!(m.write_u64((64 << 20) - 4, 1).is_err(), "straddles the end");
        assert!(m.read_u64(LOW_RAM_MAX).is_none());
        assert!(!m.is_ram(u64::MAX - 2, 8), "no overflow");
    }

    #[test]
    fn layout_is_ordered_and_fits_under_the_image() {
        let marks = [
            GDT_BASE, TSS_BASE, PT_BASE, SHIM_BASE, SHIM_END, STUB_END, ACPI_END, POOL_END,
            STACK_TOP,
        ];
        assert!(marks.windows(2).all(|w| w[0] < w[1]));
        const { assert!(PT_BASE + PT_PAGES * PAGE <= SHIM_BASE) };
        assert_eq!(STUB_BASE, SHIM_END);
        assert_eq!(ACPI_BASE, STUB_END);
        assert_eq!(POOL_BASE, ACPI_END);
        assert_eq!(STACK_TOP, IMAGE_BASE);
        const { assert!(TSS_BASE + 104 <= PT_BASE) };
    }
}
