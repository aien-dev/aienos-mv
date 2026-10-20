//! The QEMU runtime for Windows (candidate A of ledger L261): one zip, built reproducibly
//! by `scripts/aienos_run_qemu_bundle.sh` from pinned MSYS2 packages, holding
//! `aienos-qemu/qemu-system-x86_64.exe`, its DLLs, the x86 UEFI firmware and the licences.
//!
//! aienos-run.exe carries the zip's size and SHA-256 ([`QEMU_BUNDLE`]); on first use it
//! downloads the zip (next to the manifest unless `AIENOS_RUN_QEMU_BUNDLE` names another
//! URL or file), verifies it, and unpacks it into `<cache>\aienos-qemu` with Windows' own
//! `tar.exe` (bsdtar, which reads zip; in Windows since 10 1803). A stamp file records
//! which zip the folder came from, so a new aienos-run with a new pin replaces it.

use crate::fetch;
use crate::manifest::{hex_decode, hex_encode, resolve_url, FileEntry};
use std::path::{Path, PathBuf};
use std::process::Command;

/// A file pinned into the executable at build time.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Pin {
    pub name: &'static str,
    pub size: u64,
    pub sha256_hex: &'static str,
}

/// `scripts/aienos_run_qemu_bundle.sh` output, built three times on the MacBook with the
/// same bytes (QEMU 11.1.2, MSYS2 ucrt64; lock `crates/aienos-run/qemu-bundle.lock`).
pub const QEMU_BUNDLE: Pin = Pin {
    name: "aienos-qemu-win64.zip",
    size: 99_782_827,
    sha256_hex: "a88479b9c084a6edc6ba70849256524e81fbcd0474d07fa7947503e25d0f7056",
};

/// Folder (inside the zip and under the cache) and the stamp file inside it.
pub const DIR: &str = crate::backend::qemu::BUNDLE_DIR;
pub const STAMP: &str = "aienos-bundle.sha256";

impl Pin {
    pub fn entry(&self, url: &str) -> FileEntry {
        FileEntry {
            name: self.name.to_string(),
            size: self.size,
            sha256: hex_decode::<32>(self.sha256_hex).expect("pinned sha256 is 64 hex digits"),
            url: url.to_string(),
        }
    }
}

/// Where the zip comes from: `AIENOS_RUN_QEMU_BUNDLE` (URL or path) if set, else next to
/// the manifest.
pub fn source_url(override_url: Option<&str>, manifest_url: &str, pin: &Pin) -> String {
    match override_url.filter(|s| !s.is_empty()) {
        Some(u) => u.to_string(),
        None => resolve_url(manifest_url, pin.name),
    }
}

/// True when `<cache>/aienos-qemu` holds the unpacked zip `pin` names.
pub fn installed(cache: &Path, pin: &Pin) -> bool {
    let stamp = cache.join(DIR).join(STAMP);
    std::fs::read_to_string(stamp)
        .map(|s| s.trim() == pin.sha256_hex)
        .unwrap_or(false)
}

/// Unpacks `zip` into `dest` with the system `tar` (bsdtar on Windows and macOS).
pub fn tar_extract(zip: &Path, dest: &Path) -> Result<(), String> {
    let st = Command::new("tar")
        .arg("-xf")
        .arg(zip)
        .arg("-C")
        .arg(dest)
        .status()
        .map_err(|e| format!("cannot start tar: {e}"))?;
    if st.success() {
        Ok(())
    } else {
        Err(format!("tar exit {st}"))
    }
}

/// Makes sure the bundle is unpacked in `cache`; returns the folder. Downloads and verifies
/// the zip when needed (`extract` unpacks it: [`tar_extract`] outside tests). The zip is
/// deleted after a successful unpack; the folder is swapped in only when complete.
pub fn ensure(
    cache: &Path,
    url: &str,
    pin: &Pin,
    extract: &dyn Fn(&Path, &Path) -> Result<(), String>,
    say: &dyn Fn(&str),
) -> Result<PathBuf, String> {
    let dir = cache.join(DIR);
    if installed(cache, pin) {
        return Ok(dir);
    }
    say(&format!(
        "getting the QEMU runtime ({} MB, once): {url}",
        pin.size / 1_000_000
    ));
    let (zip, _) = fetch::fetch_verified(url, &pin.entry(url), cache)
        .map_err(|e| format!("{}: {e}", pin.name))?;
    say(&format!(
        "{} sha256 {} verified; unpacking",
        pin.name, pin.sha256_hex
    ));
    let tmp = cache.join(format!("{DIR}.unpack"));
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
    extract(&zip, &tmp)?;
    let unpacked = tmp.join(DIR);
    if !unpacked.join("qemu-system-x86_64.exe").is_file() {
        let _ = std::fs::remove_dir_all(&tmp);
        return Err(format!("{} has no {DIR}/qemu-system-x86_64.exe", pin.name));
    }
    std::fs::write(unpacked.join(STAMP), format!("{}\n", pin.sha256_hex))
        .map_err(|e| e.to_string())?;
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::rename(&unpacked, &dir).map_err(|e| e.to_string())?;
    let _ = std::fs::remove_dir_all(&tmp);
    let _ = std::fs::remove_file(&zip);
    Ok(dir)
}

/// Hex of a file's SHA-256 (for `--where` and the docs).
pub fn sha256_hex(p: &Path) -> Option<String> {
    fetch::sha256_file(p).ok().map(|(h, _)| hex_encode(&h))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    fn tmpdir(tag: &str) -> PathBuf {
        let d =
            std::env::temp_dir().join(format!("aienos-run-bundle-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    /// A pin for a small stand-in "zip" whose bytes the fake extractor ignores.
    /// Hashes through a file inside the calling test's own folder (tests run in parallel).
    fn pin_for(dir: &Path, bytes: &[u8]) -> (Pin, String) {
        let p = dir.join("pin-src");
        std::fs::write(&p, bytes).unwrap();
        let sha = sha256_hex(&p).unwrap();
        std::fs::remove_file(&p).unwrap();
        let leaked: &'static str = Box::leak(sha.clone().into_boxed_str());
        (
            Pin {
                name: "aienos-qemu-win64.zip",
                size: bytes.len() as u64,
                sha256_hex: leaked,
            },
            sha,
        )
    }

    fn fake_extract(zip: &Path, dest: &Path) -> Result<(), String> {
        assert!(zip.is_file());
        let d = dest.join(DIR);
        std::fs::create_dir_all(d.join("share")).unwrap();
        std::fs::write(d.join("qemu-system-x86_64.exe"), b"MZ").unwrap();
        Ok(())
    }

    #[test]
    fn pinned_bundle_is_well_formed() {
        let e = QEMU_BUNDLE.entry("https://x/y.zip");
        assert_eq!(hex_encode(&e.sha256), QEMU_BUNDLE.sha256_hex);
        assert_eq!(e.size, 99_782_827);
    }

    #[test]
    fn source_next_to_manifest_or_override() {
        assert_eq!(
            source_url(
                None,
                "https://www.aienos.com/try/manifest.txt",
                &QEMU_BUNDLE
            ),
            "https://www.aienos.com/try/aienos-qemu-win64.zip"
        );
        assert_eq!(
            source_url(Some("file:///z.zip"), "https://a/m.txt", &QEMU_BUNDLE),
            "file:///z.zip"
        );
        assert_eq!(
            source_url(Some(""), "https://a/m.txt", &QEMU_BUNDLE),
            "https://a/aienos-qemu-win64.zip"
        );
    }

    #[test]
    fn download_verify_unpack_then_reuse() {
        let d = tmpdir("ensure");
        let src = d.join("src.zip");
        std::fs::write(&src, b"pretend zip").unwrap();
        let (pin, sha) = pin_for(&d, b"pretend zip");
        let cache = d.join("cache");
        let calls = Cell::new(0);
        let ex = |z: &Path, t: &Path| {
            calls.set(calls.get() + 1);
            fake_extract(z, t)
        };
        let dir = ensure(&cache, src.to_str().unwrap(), &pin, &ex, &|_| {}).unwrap();
        assert_eq!(dir, cache.join(DIR));
        assert!(dir.join("qemu-system-x86_64.exe").is_file());
        assert_eq!(
            std::fs::read_to_string(dir.join(STAMP)).unwrap().trim(),
            sha
        );
        assert!(!cache.join(pin.name).exists(), "zip removed after unpack");
        assert!(installed(&cache, &pin));
        // Second call: nothing downloaded or unpacked.
        ensure(&cache, "/does/not/exist", &pin, &ex, &|_| {}).unwrap();
        assert_eq!(calls.get(), 1);
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn tampered_zip_is_refused_and_nothing_installed() {
        let d = tmpdir("tamper");
        let src = d.join("src.zip");
        std::fs::write(&src, b"pretend zip").unwrap();
        let (pin, _) = pin_for(&d, b"pretend zap");
        let cache = d.join("cache");
        let e = ensure(&cache, src.to_str().unwrap(), &pin, &fake_extract, &|_| {}).unwrap_err();
        assert!(e.contains("sha256 mismatch"), "{e}");
        assert!(!cache.join(DIR).exists());
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn zip_without_qemu_is_refused() {
        let d = tmpdir("empty");
        let src = d.join("src.zip");
        std::fs::write(&src, b"pretend zip").unwrap();
        let (pin, _) = pin_for(&d, b"pretend zip");
        let cache = d.join("cache");
        let nothing = |_: &Path, _: &Path| Ok(());
        let e = ensure(&cache, src.to_str().unwrap(), &pin, &nothing, &|_| {}).unwrap_err();
        assert!(e.contains("has no"), "{e}");
        assert!(!installed(&cache, &pin));
        std::fs::remove_dir_all(&d).unwrap();
    }
}
