//! Where aienos-run keeps its files on each host.
//!
//! - Linux: images in `$XDG_CACHE_HOME/aienos-run` (default `~/.cache/aienos-run`), the
//!   persistent Store disk and firmware variables in `$XDG_DATA_HOME/aienos-run`
//!   (default `~/.local/share/aienos-run`).
//! - Windows: `%LOCALAPPDATA%\AienOS\cache` and `%LOCALAPPDATA%\AienOS\data`.
//! - macOS: `~/Library/Caches/AienOS` and `~/Library/Application Support/AienOS`.
//!
//! `AIENOS_RUN_HOME=<dir>` overrides all of them with `<dir>/cache` and `<dir>/data`.

use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Paths {
    pub cache: PathBuf,
    pub data: PathBuf,
}

impl Paths {
    pub fn from_env(get: impl Fn(&str) -> Option<String>) -> Result<Paths, String> {
        let non_empty = |k: &str| get(k).filter(|v| !v.is_empty());
        if let Some(home) = non_empty("AIENOS_RUN_HOME") {
            let h = PathBuf::from(home);
            return Ok(Paths {
                cache: h.join("cache"),
                data: h.join("data"),
            });
        }
        if cfg!(windows) {
            let base = non_empty("LOCALAPPDATA").ok_or("LOCALAPPDATA is not set")?;
            let b = PathBuf::from(base).join("AienOS");
            return Ok(Paths {
                cache: b.join("cache"),
                data: b.join("data"),
            });
        }
        let home = PathBuf::from(non_empty("HOME").ok_or("HOME is not set")?);
        if cfg!(target_os = "macos") {
            return Ok(Paths {
                cache: home.join("Library/Caches/AienOS"),
                data: home.join("Library/Application Support/AienOS"),
            });
        }
        let cache = non_empty("XDG_CACHE_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| home.join(".cache"));
        let data = non_empty("XDG_DATA_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| home.join(".local/share"));
        Ok(Paths {
            cache: cache.join("aienos-run"),
            data: data.join("aienos-run"),
        })
    }

    pub fn images(&self) -> PathBuf {
        self.cache.join("images")
    }
    pub fn manifest_copy(&self) -> PathBuf {
        self.cache.join("manifest.txt")
    }
    pub fn store_disk(&self) -> PathBuf {
        self.data.join("store.img")
    }
    pub fn firmware_vars(&self) -> PathBuf {
        self.data.join("efi-vars.fd")
    }
    pub fn image_file(&self, name: &str) -> PathBuf {
        self.images().join(name)
    }
}

pub fn ensure_dir(p: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(p)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn env<'a>(pairs: &'a [(&'a str, &'a str)]) -> impl Fn(&str) -> Option<String> + 'a {
        move |k| {
            pairs
                .iter()
                .find(|(n, _)| *n == k)
                .map(|(_, v)| v.to_string())
        }
    }

    #[test]
    fn override_wins() {
        let p = Paths::from_env(env(&[("AIENOS_RUN_HOME", "/x"), ("HOME", "/h")])).unwrap();
        assert_eq!(p.cache, PathBuf::from("/x/cache"));
        assert_eq!(p.store_disk(), PathBuf::from("/x/data/store.img"));
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn linux_xdg_defaults() {
        let p = Paths::from_env(env(&[("HOME", "/h")])).unwrap();
        assert_eq!(p.cache, PathBuf::from("/h/.cache/aienos-run"));
        assert_eq!(p.data, PathBuf::from("/h/.local/share/aienos-run"));
        let p = Paths::from_env(env(&[
            ("HOME", "/h"),
            ("XDG_CACHE_HOME", "/c"),
            ("XDG_DATA_HOME", "/d"),
        ]))
        .unwrap();
        assert_eq!(p.images(), PathBuf::from("/c/aienos-run/images"));
        assert_eq!(p.data, PathBuf::from("/d/aienos-run"));
        assert!(Paths::from_env(env(&[])).is_err());
    }
}
