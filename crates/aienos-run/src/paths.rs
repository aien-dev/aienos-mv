//! Per-user paths. macOS: ~/Library/Application Support/AienOS; Linux:
//! $XDG_DATA_HOME/aienos (~/.local/share/aienos); Windows: %LOCALAPPDATA%\AienOS.
//! AIENOS_RUN_HOME overrides all of them.

use std::path::{Path, PathBuf};

pub struct Paths {
    pub home: PathBuf,
    pub boot_disk: PathBuf,
    pub store_disk: PathBuf,
    pub console_log: PathBuf,
}

pub fn default_home() -> PathBuf {
    if let Some(h) = std::env::var_os("AIENOS_RUN_HOME") {
        return PathBuf::from(h);
    }
    let home = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
    if cfg!(target_os = "macos") {
        home.join("Library/Application Support/AienOS")
    } else if cfg!(windows) {
        std::env::var_os("LOCALAPPDATA").map(PathBuf::from).unwrap_or(home).join("AienOS")
    } else {
        std::env::var_os("XDG_DATA_HOME").map(PathBuf::from).unwrap_or_else(|| home.join(".local/share")).join("aienos")
    }
}

impl Paths {
    pub fn for_user() -> Self {
        Self::at(default_home())
    }

    pub fn at(home: PathBuf) -> Self {
        Paths {
            boot_disk: home.join("aienos-boot-aarch64.img"),
            store_disk: home.join("aienos-store.img"),
            console_log: home.join("console.log"),
            home,
        }
    }

    /// EFI variables are per backend (VZ and edk2 use different formats).
    pub fn vars(&self, backend: &str) -> PathBuf {
        self.home.join(format!("efi-vars-{backend}.bin"))
    }

    pub fn ensure(&self) -> Result<(), String> {
        std::fs::create_dir_all(&self.home).map_err(|e| format!("{}: {e}", self.home.display()))
    }

    pub fn reset_store(&self) -> Result<(), String> {
        for p in [self.store_disk.clone(), self.vars("vz"), self.vars("qemu")] {
            match std::fs::remove_file(&p) {
                Ok(()) => {}
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(format!("{}: {e}", p.display())),
            }
        }
        Ok(())
    }
}

/// A sparse blank disk of `mib` MiB, created only if `p` does not exist.
pub fn ensure_blank_disk(p: &Path, mib: u64) -> Result<(), String> {
    if p.exists() {
        return Ok(());
    }
    if let Some(d) = p.parent() {
        std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
    }
    let f = std::fs::File::create(p).map_err(|e| format!("{}: {e}", p.display()))?;
    f.set_len(mib << 20).map_err(|e| format!("{}: {e}", p.display()))
}

/// Locally administered unicast MAC, stable per user home (FNV-1a of the path).
pub fn stable_mac(home: &Path) -> String {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in home.to_string_lossy().bytes() {
        h ^= b as u64;
        h = h.wrapping_mul(0x0100_0000_01b3);
    }
    let b = h.to_le_bytes();
    format!("02:a1:{:02x}:{:02x}:{:02x}:{:02x}", b[0], b[1], b[2], b[3])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layout_under_home() {
        let p = Paths::at("/h".into());
        assert_eq!(p.store_disk, PathBuf::from("/h/aienos-store.img"));
        assert_eq!(p.vars("vz"), PathBuf::from("/h/efi-vars-vz.bin"));
    }

    #[test]
    fn mac_is_stable_local_unicast() {
        let a = stable_mac(Path::new("/Users/x/Library/Application Support/AienOS"));
        assert_eq!(a, stable_mac(Path::new("/Users/x/Library/Application Support/AienOS")));
        assert!(a.starts_with("02:a1:"));
        assert_eq!(a.len(), 17);
    }

    #[test]
    fn blank_disk_and_reset() {
        let d = std::env::temp_dir().join(format!("aienos-run-test-{}", std::process::id()));
        let p = Paths::at(d.clone());
        p.ensure().unwrap();
        ensure_blank_disk(&p.store_disk, 16).unwrap();
        assert_eq!(std::fs::metadata(&p.store_disk).unwrap().len(), 16 << 20);
        p.reset_store().unwrap();
        assert!(!p.store_disk.exists());
        p.reset_store().unwrap(); // idempotent
        std::fs::remove_dir_all(d).unwrap();
    }
}
