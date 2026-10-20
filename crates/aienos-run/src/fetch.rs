//! Downloading and verifying files named by the manifest.
//!
//! Transport: the host's own `curl` (present on Linux, macOS and Windows 10+), so this
//! program carries no TLS code of its own. Plain paths and `file://` URLs are read
//! directly. Integrity never depends on the transport: every file is checked against
//! the manifest's size and SHA-256 (own FIPS 180-4 code in aienos-crypto) before it
//! is moved into the cache.

use crate::manifest::{hex_encode, FileEntry};
use std::fs::File;
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Debug)]
pub enum FetchError {
    Io(io::Error),
    Download { url: String, status: String },
    SizeMismatch { expected: u64, got: u64 },
    DigestMismatch { expected: String, got: String },
}

impl std::fmt::Display for FetchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FetchError::Io(e) => write!(f, "{e}"),
            FetchError::Download { url, status } => write!(f, "download of {url} failed: {status}"),
            FetchError::SizeMismatch { expected, got } => {
                write!(f, "size mismatch: manifest says {expected} bytes, file has {got}")
            }
            FetchError::DigestMismatch { expected, got } => {
                write!(f, "sha256 mismatch: manifest says {expected}, file is {got}")
            }
        }
    }
}

impl std::error::Error for FetchError {}

impl From<io::Error> for FetchError {
    fn from(e: io::Error) -> Self {
        FetchError::Io(e)
    }
}

/// SHA-256 and length of a file, streamed.
pub fn sha256_file(path: &Path) -> io::Result<([u8; 32], u64)> {
    let mut f = File::open(path)?;
    let mut h = aienos_crypto::sha256::Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    let mut n_total = 0u64;
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
        n_total += n as u64;
    }
    Ok((h.finalize(), n_total))
}

/// Checks a file against a manifest entry.
pub fn verify(path: &Path, entry: &FileEntry) -> Result<(), FetchError> {
    let (digest, len) = sha256_file(path)?;
    if len != entry.size {
        return Err(FetchError::SizeMismatch {
            expected: entry.size,
            got: len,
        });
    }
    if digest != entry.sha256 {
        return Err(FetchError::DigestMismatch {
            expected: hex_encode(&entry.sha256),
            got: hex_encode(&digest),
        });
    }
    Ok(())
}

fn local_path(url: &str) -> Option<PathBuf> {
    if let Some(p) = url.strip_prefix("file://") {
        return Some(PathBuf::from(p));
    }
    if !url.contains("://") {
        return Some(PathBuf::from(url));
    }
    None
}

/// Copies or downloads `url` to `dest` (overwritten).
pub fn download(url: &str, dest: &Path) -> Result<(), FetchError> {
    if let Some(src) = local_path(url) {
        std::fs::copy(&src, dest).map_err(|e| FetchError::Download {
            url: url.to_string(),
            status: e.to_string(),
        })?;
        return Ok(());
    }
    let status = Command::new("curl")
        .args(["-fsSL", "--retry", "3", "--proto", "=https,http", "-o"])
        .arg(dest)
        .arg(url)
        .status()
        .map_err(|e| FetchError::Download {
            url: url.to_string(),
            status: format!("cannot start curl: {e}"),
        })?;
    if !status.success() {
        return Err(FetchError::Download {
            url: url.to_string(),
            status: format!("curl exit {status}"),
        });
    }
    Ok(())
}

/// Reads `url` as text (the manifest).
pub fn read_text(url: &str, scratch: &Path) -> Result<String, FetchError> {
    download(url, scratch)?;
    let text = std::fs::read_to_string(scratch)?;
    let _ = std::fs::remove_file(scratch);
    Ok(text)
}

/// Brings `entry` into `dir`: keeps an existing verified copy, otherwise downloads to a
/// temporary name, verifies, then renames into place. Returns the final path and whether
/// a download happened.
pub fn fetch_verified(
    url: &str,
    entry: &FileEntry,
    dir: &Path,
) -> Result<(PathBuf, bool), FetchError> {
    std::fs::create_dir_all(dir)?;
    let dest = dir.join(&entry.name);
    if dest.exists() && verify(&dest, entry).is_ok() {
        return Ok((dest, false));
    }
    let part = dir.join(format!("{}.part", entry.name));
    download(url, &part)?;
    if let Err(e) = verify(&part, entry) {
        let _ = std::fs::remove_file(&part);
        return Err(e);
    }
    std::fs::rename(&part, &dest)?;
    Ok((dest, true))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmpdir(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("aienos-run-test-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn sha256_known_vector() {
        let d = tmpdir("sha");
        let p = d.join("t");
        std::fs::write(&p, b"test").unwrap();
        let (h, n) = sha256_file(&p).unwrap();
        assert_eq!(n, 4);
        assert_eq!(
            hex_encode(&h),
            "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
        );
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn fetch_verifies_and_refuses_tampering() {
        let d = tmpdir("fetch");
        let src = d.join("src.bin");
        std::fs::write(&src, b"test").unwrap();
        let mut e = FileEntry {
            name: "img.bin".into(),
            size: 4,
            sha256: crate::manifest::hex_decode(
                "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
            )
            .unwrap(),
            url: String::new(),
        };
        let cache = d.join("cache");
        let url = format!("file://{}", src.display());
        let (p, downloaded) = fetch_verified(&url, &e, &cache).unwrap();
        assert!(downloaded);
        assert_eq!(std::fs::read(&p).unwrap(), b"test");
        let (_, again) = fetch_verified(&url, &e, &cache).unwrap();
        assert!(!again, "a verified cached copy is reused");

        // Tampered source: the bad copy never lands in the cache.
        std::fs::write(&src, b"tesT").unwrap();
        e.name = "other.bin".into();
        assert!(matches!(
            fetch_verified(&url, &e, &cache),
            Err(FetchError::DigestMismatch { .. })
        ));
        assert!(!cache.join("other.bin").exists());
        assert!(!cache.join("other.bin.part").exists());
        std::fs::write(&src, b"tests").unwrap();
        assert!(matches!(
            fetch_verified(&url, &e, &cache),
            Err(FetchError::SizeMismatch { .. })
        ));
        std::fs::remove_dir_all(&d).unwrap();
    }
}
