//! The persistent per-user disk that holds the AienOS Store. It survives `--update`
//! (only the image changes) and is recreated empty by `--reset`.

use std::fs::OpenOptions;
use std::io;
use std::path::Path;

/// Default Store disk size: 1 GiB, sparse where the host filesystem allows.
pub const DEFAULT_STORE_BYTES: u64 = 1 << 30;

/// Creates the disk if it is missing. Returns true when it was created.
pub fn ensure(path: &Path, bytes: u64) -> io::Result<bool> {
    if path.exists() {
        return Ok(false);
    }
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let f = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)?;
    f.set_len(bytes)?;
    f.sync_all()?;
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
        assert!(ensure(&p, 1 << 20).unwrap());
        assert_eq!(std::fs::metadata(&p).unwrap().len(), 1 << 20);
        std::fs::write(&p, b"data").unwrap();
        assert!(!ensure(&p, 1 << 20).unwrap(), "an existing disk is kept");
        assert_eq!(std::fs::read(&p).unwrap(), b"data");
        reset(&p, 1 << 20).unwrap();
        let back = std::fs::read(&p).unwrap();
        assert_eq!(back.len(), 1 << 20);
        assert!(back.iter().all(|b| *b == 0), "reset gives an empty disk");
        std::fs::remove_dir_all(&d).unwrap();
    }
}
