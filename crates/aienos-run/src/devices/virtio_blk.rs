//! virtio-blk over virtio-mmio, backend-neutral (KVM, WHPX and Virtualization.framework
//! backends can all route an MMIO window to it). Written from the OASIS VIRTIO 1.2
//! specification: section 4.2.2 (MMIO register layout), 2.7 (split virtqueues) and 5.2
//! (block device). Modern (version 2) interface only, one request queue.
//!
//! The model owns no threads: the backend calls [`VirtioBlk::mmio_read`] /
//! [`VirtioBlk::mmio_write`] on each trapped access, and a queue notify processes the
//! request ring synchronously against the disk file. `mmio_write` returns true when the
//! backend must raise the device's interrupt line.

use std::fs::File;
use std::io;

/// Positional read/write on the disk file, Unix and Windows.
fn read_at(f: &File, mut buf: &mut [u8], mut off: u64) -> io::Result<()> {
    while !buf.is_empty() {
        #[cfg(unix)]
        let n = std::os::unix::fs::FileExt::read_at(f, buf, off)?;
        #[cfg(windows)]
        let n = std::os::windows::fs::FileExt::seek_read(f, buf, off)?;
        if n == 0 {
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
        buf = &mut buf[n..];
        off += n as u64;
    }
    Ok(())
}

fn write_at(f: &File, mut buf: &[u8], mut off: u64) -> io::Result<()> {
    while !buf.is_empty() {
        #[cfg(unix)]
        let n = std::os::unix::fs::FileExt::write_at(f, buf, off)?;
        #[cfg(windows)]
        let n = std::os::windows::fs::FileExt::seek_write(f, buf, off)?;
        if n == 0 {
            return Err(io::ErrorKind::WriteZero.into());
        }
        buf = &buf[n..];
        off += n as u64;
    }
    Ok(())
}

/// Guest physical memory as the device sees it (DMA). Err: the range is not guest RAM.
#[allow(clippy::result_unit_err)]
pub trait GuestMemory {
    fn read(&self, gpa: u64, buf: &mut [u8]) -> Result<(), ()>;
    fn write(&self, gpa: u64, buf: &[u8]) -> Result<(), ()>;
}

pub const MMIO_SIZE: u64 = 0x200;
const MAGIC: u32 = 0x7472_6976; // "virt"
const DEVICE_ID_BLOCK: u32 = 2;
const VENDOR_ID: u32 = 0x4e45_4941; // "AIEN"
pub const QUEUE_MAX: u16 = 256;

const F_BLK_FLUSH: u64 = 1 << 9;
const F_VERSION_1: u64 = 1 << 32;
const FEATURES: u64 = F_VERSION_1 | F_BLK_FLUSH;

const STATUS_FEATURES_OK: u32 = 8;
const STATUS_FAILED: u32 = 128;

const DESC_F_NEXT: u16 = 1;
const DESC_F_WRITE: u16 = 2;

const T_IN: u32 = 0;
const T_OUT: u32 = 1;
const T_FLUSH: u32 = 4;
const T_GET_ID: u32 = 8;
const S_OK: u8 = 0;
const S_IOERR: u8 = 1;
const S_UNSUPP: u8 = 2;

#[derive(Default, Clone, Copy)]
struct Queue {
    num: u16,
    ready: bool,
    desc: u64,
    driver: u64,
    device: u64,
    last_avail: u16,
}

pub struct VirtioBlk {
    disk: File,
    sectors: u64,
    read_only: bool,
    status: u32,
    dev_feat_sel: u32,
    drv_feat_sel: u32,
    driver_features: u64,
    queue_sel: u32,
    q: Queue,
    isr: u32,
    pub requests: u64,
}

fn lo(v: u64) -> u32 {
    v as u32
}
fn hi(v: u64) -> u32 {
    (v >> 32) as u32
}
fn set_lo(v: &mut u64, x: u32) {
    *v = (*v & !0xffff_ffff) | x as u64;
}
fn set_hi(v: &mut u64, x: u32) {
    *v = (*v & 0xffff_ffff) | ((x as u64) << 32);
}

impl VirtioBlk {
    pub fn new(disk: File, read_only: bool) -> io::Result<VirtioBlk> {
        let len = disk.metadata()?.len();
        Ok(VirtioBlk {
            disk,
            sectors: len / 512,
            read_only,
            status: 0,
            dev_feat_sel: 0,
            drv_feat_sel: 0,
            driver_features: 0,
            queue_sel: 0,
            q: Queue::default(),
            isr: 0,
            requests: 0,
        })
    }

    fn reset(&mut self) {
        self.status = 0;
        self.driver_features = 0;
        self.q = Queue::default();
        self.isr = 0;
    }

    pub fn mmio_read(&mut self, off: u64) -> u32 {
        match off {
            0x000 => MAGIC,
            0x004 => 2,
            0x008 => DEVICE_ID_BLOCK,
            0x00c => VENDOR_ID,
            0x010 => match self.dev_feat_sel {
                0 => lo(FEATURES),
                1 => hi(FEATURES),
                _ => 0,
            },
            0x034 => {
                if self.queue_sel == 0 {
                    QUEUE_MAX as u32
                } else {
                    0
                }
            }
            0x044 => self.q.ready as u32,
            0x060 => self.isr,
            0x070 => self.status,
            0x0fc => 0,
            // Config space: capacity (le64) in 512-byte sectors.
            0x100 => lo(self.sectors),
            0x104 => hi(self.sectors),
            _ => 0,
        }
    }

    /// Returns true when the interrupt line must be raised.
    pub fn mmio_write(&mut self, off: u64, val: u32, mem: &dyn GuestMemory) -> bool {
        let q0 = self.queue_sel == 0;
        match off {
            0x014 => self.dev_feat_sel = val,
            0x020 => match self.drv_feat_sel {
                0 => set_lo(&mut self.driver_features, val),
                1 => set_hi(&mut self.driver_features, val),
                _ => {}
            },
            0x024 => self.drv_feat_sel = val,
            0x030 => self.queue_sel = val,
            0x038 if q0 => self.q.num = (val as u16).min(QUEUE_MAX),
            0x044 if q0 => self.q.ready = val & 1 == 1,
            0x050 if val == 0 => return self.process(mem),
            0x064 => self.isr &= !val,
            0x070 => {
                if val == 0 {
                    self.reset();
                } else {
                    let mut v = val;
                    // FEATURES_OK only sticks for a feature set we offered, incl. VERSION_1.
                    if v & STATUS_FEATURES_OK != 0
                        && (self.driver_features & !FEATURES != 0
                            || self.driver_features & F_VERSION_1 == 0)
                    {
                        v &= !STATUS_FEATURES_OK;
                    }
                    self.status = v;
                }
            }
            0x080 if q0 => set_lo(&mut self.q.desc, val),
            0x084 if q0 => set_hi(&mut self.q.desc, val),
            0x090 if q0 => set_lo(&mut self.q.driver, val),
            0x094 if q0 => set_hi(&mut self.q.driver, val),
            0x0a0 if q0 => set_lo(&mut self.q.device, val),
            0x0a4 if q0 => set_hi(&mut self.q.device, val),
            _ => {}
        }
        false
    }

    fn rd16(mem: &dyn GuestMemory, a: u64) -> Option<u16> {
        let mut b = [0u8; 2];
        mem.read(a, &mut b).ok()?;
        Some(u16::from_le_bytes(b))
    }

    /// Drains the available ring. Any malformed chain marks the device FAILED
    /// (NEEDS_RESET semantics simplified) instead of touching memory it should not.
    fn process(&mut self, mem: &dyn GuestMemory) -> bool {
        let q = self.q;
        if !q.ready || q.num == 0 || self.status & STATUS_FAILED != 0 {
            return false;
        }
        let mut done = false;
        loop {
            let Some(avail_idx) = Self::rd16(mem, q.driver + 2) else {
                self.status |= STATUS_FAILED;
                break;
            };
            if self.q.last_avail == avail_idx {
                break;
            }
            let slot = (self.q.last_avail % q.num) as u64;
            let Some(head) = Self::rd16(mem, q.driver + 4 + 2 * slot) else {
                self.status |= STATUS_FAILED;
                break;
            };
            let written = match self.request(mem, head) {
                Ok(n) => n,
                Err(()) => {
                    self.status |= STATUS_FAILED;
                    break;
                }
            };
            // used ring entry, then used idx.
            let Some(used_idx) = Self::rd16(mem, q.device + 2) else {
                self.status |= STATUS_FAILED;
                break;
            };
            let e = q.device + 4 + 8 * (used_idx % q.num) as u64;
            let mut ent = [0u8; 8];
            ent[0..4].copy_from_slice(&(head as u32).to_le_bytes());
            ent[4..8].copy_from_slice(&written.to_le_bytes());
            if mem.write(e, &ent).is_err()
                || mem
                    .write(q.device + 2, &used_idx.wrapping_add(1).to_le_bytes())
                    .is_err()
            {
                self.status |= STATUS_FAILED;
                break;
            }
            self.q.last_avail = self.q.last_avail.wrapping_add(1);
            self.requests += 1;
            done = true;
        }
        if done {
            self.isr |= 1;
        }
        done
    }

    /// One request chain: header, data buffers, status byte. Returns bytes written
    /// into guest buffers (data + status).
    fn request(&mut self, mem: &dyn GuestMemory, head: u16) -> Result<u32, ()> {
        let num = self.q.num;
        let mut chain = Vec::new();
        let mut i = head;
        loop {
            if i >= num || chain.len() > num as usize {
                return Err(());
            }
            let mut d = [0u8; 16];
            mem.read(self.q.desc + 16 * i as u64, &mut d)?;
            let addr = u64::from_le_bytes(d[0..8].try_into().unwrap());
            let len = u32::from_le_bytes(d[8..12].try_into().unwrap());
            let flags = u16::from_le_bytes(d[12..14].try_into().unwrap());
            let next = u16::from_le_bytes(d[14..16].try_into().unwrap());
            chain.push((addr, len, flags));
            if flags & DESC_F_NEXT == 0 {
                break;
            }
            i = next;
        }
        if chain.len() < 2 {
            return Err(());
        }
        let (haddr, hlen, hflags) = chain[0];
        let (saddr, slen, sflags) = *chain.last().unwrap();
        if hlen < 16 || hflags & DESC_F_WRITE != 0 || slen < 1 || sflags & DESC_F_WRITE == 0 {
            return Err(());
        }
        let mut h = [0u8; 16];
        mem.read(haddr, &mut h)?;
        let ty = u32::from_le_bytes(h[0..4].try_into().unwrap());
        let sector = u64::from_le_bytes(h[8..16].try_into().unwrap());
        let data = &chain[1..chain.len() - 1];
        let total: u64 = data.iter().map(|d| d.1 as u64).sum();
        let mut written = 0u32;
        let in_range = |total: u64| {
            total.is_multiple_of(512)
                && sector
                    .checked_add(total / 512)
                    .is_some_and(|end| end <= self.sectors)
        };
        let status = match ty {
            T_IN if data.iter().all(|d| d.2 & DESC_F_WRITE != 0) && in_range(total) => {
                let mut off = sector * 512;
                let mut st = S_OK;
                for &(a, l, _) in data {
                    let mut buf = vec![0u8; l as usize];
                    if read_at(&self.disk, &mut buf, off).is_err() {
                        st = S_IOERR;
                        break;
                    }
                    mem.write(a, &buf)?;
                    written += l;
                    off += l as u64;
                }
                st
            }
            T_OUT if self.read_only => S_IOERR,
            T_OUT if data.iter().all(|d| d.2 & DESC_F_WRITE == 0) && in_range(total) => {
                let mut off = sector * 512;
                let mut st = S_OK;
                for &(a, l, _) in data {
                    let mut buf = vec![0u8; l as usize];
                    mem.read(a, &mut buf)?;
                    if write_at(&self.disk, &buf, off).is_err() {
                        st = S_IOERR;
                        break;
                    }
                    off += l as u64;
                }
                st
            }
            T_FLUSH => {
                if self.disk.sync_data().is_ok() {
                    S_OK
                } else {
                    S_IOERR
                }
            }
            T_GET_ID if data.len() == 1 && data[0].2 & DESC_F_WRITE != 0 => {
                let mut id = [0u8; 20];
                id[..12].copy_from_slice(b"aienos-store");
                let n = (data[0].1 as usize).min(20);
                mem.write(data[0].0, &id[..n])?;
                written += n as u32;
                S_OK
            }
            T_IN | T_OUT => S_IOERR,
            _ => S_UNSUPP,
        };
        mem.write(saddr, &[status])?;
        Ok(written + 1)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;

    struct Mem(RefCell<Vec<u8>>);
    impl GuestMemory for Mem {
        fn read(&self, gpa: u64, buf: &mut [u8]) -> Result<(), ()> {
            let m = self.0.borrow();
            let s = m.get(gpa as usize..gpa as usize + buf.len()).ok_or(())?;
            buf.copy_from_slice(s);
            Ok(())
        }
        fn write(&self, gpa: u64, buf: &[u8]) -> Result<(), ()> {
            let mut m = self.0.borrow_mut();
            let d = m
                .get_mut(gpa as usize..gpa as usize + buf.len())
                .ok_or(())?;
            d.copy_from_slice(buf);
            Ok(())
        }
    }

    const DESC: u64 = 0x1000;
    const AVAIL: u64 = 0x2000;
    const USED: u64 = 0x3000;

    fn disk(tag: &str, sectors: u64) -> (File, std::path::PathBuf) {
        let p = std::env::temp_dir().join(format!("aienos-vblk-{tag}-{}", std::process::id()));
        let f = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(&p)
            .unwrap();
        f.set_len(sectors * 512).unwrap();
        (f, p)
    }

    /// Plays the driver: negotiate, set up queue 0.
    fn bring_up(d: &mut VirtioBlk, m: &Mem) {
        assert_eq!(d.mmio_read(0), MAGIC);
        assert_eq!(d.mmio_read(8), 2);
        d.mmio_write(0x70, 1 | 2, m);
        d.mmio_write(0x14, 1, m);
        assert_eq!(d.mmio_read(0x10), 1, "VERSION_1 offered");
        d.mmio_write(0x24, 1, m);
        d.mmio_write(0x20, 1, m);
        d.mmio_write(0x70, 1 | 2 | 8, m);
        assert_eq!(d.mmio_read(0x70) & 8, 8, "FEATURES_OK accepted");
        d.mmio_write(0x30, 0, m);
        assert_eq!(d.mmio_read(0x34), QUEUE_MAX as u32);
        d.mmio_write(0x38, 8, m);
        d.mmio_write(0x80, DESC as u32, m);
        d.mmio_write(0x90, AVAIL as u32, m);
        d.mmio_write(0xa0, USED as u32, m);
        d.mmio_write(0x44, 1, m);
        d.mmio_write(0x70, 1 | 2 | 8 | 4, m);
    }

    fn desc(m: &Mem, i: u16, addr: u64, len: u32, flags: u16, next: u16) {
        let mut d = [0u8; 16];
        d[0..8].copy_from_slice(&addr.to_le_bytes());
        d[8..12].copy_from_slice(&len.to_le_bytes());
        d[12..14].copy_from_slice(&flags.to_le_bytes());
        d[14..16].copy_from_slice(&next.to_le_bytes());
        m.write(DESC + 16 * i as u64, &d).unwrap();
    }

    /// Queues one request (3 descriptors starting at `first`) and notifies.
    fn submit(
        d: &mut VirtioBlk,
        m: &Mem,
        first: u16,
        ty: u32,
        sector: u64,
        buf: u64,
        len: u32,
    ) -> u8 {
        let hdr = 0x4000 + first as u64 * 0x100;
        let mut h = [0u8; 16];
        h[0..4].copy_from_slice(&ty.to_le_bytes());
        h[8..16].copy_from_slice(&sector.to_le_bytes());
        m.write(hdr, &h).unwrap();
        let dir = if ty == T_IN { DESC_F_WRITE } else { 0 };
        let st = hdr + 0x80;
        desc(m, first, hdr, 16, DESC_F_NEXT, first + 1);
        desc(m, first + 1, buf, len, dir | DESC_F_NEXT, first + 2);
        desc(m, first + 2, st, 1, DESC_F_WRITE, 0);
        let idx = VirtioBlk::rd16(m, AVAIL + 2).unwrap();
        m.write(AVAIL + 4 + 2 * (idx % 8) as u64, &first.to_le_bytes())
            .unwrap();
        m.write(AVAIL + 2, &idx.wrapping_add(1).to_le_bytes())
            .unwrap();
        assert!(d.mmio_write(0x50, 0, m), "interrupt raised");
        let mut s = [0u8; 1];
        m.read(st, &mut s).unwrap();
        s[0]
    }

    #[test]
    fn write_then_read_back_through_the_ring() {
        let (f, p) = disk("rw", 64);
        let mut d = VirtioBlk::new(f, false).unwrap();
        let m = Mem(RefCell::new(vec![0u8; 0x10000]));
        bring_up(&mut d, &m);
        assert_eq!(d.mmio_read(0x100), 64, "capacity in sectors");
        m.write(0x8000, &[0xa5; 1024]).unwrap();
        assert_eq!(submit(&mut d, &m, 0, T_OUT, 3, 0x8000, 1024), S_OK);
        assert_eq!(submit(&mut d, &m, 3, T_IN, 3, 0x9000, 1024), S_OK);
        let mut back = [0u8; 1024];
        m.read(0x9000, &mut back).unwrap();
        assert_eq!(back, [0xa5; 1024]);
        assert_eq!(VirtioBlk::rd16(&m, USED + 2), Some(2));
        assert_eq!(d.mmio_read(0x60), 1);
        d.mmio_write(0x64, 1, &m);
        assert_eq!(d.mmio_read(0x60), 0);
        let on_disk = std::fs::read(&p).unwrap();
        assert_eq!(&on_disk[3 * 512..5 * 512], &[0xa5; 1024][..]);
        assert_eq!(submit(&mut d, &m, 0, T_FLUSH, 0, 0x8000, 0), S_OK);
        std::fs::remove_file(p).unwrap();
    }

    #[test]
    fn refuses_out_of_range_readonly_and_bad_features() {
        let (f, p) = disk("ro", 8);
        let mut d = VirtioBlk::new(f, true).unwrap();
        let m = Mem(RefCell::new(vec![0u8; 0x10000]));
        bring_up(&mut d, &m);
        assert_eq!(
            submit(&mut d, &m, 0, T_IN, 7, 0x8000, 1024),
            S_IOERR,
            "past the end"
        );
        assert_eq!(
            submit(&mut d, &m, 3, T_OUT, 0, 0x8000, 512),
            S_IOERR,
            "read-only"
        );
        assert_eq!(submit(&mut d, &m, 0, 99, 0, 0x8000, 512), S_UNSUPP);
        d.mmio_write(0x70, 0, &m);
        assert_eq!(d.mmio_read(0x70), 0, "reset");
        d.mmio_write(0x24, 0, &m);
        d.mmio_write(0x20, 1 << 3, &m); // a feature we never offered
        d.mmio_write(0x70, 1 | 2 | 8, &m);
        assert_eq!(d.mmio_read(0x70) & 8, 0, "FEATURES_OK refused");
        std::fs::remove_file(p).unwrap();
    }

    #[test]
    fn malformed_chain_fails_the_device_not_memory() {
        let (f, p) = disk("bad", 8);
        let mut d = VirtioBlk::new(f, false).unwrap();
        let m = Mem(RefCell::new(vec![0u8; 0x10000]));
        bring_up(&mut d, &m);
        // A descriptor loop: 0 -> 0.
        desc(&m, 0, 0x4000, 16, DESC_F_NEXT, 0);
        m.write(AVAIL + 4, &0u16.to_le_bytes()).unwrap();
        m.write(AVAIL + 2, &1u16.to_le_bytes()).unwrap();
        assert!(!d.mmio_write(0x50, 0, &m));
        assert_eq!(d.mmio_read(0x70) & STATUS_FAILED, STATUS_FAILED);
        std::fs::remove_file(p).unwrap();
    }
}
