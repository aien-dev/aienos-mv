//! Boot disk writer: a raw GPT disk with one EFI System Partition (FAT16)
//! holding \EFI\BOOT\BOOTAA64.EFI. Pure Rust, no mtools / hdiutil, and
//! deterministic (same input bytes -> same disk sha256), so a published disk
//! and a locally built one can be compared byte for byte.

use std::path::Path;

const SECTOR: usize = 512;
const PART_START: u64 = 2048; // 1 MiB aligned
const ESP_TYPE: [u8; 16] = guid(0xC12A7328, 0xF81F, 0x11D2, [0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B]);
const DISK_GUID: [u8; 16] = guid(0xA1E0_0505, 0x2026, 0x1020, [0x41, 0x49, 0x45, 0x4E, 0x4F, 0x53, 0x44, 0x4B]);
const PART_GUID: [u8; 16] = guid(0xA1E0_0505, 0x2026, 0x1020, [0x41, 0x49, 0x45, 0x4E, 0x45, 0x53, 0x50, 0x31]);

const fn guid(a: u32, b: u16, c: u16, d: [u8; 8]) -> [u8; 16] {
    let a = a.to_le_bytes();
    let b = b.to_le_bytes();
    let c = c.to_le_bytes();
    [a[0], a[1], a[2], a[3], b[0], b[1], c[0], c[1], d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]]
}

pub fn crc32(data: &[u8]) -> u32 {
    let mut c = 0xFFFF_FFFFu32;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0xEDB8_8320 } else { c >> 1 };
        }
    }
    !c
}

/// One file on the ESP: an 8.3 path such as "EFI/BOOT/BOOTAA64.EFI".
pub struct EspFile<'a> {
    pub path: &'a str,
    pub data: &'a [u8],
}

fn short_name(n: &str) -> Result<[u8; 11], String> {
    let up = n.to_ascii_uppercase();
    let (base, ext) = match up.rsplit_once('.') {
        Some((b, e)) => (b.to_string(), e.to_string()),
        None => (up.clone(), String::new()),
    };
    let ok = |s: &str| s.bytes().all(|c| c.is_ascii_alphanumeric() || b"_-~$".contains(&c));
    if base.is_empty() || base.len() > 8 || ext.len() > 3 || !ok(&base) || !ok(&ext) {
        return Err(format!("{n:?} is not an 8.3 name"));
    }
    let mut out = [b' '; 11];
    out[..base.len()].copy_from_slice(base.as_bytes());
    out[8..8 + ext.len()].copy_from_slice(ext.as_bytes());
    Ok(out)
}

fn dirent(name: &[u8; 11], attr: u8, cluster: u16, size: u32) -> [u8; 32] {
    let mut e = [0u8; 32];
    e[..11].copy_from_slice(name);
    e[11] = attr;
    // 2026-01-01 00:00 (fixed: deterministic image).
    let date: u16 = ((2026 - 1980) << 9) | (1 << 5) | 1;
    e[16..18].copy_from_slice(&date.to_le_bytes());
    e[18..20].copy_from_slice(&date.to_le_bytes());
    e[24..26].copy_from_slice(&date.to_le_bytes());
    e[26..28].copy_from_slice(&cluster.to_le_bytes());
    e[28..32].copy_from_slice(&size.to_le_bytes());
    e
}

struct Node {
    name: [u8; 11],
    data: Option<Vec<u8>>, // None = directory
    children: Vec<Node>,
    cluster: u16,
}

/// FAT16 volume image of `sectors` sectors containing `files`.
pub fn fat16_image(sectors: u32, hidden: u32, files: &[EspFile]) -> Result<Vec<u8>, String> {
    const SPC: u32 = 4; // 2 KiB clusters
    const RESERVED: u32 = 4;
    const ROOT_ENTRIES: u32 = 512;
    let root_secs = ROOT_ENTRIES * 32 / SECTOR as u32;
    // FAT size: solve iteratively.
    let mut fat_secs = 1;
    loop {
        let data = sectors - RESERVED - 2 * fat_secs - root_secs;
        let clusters = data / SPC;
        let need = ((clusters + 2) * 2).div_ceil(SECTOR as u32);
        if need <= fat_secs {
            break;
        }
        fat_secs = need;
    }
    let data_start = RESERVED + 2 * fat_secs + root_secs;
    let clusters = (sectors - data_start) / SPC;
    if !(4085..65525).contains(&clusters) {
        return Err(format!("{sectors} sectors gives {clusters} clusters: not a FAT16 size"));
    }

    // Build the tree.
    let mut root = Node { name: [b' '; 11], data: None, children: Vec::new(), cluster: 0 };
    for f in files {
        let parts: Vec<&str> = f.path.split('/').filter(|s| !s.is_empty()).collect();
        let mut cur = &mut root;
        for (i, p) in parts.iter().enumerate() {
            let sn = short_name(p)?;
            let last = i + 1 == parts.len();
            let idx = match cur.children.iter().position(|c| c.name == sn) {
                Some(ix) => ix,
                None => {
                    cur.children.push(Node {
                        name: sn,
                        data: if last { Some(f.data.to_vec()) } else { None },
                        children: Vec::new(),
                        cluster: 0,
                    });
                    cur.children.len() - 1
                }
            };
            cur = &mut cur.children[idx];
        }
    }

    let csize = (SPC as usize) * SECTOR;
    let mut fat = vec![0u16; clusters as usize + 2];
    fat[0] = 0xFFF8;
    fat[1] = 0xFFFF;
    let mut data_area: Vec<u8> = Vec::new();
    let mut next: u16 = 2;
    // Allocate clusters: directories first (breadth), then file data.
    fn alloc(fat: &mut [u16], next: &mut u16, n: usize) -> Result<u16, String> {
        if n == 0 {
            return Ok(0);
        }
        let first = *next;
        for i in 0..n {
            let c = *next as usize + i;
            if c >= fat.len() {
                return Err("ESP too small for the files".into());
            }
            fat[c] = if i + 1 == n { 0xFFFF } else { (c + 1) as u16 };
        }
        *next += n as u16;
        Ok(first)
    }
    fn assign(n: &mut Node, fat: &mut [u16], next: &mut u16, csize: usize) -> Result<(), String> {
        for c in n.children.iter_mut() {
            let len = match &c.data {
                Some(d) => d.len().div_ceil(csize),
                None => ((c.children.len() + 2) * 32).div_ceil(csize),
            };
            c.cluster = alloc(fat, next, len)?;
        }
        for c in n.children.iter_mut() {
            if c.data.is_none() {
                assign(c, fat, next, csize)?;
            }
        }
        Ok(())
    }
    assign(&mut root, &mut fat, &mut next, csize)?;
    data_area.resize((next as usize - 2) * csize, 0);
    fn write(n: &Node, parent: u16, data: &mut [u8], csize: usize) {
        let off = |cl: u16| (cl as usize - 2) * csize;
        if n.cluster != 0 {
            let mut d = Vec::new();
            d.extend_from_slice(&dirent(b".          ", 0x10, n.cluster, 0));
            d.extend_from_slice(&dirent(b"..         ", 0x10, parent, 0));
            for c in &n.children {
                let size = c.data.as_ref().map(|v| v.len() as u32).unwrap_or(0);
                d.extend_from_slice(&dirent(&c.name, if c.data.is_some() { 0x20 } else { 0x10 }, c.cluster, size));
            }
            data[off(n.cluster)..off(n.cluster) + d.len()].copy_from_slice(&d);
        }
        for c in &n.children {
            match &c.data {
                Some(v) if c.cluster != 0 => data[off(c.cluster)..off(c.cluster) + v.len()].copy_from_slice(v),
                Some(_) => {}
                None => write(c, n.cluster, data, csize),
            }
        }
    }
    write(&root, 0, &mut data_area, csize);

    let mut img = vec![0u8; sectors as usize * SECTOR];
    // Boot sector (BPB).
    let b = &mut img[..SECTOR];
    b[0..3].copy_from_slice(&[0xEB, 0x3C, 0x90]);
    b[3..11].copy_from_slice(b"AIENOS  ");
    b[11..13].copy_from_slice(&(SECTOR as u16).to_le_bytes());
    b[13] = SPC as u8;
    b[14..16].copy_from_slice(&(RESERVED as u16).to_le_bytes());
    b[16] = 2;
    b[17..19].copy_from_slice(&(ROOT_ENTRIES as u16).to_le_bytes());
    if sectors < 65536 {
        b[19..21].copy_from_slice(&(sectors as u16).to_le_bytes());
    } else {
        b[32..36].copy_from_slice(&sectors.to_le_bytes());
    }
    b[21] = 0xF8;
    b[22..24].copy_from_slice(&(fat_secs as u16).to_le_bytes());
    b[24..26].copy_from_slice(&32u16.to_le_bytes());
    b[26..28].copy_from_slice(&64u16.to_le_bytes());
    b[28..32].copy_from_slice(&hidden.to_le_bytes());
    b[36] = 0x80;
    b[38] = 0x29;
    b[39..43].copy_from_slice(&0xA1E0_2026u32.to_le_bytes());
    b[43..54].copy_from_slice(b"AIENOS     ");
    b[54..62].copy_from_slice(b"FAT16   ");
    b[510] = 0x55;
    b[511] = 0xAA;
    // FATs.
    let mut fb = Vec::with_capacity(fat.len() * 2);
    for e in &fat {
        fb.extend_from_slice(&e.to_le_bytes());
    }
    for k in 0..2 {
        let o = (RESERVED + k * fat_secs) as usize * SECTOR;
        img[o..o + fb.len()].copy_from_slice(&fb);
    }
    // Root directory.
    let ro = (RESERVED + 2 * fat_secs) as usize * SECTOR;
    let mut r = Vec::new();
    r.extend_from_slice(&dirent(b"AIENOS     ", 0x08, 0, 0)); // volume label
    for c in &root.children {
        let size = c.data.as_ref().map(|v| v.len() as u32).unwrap_or(0);
        r.extend_from_slice(&dirent(&c.name, if c.data.is_some() { 0x20 } else { 0x10 }, c.cluster, size));
    }
    if r.len() > (ROOT_ENTRIES as usize) * 32 {
        return Err("too many root entries".into());
    }
    img[ro..ro + r.len()].copy_from_slice(&r);
    let dstart = data_start as usize * SECTOR;
    img[dstart..dstart + data_area.len()].copy_from_slice(&data_area);
    Ok(img)
}

/// GPT metadata for a disk of `total` sectors with one partition spanning
/// [PART_START, last usable]: protective MBR, primary and backup headers and
/// entry arrays, as (byte offset, bytes) pieces. Returns the pieces and the
/// partition's last LBA.
fn gpt_single(total: u64, ptype: &[u8; 16], disk_guid: &[u8; 16], part_guid: &[u8; 16], name: &str) -> (Vec<(u64, Vec<u8>)>, u64) {
    let last_usable = total - 34;
    let part_end = last_usable; // inclusive
    let mut m = vec![0u8; SECTOR];
    m[446 + 1..446 + 4].copy_from_slice(&[0x00, 0x02, 0x00]);
    m[446 + 4] = 0xEE;
    m[446 + 5..446 + 8].copy_from_slice(&[0xFF, 0xFF, 0xFF]);
    m[446 + 8..446 + 12].copy_from_slice(&1u32.to_le_bytes());
    m[446 + 12..446 + 16].copy_from_slice(&((total - 1).min(0xFFFF_FFFF) as u32).to_le_bytes());
    m[510] = 0x55;
    m[511] = 0xAA;
    // Partition entry array (128 x 128 bytes).
    let mut ents = vec![0u8; 128 * 128];
    ents[0..16].copy_from_slice(ptype);
    ents[16..32].copy_from_slice(part_guid);
    ents[32..40].copy_from_slice(&PART_START.to_le_bytes());
    ents[40..48].copy_from_slice(&part_end.to_le_bytes());
    for (i, ch) in name.encode_utf16().take(36).enumerate() {
        ents[56 + 2 * i..58 + 2 * i].copy_from_slice(&ch.to_le_bytes());
    }
    let ents_crc = crc32(&ents);
    let header = |my: u64, alt: u64, ents_lba: u64| -> Vec<u8> {
        let mut h = vec![0u8; 92];
        h[0..8].copy_from_slice(b"EFI PART");
        h[8..12].copy_from_slice(&0x0001_0000u32.to_le_bytes());
        h[12..16].copy_from_slice(&92u32.to_le_bytes());
        h[24..32].copy_from_slice(&my.to_le_bytes());
        h[32..40].copy_from_slice(&alt.to_le_bytes());
        h[40..48].copy_from_slice(&34u64.to_le_bytes());
        h[48..56].copy_from_slice(&last_usable.to_le_bytes());
        h[56..72].copy_from_slice(disk_guid);
        h[72..80].copy_from_slice(&ents_lba.to_le_bytes());
        h[80..84].copy_from_slice(&128u32.to_le_bytes());
        h[84..88].copy_from_slice(&128u32.to_le_bytes());
        h[88..92].copy_from_slice(&ents_crc.to_le_bytes());
        let c = crc32(&h);
        h[16..20].copy_from_slice(&c.to_le_bytes());
        h
    };
    let s = |lba: u64| lba * SECTOR as u64;
    let pieces = vec![
        (0, m),
        (s(1), header(1, total - 1, 2)),
        (s(2), ents.clone()),
        (s(total - 33), ents),
        (s(total - 1), header(total - 1, 1, total - 33)),
    ];
    (pieces, part_end)
}

/// Raw GPT disk of `mib` MiB with one ESP (FAT16) holding `files`.
pub fn gpt_disk(mib: u64, files: &[EspFile]) -> Result<Vec<u8>, String> {
    let total = mib * 1024 * 1024 / SECTOR as u64;
    let (pieces, part_end) = gpt_single(total, &ESP_TYPE, &DISK_GUID, &PART_GUID, "EFI System Partition");
    let part_secs = (part_end - PART_START + 1) as u32;
    let fat = fat16_image(part_secs, PART_START as u32, files)?;
    let mut d = vec![0u8; (total as usize) * SECTOR];
    for (off, b) in pieces {
        d[off as usize..off as usize + b.len()].copy_from_slice(&b);
    }
    let s = PART_START as usize * SECTOR;
    d[s..s + fat.len()].copy_from_slice(&fat);
    Ok(d)
}

/// AIENOS partition type GUID (native/kernel/dev/disk_part.h): the one
/// partition the kernel's Store stages write.
pub const AIENOS_TYPE: [u8; 16] = guid(0x38DA_AC89, 0x5EAD, 0x4B40, [0x8B, 0x1E, 0x36, 0x87, 0xA7, 0x41, 0x80, 0x61]);
const STORE_DISK_GUID: [u8; 16] = guid(0xA1E0_0505, 0x2026, 0x1020, [0x41, 0x49, 0x45, 0x4E, 0x53, 0x54, 0x52, 0x44]);
const STORE_PART_GUID: [u8; 16] = guid(0xA1E0_0505, 0x2026, 0x1020, [0x41, 0x49, 0x45, 0x4E, 0x53, 0x54, 0x52, 0x31]);

/// The per-user Store disk: a sparse raw disk of `mib` MiB, GPT with one
/// all-zero AIENOS partition (the kernel formats the Store inside it on
/// first boot). Created only when `p` does not exist; never rewritten.
pub fn ensure_store_disk(p: &Path, mib: u64) -> Result<(), String> {
    use std::io::{Seek, SeekFrom, Write};
    if p.exists() {
        return Ok(());
    }
    if mib < 16 {
        return Err(format!("store disk of {mib} MiB is too small (16 MiB minimum)"));
    }
    if let Some(d) = p.parent() {
        std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
    }
    let tmp = p.with_extension("tmp");
    let total = mib * 1024 * 1024 / SECTOR as u64;
    let (pieces, _) = gpt_single(total, &AIENOS_TYPE, &STORE_DISK_GUID, &STORE_PART_GUID, "AIENOS Store");
    let io = |e: std::io::Error| format!("{}: {e}", tmp.display());
    let mut f = std::fs::File::create(&tmp).map_err(io)?;
    f.set_len(mib << 20).map_err(io)?;
    for (off, b) in pieces {
        f.seek(SeekFrom::Start(off)).map_err(io)?;
        f.write_all(&b).map_err(io)?;
    }
    f.sync_all().map_err(io)?;
    std::fs::rename(&tmp, p).map_err(|e| format!("{}: {e}", p.display()))
}

/// Writes a boot disk for one UEFI image at `disk` (atomically via a temp file).
pub fn write_boot_disk(disk: &Path, efi: &Path, mib: u64) -> Result<(), String> {
    let img = std::fs::read(efi).map_err(|e| format!("{}: {e}", efi.display()))?;
    if img.len() < 2 || &img[..2] != b"MZ" {
        return Err(format!("{} is not a PE/UEFI image", efi.display()));
    }
    let d = gpt_disk(mib, &[EspFile { path: "EFI/BOOT/BOOTAA64.EFI", data: &img }])?;
    let tmp = disk.with_extension("tmp");
    std::fs::write(&tmp, &d).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, disk).map_err(|e| format!("{}: {e}", disk.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32_known_vector() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn short_names() {
        assert_eq!(&short_name("BOOTAA64.EFI").unwrap(), b"BOOTAA64EFI");
        assert_eq!(&short_name("efi").unwrap(), b"EFI        ");
        assert!(short_name("BOOTREPORT.TXT").is_err());
        assert!(short_name("a b").is_err());
    }

    #[test]
    fn gpt_disk_layout_and_file_bytes() {
        let payload: Vec<u8> = (0..5000u32).map(|i| (i * 7) as u8).collect();
        let d = gpt_disk(64, &[EspFile { path: "EFI/BOOT/BOOTAA64.EFI", data: &payload }]).unwrap();
        assert_eq!(d.len(), 64 << 20);
        assert_eq!(&d[512..520], b"EFI PART");
        // Header CRC verifies.
        let mut h = d[512..512 + 92].to_vec();
        let want = u32::from_le_bytes(h[16..20].try_into().unwrap());
        h[16..20].fill(0);
        assert_eq!(crc32(&h), want);
        // Backup header at the last LBA.
        assert_eq!(&d[d.len() - 512..d.len() - 504], b"EFI PART");
        // FAT16 BPB at the partition start, payload findable in the data area.
        let p = 2048 * 512;
        assert_eq!(&d[p + 54..p + 62], b"FAT16   ");
        assert_eq!(&d[p + 510..p + 512], &[0x55, 0xAA]);
        assert!(d.windows(payload.len()).any(|w| w == payload.as_slice()));
        // Root dir holds EFI as a directory.
        assert!(d.windows(11).any(|w| w == b"EFI        "));
        assert!(d.windows(11).any(|w| w == b"BOOTAA64EFI"));
    }

    #[test]
    fn deterministic() {
        let a = gpt_disk(32, &[EspFile { path: "EFI/BOOT/BOOTAA64.EFI", data: b"MZxx" }]).unwrap();
        let b = gpt_disk(32, &[EspFile { path: "EFI/BOOT/BOOTAA64.EFI", data: b"MZxx" }]).unwrap();
        assert!(a == b);
    }

    #[test]
    fn store_disk_has_one_aienos_partition() {
        let p = std::env::temp_dir().join(format!("aienos-run-store-{}.img", std::process::id()));
        let _ = std::fs::remove_file(&p);
        ensure_store_disk(&p, 16).unwrap();
        let d = std::fs::read(&p).unwrap();
        assert_eq!(d.len(), 16 << 20);
        assert_eq!(&d[510..512], &[0x55, 0xAA]);
        assert_eq!(&d[512..520], b"EFI PART");
        assert_eq!(&d[1024..1040], &AIENOS_TYPE);
        let total = (16u64 << 20) / 512;
        let b = ((total - 1) * 512) as usize;
        assert_eq!(&d[b..b + 8], b"EFI PART");
        // Never rewritten once it exists (the Store lives in it).
        std::fs::write(&p, b"kept").unwrap();
        ensure_store_disk(&p, 16).unwrap();
        assert_eq!(std::fs::read(&p).unwrap(), b"kept");
        std::fs::remove_file(&p).unwrap();
    }
}
