//! Reads the kernel (EFI/BOOT/BOOTX64.EFI, the UEFI removable-media path) out of a disk
//! image, read-only, so the own VMM boots the same image file QEMU and OVMF boot.
//!
//! Containers: raw, or qcow2 v2/v3 without a backing file, encryption, compression of
//! the clusters read, an external data file or extended L2 entries (QEMU
//! docs/interop/qcow2.txt). Partitions: GPT (EFI system partition first), MBR (type 0xEF
//! first), or a bare FAT volume. File systems: FAT12, FAT16, FAT32, 8.3 names (Microsoft
//! FAT specification: the FAT type follows from the cluster count).

use crate::manifest::ImageKind;
use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

/// Larger than any kernel we ship; bounds what a corrupt image can make us allocate.
pub const MAX_EFI_BYTES: u64 = 64 << 20;
const BOOT_PATH: [&str; 3] = ["EFI", "BOOT", "BOOTX64.EFI"];
const ESP_GUID: [u8; 16] = [
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
];

/// Byte-addressed read access to the guest-visible disk.
pub trait Blocks {
    fn read_at(&mut self, off: u64, buf: &mut [u8]) -> Result<(), String>;
}

pub struct Raw<R>(pub R);

impl<R: Read + Seek> Blocks for Raw<R> {
    fn read_at(&mut self, off: u64, buf: &mut [u8]) -> Result<(), String> {
        self.0
            .seek(SeekFrom::Start(off))
            .and_then(|_| self.0.read_exact(buf))
            .map_err(|e| format!("disk read at 0x{off:x}: {e}"))
    }
}

pub struct Qcow2<R> {
    r: R,
    version: u32,
    cluster_bits: u32,
    size: u64,
    l1: Vec<u64>,
    l2: HashMap<u64, Vec<u64>>,
}

const QCOW_OFFSET_MASK: u64 = 0x00ff_ffff_ffff_fe00;
const QCOW_COMPRESSED: u64 = 1 << 62;
/// Incompatible feature bits we can read through: dirty (0) and compression type (3).
const QCOW_INCOMPAT_OK: u64 = 1 | 1 << 3;

impl<R: Read + Seek> Qcow2<R> {
    pub fn open(mut r: R) -> Result<Qcow2<R>, String> {
        let mut h = [0u8; 104];
        Raw(&mut r).read_at(0, &mut h[..72])?;
        let be32 = |o: usize| u32::from_be_bytes(h[o..o + 4].try_into().expect("4"));
        let be64 = |o: usize| u64::from_be_bytes(h[o..o + 8].try_into().expect("8"));
        if &h[..4] != b"QFI\xfb" {
            return Err("not a qcow2 image".into());
        }
        let version = be32(4);
        if !(2..=3).contains(&version) {
            return Err(format!("qcow2 version {version} not supported"));
        }
        if be64(8) != 0 {
            return Err("qcow2 with a backing file is not supported".into());
        }
        let cluster_bits = be32(20);
        if !(9..=21).contains(&cluster_bits) {
            return Err(format!("qcow2 cluster_bits {cluster_bits} out of range"));
        }
        if be32(32) != 0 {
            return Err("encrypted qcow2 is not supported".into());
        }
        let (size, l1_size, l1_off) = (be64(24), be32(36) as usize, be64(40));
        if version == 3 {
            Raw(&mut r).read_at(72, &mut h[72..80])?;
            let incompat = u64::from_be_bytes(h[72..80].try_into().expect("8"));
            if incompat & !QCOW_INCOMPAT_OK != 0 {
                return Err(format!(
                    "qcow2 incompatible features 0x{incompat:x} not supported"
                ));
            }
        }
        if l1_size > (4 << 20) {
            return Err("qcow2 L1 table too large".into());
        }
        let mut raw = vec![0u8; l1_size * 8];
        Raw(&mut r).read_at(l1_off, &mut raw)?;
        let l1 = raw
            .chunks_exact(8)
            .map(|b| u64::from_be_bytes(b.try_into().expect("8")))
            .collect();
        Ok(Qcow2 {
            r,
            version,
            cluster_bits,
            size,
            l1,
            l2: HashMap::new(),
        })
    }

    /// Host offset of the guest cluster holding `off`, or None for a zero cluster.
    fn map(&mut self, off: u64) -> Result<Option<u64>, String> {
        let l2_bits = self.cluster_bits - 3;
        let l1i = (off >> (self.cluster_bits + l2_bits)) as usize;
        let l2i = ((off >> self.cluster_bits) & ((1 << l2_bits) - 1)) as usize;
        let l2_off = self.l1.get(l1i).map_or(0, |e| e & QCOW_OFFSET_MASK);
        if l2_off == 0 {
            return Ok(None);
        }
        if !self.l2.contains_key(&l2_off) {
            let mut raw = vec![0u8; 1 << self.cluster_bits];
            Raw(&mut self.r).read_at(l2_off, &mut raw)?;
            let t = raw
                .chunks_exact(8)
                .map(|b| u64::from_be_bytes(b.try_into().expect("8")))
                .collect();
            self.l2.insert(l2_off, t);
        }
        let e = self.l2[&l2_off][l2i];
        if e & QCOW_COMPRESSED != 0 {
            return Err(format!(
                "qcow2 cluster at 0x{off:x} is compressed (not supported)"
            ));
        }
        let host = e & QCOW_OFFSET_MASK;
        Ok((host != 0 && !(self.version == 3 && e & 1 != 0)).then_some(host))
    }
}

impl<R: Read + Seek> Blocks for Qcow2<R> {
    fn read_at(&mut self, mut off: u64, buf: &mut [u8]) -> Result<(), String> {
        if off
            .checked_add(buf.len() as u64)
            .is_none_or(|e| e > self.size)
        {
            return Err(format!("read at 0x{off:x} past the end of the qcow2 disk"));
        }
        let cs = 1u64 << self.cluster_bits;
        let mut done = 0;
        while done < buf.len() {
            let n = ((cs - (off & (cs - 1))) as usize).min(buf.len() - done);
            let part = &mut buf[done..done + n];
            match self.map(off)? {
                Some(host) => Raw(&mut self.r).read_at(host + (off & (cs - 1)), part)?,
                None => part.fill(0),
            }
            done += n;
            off += n as u64;
        }
        Ok(())
    }
}

/// Byte offsets of candidate FAT volumes, most likely first.
fn volumes(d: &mut dyn Blocks) -> Result<Vec<u64>, String> {
    let mut s0 = [0u8; 512];
    d.read_at(0, &mut s0)?;
    let mut out = Vec::new();
    if s0[510..512] != [0x55, 0xaa] {
        return Ok(out);
    }
    let parts: Vec<&[u8]> = (0..4).map(|i| &s0[446 + 16 * i..462 + 16 * i]).collect();
    let lba = |p: &[u8]| u64::from(u32::from_le_bytes(p[8..12].try_into().expect("4")));
    if parts.iter().any(|p| p[4] == 0xee) {
        let mut h = [0u8; 92];
        d.read_at(512, &mut h)?;
        if &h[..8] == b"EFI PART" {
            let first = u64::from_le_bytes(h[72..80].try_into().expect("8"));
            let count = u32::from_le_bytes(h[80..84].try_into().expect("4")).min(256) as usize;
            let esize = u32::from_le_bytes(h[84..88].try_into().expect("4")) as usize;
            if (128..=1024).contains(&esize) {
                let mut t = vec![0u8; count * esize];
                d.read_at(first * 512, &mut t)?;
                let mut esp = Vec::new();
                for e in t.chunks_exact(esize).filter(|e| e[..16] != [0; 16]) {
                    let start = u64::from_le_bytes(e[32..40].try_into().expect("8")) * 512;
                    if e[..16] == ESP_GUID {
                        esp.push(start);
                    } else {
                        out.push(start);
                    }
                }
                esp.append(&mut out);
                out = esp;
            }
        }
    } else {
        let mut esp: Vec<u64> = Vec::new();
        for p in parts.iter().filter(|p| p[4] != 0 && lba(p) != 0) {
            if p[4] == 0xef {
                esp.push(lba(p) * 512);
            } else {
                out.push(lba(p) * 512);
            }
        }
        esp.append(&mut out);
        out = esp;
    }
    out.push(0); // a bare FAT volume (superfloppy)
    Ok(out)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FatType {
    Fat12,
    Fat16,
    Fat32,
}

struct Fat {
    base: u64,
    kind: FatType,
    cluster_bytes: u64,
    fat: Vec<u8>,
    root: u64,         // FAT12/16: byte offset of the root directory
    root_bytes: u64,   // FAT12/16: its size
    root_cluster: u32, // FAT32
    data: u64,
    clusters: u32,
}

impl Fat {
    fn open(d: &mut dyn Blocks, base: u64) -> Result<Fat, String> {
        let mut b = [0u8; 512];
        d.read_at(base, &mut b)?;
        let u16at = |o: usize| u64::from(u16::from_le_bytes([b[o], b[o + 1]]));
        let u32at = |o: usize| u64::from(u32::from_le_bytes(b[o..o + 4].try_into().expect("4")));
        let bps = u16at(11);
        let spc = u64::from(b[13]);
        let reserved = u16at(14);
        let nfats = u64::from(b[16]);
        let root_entries = u16at(17);
        let total = if u16at(19) != 0 { u16at(19) } else { u32at(32) };
        let fatsz = if u16at(22) != 0 { u16at(22) } else { u32at(36) };
        let valid = b[510..512] == [0x55, 0xaa]
            && [512, 1024, 2048, 4096].contains(&bps)
            && spc.is_power_of_two()
            && reserved > 0
            && (1..=2).contains(&nfats)
            && fatsz > 0;
        if !valid {
            return Err(format!("no FAT volume at 0x{base:x}"));
        }
        let root_secs = (root_entries * 32).div_ceil(bps);
        let meta = reserved + nfats * fatsz + root_secs;
        let clusters = total.checked_sub(meta).ok_or("FAT sizes inconsistent")? / spc;
        let kind = match clusters {
            0..4085 => FatType::Fat12,
            4085..65525 => FatType::Fat16,
            _ => FatType::Fat32,
        };
        let fat_bytes = fatsz * bps;
        if fat_bytes > (64 << 20) {
            return Err("FAT too large".into());
        }
        let mut fat = vec![0u8; fat_bytes as usize];
        d.read_at(base + reserved * bps, &mut fat)?;
        Ok(Fat {
            base,
            kind,
            cluster_bytes: spc * bps,
            fat,
            root: base + (reserved + nfats * fatsz) * bps,
            root_bytes: root_secs * bps,
            root_cluster: u32at(44) as u32,
            data: base + meta * bps,
            clusters: clusters as u32,
        })
    }

    fn next(&self, c: u32) -> Result<Option<u32>, String> {
        let at = |o: usize, n: usize| {
            self.fat
                .get(o..o + n)
                .ok_or_else(|| format!("cluster {c} outside the FAT"))
        };
        let (v, eoc) = match self.kind {
            FatType::Fat12 => {
                let o = c as usize + c as usize / 2;
                let b = at(o, 2)?;
                let v = u32::from(u16::from_le_bytes([b[0], b[1]]));
                (if c & 1 == 1 { v >> 4 } else { v & 0xfff }, 0xff8)
            }
            FatType::Fat16 => {
                let b = at(2 * c as usize, 2)?;
                (u32::from(u16::from_le_bytes([b[0], b[1]])), 0xfff8)
            }
            FatType::Fat32 => {
                let b = at(4 * c as usize, 4)?;
                let v = u32::from_le_bytes(b.try_into().expect("4")) & 0x0fff_ffff;
                (v, 0x0fff_fff8)
            }
        };
        if v >= eoc {
            Ok(None)
        } else if v < 2 || v >= self.clusters + 2 {
            Err(format!("FAT chain broken after cluster {c} (next {v:#x})"))
        } else {
            Ok(Some(v))
        }
    }

    fn read_chain(&self, d: &mut dyn Blocks, first: u32, max: u64) -> Result<Vec<u8>, String> {
        let mut out = Vec::new();
        let mut c = Some(first);
        let mut steps = 0u32;
        while let Some(n) = c {
            if n < 2 || n >= self.clusters + 2 || steps > self.clusters {
                return Err(format!("FAT chain broken at cluster {n}"));
            }
            if out.len() as u64 >= max {
                break;
            }
            let at = out.len();
            out.resize(at + self.cluster_bytes as usize, 0);
            d.read_at(
                self.data + u64::from(n - 2) * self.cluster_bytes,
                &mut out[at..],
            )?;
            c = self.next(n)?;
            steps += 1;
        }
        Ok(out)
    }

    /// (first cluster, size, is_dir) of `name` in a directory's bytes.
    fn lookup(dir: &[u8], name: &str) -> Option<(u32, u64, bool)> {
        let short = short_name(name)?;
        for e in dir.chunks_exact(32) {
            match e[0] {
                0 => return None,
                0xe5 => continue,
                _ if e[11] & 0x0f == 0x0f => continue, // long-name slot
                _ => {}
            }
            if e[11] & 0x08 == 0 && e[..11].eq_ignore_ascii_case(&short) {
                let hi = u32::from(u16::from_le_bytes([e[20], e[21]]));
                let lo = u32::from(u16::from_le_bytes([e[26], e[27]]));
                let size = u64::from(u32::from_le_bytes(e[28..32].try_into().expect("4")));
                return Some((hi << 16 | lo, size, e[11] & 0x10 != 0));
            }
        }
        None
    }

    fn read_path(&self, d: &mut dyn Blocks, path: &[&str]) -> Result<Vec<u8>, String> {
        let mut dir = if self.kind == FatType::Fat32 {
            self.read_chain(d, self.root_cluster, 4 << 20)?
        } else {
            let mut r = vec![0u8; self.root_bytes as usize];
            d.read_at(self.root, &mut r)?;
            r
        };
        for (i, name) in path.iter().enumerate() {
            let (c, size, is_dir) = Fat::lookup(&dir, name).ok_or_else(|| {
                format!(
                    "{} not found on the FAT volume at 0x{:x}",
                    path[..=i].join("/"),
                    self.base
                )
            })?;
            let last = i + 1 == path.len();
            if is_dir == last {
                return Err(format!("{} has the wrong type", path[..=i].join("/")));
            }
            if last {
                if size > MAX_EFI_BYTES || size == 0 {
                    return Err(format!("{} is {size} bytes", path.join("/")));
                }
                let mut f = self.read_chain(d, c, size)?;
                if (f.len() as u64) < size {
                    return Err(format!(
                        "{} is shorter than its directory entry",
                        path.join("/")
                    ));
                }
                f.truncate(size as usize);
                return Ok(f);
            }
            dir = self.read_chain(d, c, 4 << 20)?;
        }
        unreachable!("the path is not empty")
    }
}

/// "BOOTX64.EFI" -> b"BOOTX64 EFI".
fn short_name(name: &str) -> Option<[u8; 11]> {
    let (base, ext) = name.split_once('.').unwrap_or((name, ""));
    if base.is_empty() || base.len() > 8 || ext.len() > 3 || !name.is_ascii() {
        return None;
    }
    let mut s = [b' '; 11];
    s[..base.len()].copy_from_slice(base.to_ascii_uppercase().as_bytes());
    s[8..8 + ext.len()].copy_from_slice(ext.to_ascii_uppercase().as_bytes());
    Some(s)
}

/// EFI/BOOT/BOOTX64.EFI from the first FAT volume on the disk that has it.
pub fn boot_efi_from(d: &mut dyn Blocks) -> Result<Vec<u8>, String> {
    let mut why = Vec::new();
    for base in volumes(d)? {
        match Fat::open(d, base).and_then(|f| f.read_path(d, &BOOT_PATH)) {
            Ok(f) => return Ok(f),
            Err(e) => why.push(e),
        }
    }
    Err(format!(
        "no EFI/BOOT/BOOTX64.EFI on the disk ({})",
        why.join("; ")
    ))
}

/// The kernel bytes for an image of `kind` at `path`.
pub fn boot_efi(path: &Path, kind: ImageKind) -> Result<Vec<u8>, String> {
    let open = || File::open(path).map_err(|e| format!("{}: {e}", path.display()));
    match kind {
        ImageKind::UefiApp => {
            let f = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
            if f.len() as u64 > MAX_EFI_BYTES {
                return Err(format!("{} is larger than {MAX_EFI_BYTES} bytes", path.display()));
            }
            Ok(f)
        }
        ImageKind::DiskRaw => boot_efi_from(&mut Raw(open()?)),
        ImageKind::DiskQcow2 => boot_efi_from(&mut Qcow2::open(open()?)?),
        ImageKind::DiskVhdx | ImageKind::Iso => Err(format!(
            "the own VMM boots UEFI apps, raw and qcow2 disks; this image is {kind:?} (use --backend qemu)"
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    const BPS: usize = 512;

    /// A FAT volume of `bits` holding EFI/BOOT/BOOTX64.EFI = `file`, spread over
    /// non-adjacent clusters, plus a long-name slot and a deleted entry to skip.
    fn fat_volume(bits: u32, file: &[u8]) -> Vec<u8> {
        let (clusters, reserved, root_entries) = match bits {
            12 => (200usize, 1usize, 64usize),
            16 => (5000, 1, 512),
            _ => (66_000, 32, 0),
        };
        let fatsz = ((clusters + 2) * bits as usize / 8).div_ceil(BPS);
        let root_secs = root_entries * 32 / BPS;
        let total = reserved + 2 * fatsz + root_secs + clusters;
        let mut v = vec![0u8; total * BPS];
        let put16 = |v: &mut Vec<u8>, o: usize, x: usize| {
            v[o..o + 2].copy_from_slice(&(x as u16).to_le_bytes())
        };
        let put32 = |v: &mut Vec<u8>, o: usize, x: usize| {
            v[o..o + 4].copy_from_slice(&(x as u32).to_le_bytes())
        };
        v[..3].copy_from_slice(&[0xeb, 0x3c, 0x90]);
        put16(&mut v, 11, BPS);
        v[13] = 1;
        put16(&mut v, 14, reserved);
        v[16] = 2;
        put16(&mut v, 17, root_entries);
        if total < 65536 && bits != 32 {
            put16(&mut v, 19, total);
        } else {
            put32(&mut v, 32, total);
        }
        v[21] = 0xf8;
        if bits == 32 {
            put32(&mut v, 36, fatsz);
            put32(&mut v, 44, 2);
        } else {
            put16(&mut v, 22, fatsz);
        }
        v[510] = 0x55;
        v[511] = 0xaa;
        let eoc = (1usize << bits.min(28)) - 1;
        let set = |v: &mut Vec<u8>, c: usize, x: usize| {
            for f in 0..2 {
                let fat = (reserved + f * fatsz) * BPS;
                match bits {
                    12 => {
                        let o = fat + c + c / 2;
                        let old = u16::from_le_bytes([v[o], v[o + 1]]) as usize;
                        let n = if c & 1 == 1 {
                            (old & 0xf) | x << 4
                        } else {
                            (old & 0xf000) | x
                        };
                        put16(v, o, n);
                    }
                    16 => put16(v, fat + 2 * c, x),
                    _ => put32(v, fat + 4 * c, x),
                }
            }
        };
        let data = (reserved + 2 * fatsz + root_secs) * BPS;
        let clus = |c: usize| data + (c - 2) * BPS;
        let entry =
            |v: &mut Vec<u8>, at: usize, name: &[u8; 11], attr: u8, c: usize, size: usize| {
                v[at..at + 11].copy_from_slice(name);
                v[at + 11] = attr;
                put16(v, at + 20, c >> 16);
                put16(v, at + 26, c & 0xffff);
                put32(v, at + 28, size);
            };
        // root: cluster 2 on FAT32, the fixed area otherwise
        let root = if bits == 32 {
            set(&mut v, 2, eoc);
            clus(2)
        } else {
            (reserved + 2 * fatsz) * BPS
        };
        let (efi, boot) = (3, 4);
        v[root] = 0x41; // long-name slot for "efi"
        v[root + 11] = 0x0f;
        entry(&mut v, root + 32, b"OLD     EFI", 0x20, 9, 1);
        v[root + 32] = 0xe5;
        entry(&mut v, root + 64, b"EFI        ", 0x10, efi, 0);
        set(&mut v, efi, eoc);
        entry(&mut v, clus(efi), b".          ", 0x10, efi, 0);
        entry(&mut v, clus(efi) + 32, b"..         ", 0x10, 0, 0);
        entry(&mut v, clus(efi) + 64, b"BOOT       ", 0x10, boot, 0);
        set(&mut v, boot, eoc);
        let chain: Vec<usize> = (0..file.len().div_ceil(BPS))
            .map(|i| [10, 7, 12, 20, 5][i % 5] + 20 * (i / 5))
            .collect();
        entry(
            &mut v,
            clus(boot),
            b"bootx64 efi",
            0x20,
            chain[0],
            file.len(),
        );
        for (i, c) in chain.iter().enumerate() {
            set(&mut v, *c, chain.get(i + 1).copied().unwrap_or(eoc));
            let part = &file[i * BPS..file.len().min((i + 1) * BPS)];
            let at = clus(*c);
            v[at..at + part.len()].copy_from_slice(part);
        }
        v
    }

    fn file(n: usize) -> Vec<u8> {
        (0..n).map(|i| (i * 7 + i / 300) as u8).collect()
    }

    fn mbr(vol: &[u8], ptype: u8) -> Vec<u8> {
        let mut d = vec![0u8; 2048 * BPS];
        d[446 + 4] = ptype;
        d[446 + 8..446 + 12].copy_from_slice(&2048u32.to_le_bytes());
        d[510] = 0x55;
        d[511] = 0xaa;
        d.extend_from_slice(vol);
        d
    }

    fn gpt(vol: &[u8]) -> Vec<u8> {
        let mut d = mbr(&[], 0xee);
        d[512..520].copy_from_slice(b"EFI PART");
        d[512 + 72..512 + 80].copy_from_slice(&2u64.to_le_bytes());
        d[512 + 80..512 + 84].copy_from_slice(&128u32.to_le_bytes());
        d[512 + 84..512 + 88].copy_from_slice(&128u32.to_le_bytes());
        // entry 0: some other partition with nothing on it; entry 1: the ESP at LBA 2048
        let e0 = 2 * BPS;
        d[e0] = 0xaf;
        d[e0 + 32..e0 + 40].copy_from_slice(&8u64.to_le_bytes());
        let e1 = e0 + 128;
        d[e1..e1 + 16].copy_from_slice(&ESP_GUID);
        d[e1 + 32..e1 + 40].copy_from_slice(&2048u64.to_le_bytes());
        d.extend_from_slice(vol);
        d
    }

    /// The disk as qcow2 v3 (64 KiB clusters), leaving all-zero clusters unallocated.
    fn qcow2(disk: &[u8]) -> Vec<u8> {
        let cs = 65536usize;
        let n = disk.len().div_ceil(cs);
        let l2_tables = n.div_ceil(cs / 8);
        let mut q = vec![0u8; cs * (2 + l2_tables)];
        q[..4].copy_from_slice(b"QFI\xfb");
        q[4..8].copy_from_slice(&3u32.to_be_bytes());
        q[20..24].copy_from_slice(&16u32.to_be_bytes());
        q[24..32].copy_from_slice(&(disk.len() as u64).to_be_bytes());
        q[36..40].copy_from_slice(&(l2_tables as u32).to_be_bytes());
        q[40..48].copy_from_slice(&(cs as u64).to_be_bytes());
        q[100..104].copy_from_slice(&104u32.to_be_bytes());
        for t in 0..l2_tables {
            let at = (2 + t) * cs;
            q[cs + 8 * t..cs + 8 * t + 8].copy_from_slice(&(at as u64 | 1 << 63).to_be_bytes());
        }
        for (i, chunk) in disk.chunks(cs).enumerate() {
            if chunk.iter().all(|b| *b == 0) {
                continue;
            }
            let host = q.len();
            q.extend_from_slice(chunk);
            q.resize(host + cs, 0);
            let l2 = 2 * cs + 8 * i;
            q[l2..l2 + 8].copy_from_slice(&(host as u64 | 1 << 63).to_be_bytes());
        }
        q
    }

    #[test]
    fn short_names() {
        assert_eq!(&short_name("BOOTX64.EFI").unwrap(), b"BOOTX64 EFI");
        assert_eq!(&short_name("efi").unwrap(), b"EFI        ");
        assert!(short_name("averylongname").is_none());
    }

    #[test]
    fn fat12_16_32_bare_mbr_and_gpt() {
        for bits in [12, 16, 32] {
            let f = file(1300 + bits as usize);
            let vol = fat_volume(bits, &f);
            let kind = Fat::open(&mut Raw(Cursor::new(vol.clone())), 0)
                .unwrap()
                .kind;
            let want = match bits {
                12 => FatType::Fat12,
                16 => FatType::Fat16,
                _ => FatType::Fat32,
            };
            assert_eq!(kind, want);
            assert_eq!(
                boot_efi_from(&mut Raw(Cursor::new(vol.clone()))).unwrap(),
                f,
                "bare FAT{bits}"
            );
            let m = mbr(&vol, 0xef);
            assert_eq!(
                boot_efi_from(&mut Raw(Cursor::new(m))).unwrap(),
                f,
                "MBR FAT{bits}"
            );
            let g = gpt(&vol);
            assert_eq!(
                boot_efi_from(&mut Raw(Cursor::new(g))).unwrap(),
                f,
                "GPT FAT{bits}"
            );
        }
    }

    #[test]
    fn qcow2_reads_like_raw() {
        let f = file(5000);
        let disk = gpt(&fat_volume(16, &f));
        let q = qcow2(&disk);
        assert!(q.len() < disk.len(), "zero clusters stay unallocated");
        let mut img = Qcow2::open(Cursor::new(q.clone())).unwrap();
        let mut a = vec![0u8; 200_000];
        img.read_at(1000, &mut a).unwrap();
        assert_eq!(a, disk[1000..201_000]);
        assert!(img.read_at(disk.len() as u64 - 4, &mut [0u8; 8]).is_err());
        assert_eq!(
            boot_efi_from(&mut Qcow2::open(Cursor::new(q.clone())).unwrap()).unwrap(),
            f
        );
        let mut backing = q.clone();
        backing[8..16].copy_from_slice(&512u64.to_be_bytes());
        assert!(Qcow2::open(Cursor::new(backing))
            .err()
            .unwrap()
            .contains("backing"));
        let mut ext_l2 = q;
        ext_l2[79] = 1 << 4;
        assert!(Qcow2::open(Cursor::new(ext_l2))
            .err()
            .unwrap()
            .contains("features"));
    }

    #[test]
    fn broken_images_fail_with_a_reason() {
        let f = file(1500);
        let mut vol = fat_volume(16, &f);
        assert!(boot_efi_from(&mut Raw(Cursor::new(vec![0u8; 4096]))).is_err());
        // the file's second cluster marked free in both FATs: the chain is broken
        let fatsz = 5002 * 2 / BPS + 1;
        for fat in [BPS, BPS + fatsz * BPS] {
            vol[fat + 2 * 7..fat + 2 * 7 + 2].copy_from_slice(&0u16.to_le_bytes());
        }
        let e = boot_efi_from(&mut Raw(Cursor::new(vol))).unwrap_err();
        assert!(e.contains("chain broken"), "{e}");
    }

    /// The stand-in image scripts/aienos_run_standin_image.sh builds (QEMU vvfat: MBR,
    /// FAT16), when this machine has one; it must yield the very EFI file it wraps.
    #[test]
    fn standin_image_if_present() {
        let Some(home) = std::env::var_os("HOME") else {
            return;
        };
        let dir = Path::new(&home).join(".hermes/cache/scratch/aienos-run-work/images");
        let (img, efi) = (
            dir.join("aienos-standin-imackern-x86_64.qcow2"),
            dir.join("IMACKERN.EFI"),
        );
        if !img.exists() || !efi.exists() {
            eprintln!("skipped: no stand-in image at {}", dir.display());
            return;
        }
        let got = boot_efi(&img, ImageKind::DiskQcow2).unwrap();
        assert_eq!(got, std::fs::read(efi).unwrap());
    }
}
