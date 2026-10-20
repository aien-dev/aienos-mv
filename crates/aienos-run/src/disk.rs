//! The persistent per-user disk that holds the AienOS Store. It survives `--update`
//! (only the image changes) and is recreated empty by `--reset`.
//!
//! The disk is GPT with one empty AIENOS partition: the kernel writes only inside that
//! partition and refuses a disk without one ("disk: no AIENOS partition, refusing
//! writes", native/kernel/dev/disk_part.h), so a blank file would never hold a Store.

use std::io;
use std::path::Path;

/// Default Store disk size: 1 GiB, sparse where the host filesystem allows.
pub const DEFAULT_STORE_BYTES: u64 = 1 << 30;

/// Creates the disk if it is missing. Returns true when it was created.
pub fn ensure(path: &Path, bytes: u64) -> io::Result<bool> {
    if path.exists() {
        return Ok(false);
    }
    crate::esp::ensure_store_disk(path, bytes >> 20).map_err(io::Error::other)?;
    Ok(true)
}

/// Deletes and recreates the disk empty.
pub fn reset(path: &Path, bytes: u64) -> io::Result<()> {
    match std::fs::remove_file(path) {
        Ok(()) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => {}
        Err(e) => return Err(e),
    }
    ensure(path, bytes).map(|_| ())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn create_keep_reset() {
        let d = std::env::temp_dir().join(format!("aienos-run-disk-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let p = d.join("store.img");
        assert!(ensure(&p, 16 << 20).unwrap());
        let fresh = std::fs::read(&p).unwrap();
        assert_eq!(fresh.len(), 16 << 20);
        assert_eq!(&fresh[512..520], b"EFI PART", "GPT header at LBA 1");
        assert_eq!(&fresh[1024..1040], &crate::esp::AIENOS_TYPE, "first entry is AIENOS");
        std::fs::write(&p, b"data").unwrap();
        assert!(!ensure(&p, 16 << 20).unwrap(), "an existing disk is kept");
        assert_eq!(std::fs::read(&p).unwrap(), b"data");
        reset(&p, 16 << 20).unwrap();
        assert_eq!(std::fs::read(&p).unwrap(), fresh, "reset gives the same empty disk");
        std::fs::remove_dir_all(&d).unwrap();
    }
}
