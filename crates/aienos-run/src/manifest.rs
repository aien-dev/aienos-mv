//! The download manifest: which AienOS images and runner binaries exist, where to get
//! them and the SHA-256 each file must have.
//!
//! Format (UTF-8 text, one record per line, `#` starts a comment line):
//!
//! ```text
//! aienos-run-manifest 1
//! channel <name>
//! image <arch> <kind> <file-name> <size-bytes> <sha256-hex> <url>
//! runner <os>-<arch> <file-name> <size-bytes> <sha256-hex> <url>
//! signature ed25519 <public-key-hex> <signature-hex>
//! ```
//!
//! `<url>` may be absolute (`https://`, `file://`) or relative to the manifest's own URL.
//! The optional `signature` line must be last; it signs every byte before it. A signature
//! made with the RFC 8032 TEST 1 key is accepted only when the caller allows test keys,
//! and is always reported as TEST.

use std::fmt;

/// RFC 8032 section 7.1 TEST 1 public key: the repo's labelled TEST signer.
pub const TEST_PUBLIC_KEY_HEX: &str =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Arch {
    Aarch64,
    X86_64,
}

impl Arch {
    pub fn parse(s: &str) -> Option<Arch> {
        match s {
            "aarch64" | "arm64" => Some(Arch::Aarch64),
            "x86_64" | "amd64" | "x64" => Some(Arch::X86_64),
            _ => None,
        }
    }
    pub fn host() -> Arch {
        if cfg!(target_arch = "aarch64") {
            Arch::Aarch64
        } else {
            Arch::X86_64
        }
    }
    pub fn as_str(self) -> &'static str {
        match self {
            Arch::Aarch64 => "aarch64",
            Arch::X86_64 => "x86_64",
        }
    }
}

/// What an image file is, which decides how a backend boots it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ImageKind {
    /// The Aienux kernel as a UEFI application (BOOTAA64.EFI / BOOTX64.EFI).
    UefiApp,
    /// A whole bootable disk (GPT with an EFI system partition), raw bytes.
    DiskRaw,
    /// The same disk as qcow2.
    DiskQcow2,
    /// The same disk as VHDX.
    DiskVhdx,
    /// A hybrid ISO (CD or USB stick).
    Iso,
}

impl ImageKind {
    pub fn parse(s: &str) -> Option<ImageKind> {
        match s {
            "uefi-app" => Some(ImageKind::UefiApp),
            "disk-raw" => Some(ImageKind::DiskRaw),
            "disk-qcow2" => Some(ImageKind::DiskQcow2),
            "disk-vhdx" => Some(ImageKind::DiskVhdx),
            "iso" => Some(ImageKind::Iso),
            _ => None,
        }
    }
    pub fn as_str(self) -> &'static str {
        match self {
            ImageKind::UefiApp => "uefi-app",
            ImageKind::DiskRaw => "disk-raw",
            ImageKind::DiskQcow2 => "disk-qcow2",
            ImageKind::DiskVhdx => "disk-vhdx",
            ImageKind::Iso => "iso",
        }
    }
}

/// One downloadable file with its expected size and digest.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileEntry {
    pub name: String,
    pub size: u64,
    pub sha256: [u8; 32],
    pub url: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImageEntry {
    pub arch: Arch,
    pub kind: ImageKind,
    pub file: FileEntry,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RunnerEntry {
    /// `<os>-<arch>`, for example `linux-aarch64` or `windows-x86_64`.
    pub platform: String,
    pub file: FileEntry,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SignatureState {
    Unsigned,
    /// Signed with the RFC 8032 TEST 1 key (never a release signature).
    TestKey,
    /// Signed with a key from the pinned list.
    Pinned([u8; 32]),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Manifest {
    pub channel: String,
    pub images: Vec<ImageEntry>,
    pub runners: Vec<RunnerEntry>,
    pub signature: SignatureState,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ManifestError {
    Empty,
    BadHeader(String),
    BadLine { line: usize, why: String },
    SignatureNotLast { line: usize },
    SignatureInvalid,
    UntrustedKey(String),
    TestKeyRefused,
    Duplicate { line: usize, what: String },
}

impl fmt::Display for ManifestError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ManifestError::Empty => write!(f, "manifest is empty"),
            ManifestError::BadHeader(h) => {
                write!(f, "not an aienos-run manifest (first line {h:?})")
            }
            ManifestError::BadLine { line, why } => write!(f, "manifest line {line}: {why}"),
            ManifestError::SignatureNotLast { line } => {
                write!(f, "manifest line {line}: signature must be the last line")
            }
            ManifestError::SignatureInvalid => write!(f, "manifest signature does not verify"),
            ManifestError::UntrustedKey(k) => {
                write!(f, "manifest signed by a key that is not pinned: {k}")
            }
            ManifestError::TestKeyRefused => write!(
                f,
                "manifest is signed with the public TEST key; pass --allow-test-key to accept it"
            ),
            ManifestError::Duplicate { line, what } => {
                write!(f, "manifest line {line}: duplicate {what}")
            }
        }
    }
}

impl std::error::Error for ManifestError {}

/// Trust policy for the optional signature line.
#[derive(Debug, Clone, Default)]
pub struct Trust {
    pub pinned: Vec<[u8; 32]>,
    pub allow_test_key: bool,
    pub require_signature: bool,
}

pub fn hex_encode(bytes: &[u8]) -> String {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push(H[(b >> 4) as usize] as char);
        s.push(H[(b & 15) as usize] as char);
    }
    s
}

pub fn hex_decode<const N: usize>(s: &str) -> Option<[u8; N]> {
    let b = s.as_bytes();
    if b.len() != N * 2 {
        return None;
    }
    let nib = |c: u8| match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    };
    let mut out = [0u8; N];
    for (i, o) in out.iter_mut().enumerate() {
        *o = (nib(b[2 * i])? << 4) | nib(b[2 * i + 1])?;
    }
    Some(out)
}

fn file_entry(fields: &[&str], line: usize) -> Result<FileEntry, ManifestError> {
    let bad = |why: &str| ManifestError::BadLine {
        line,
        why: why.to_string(),
    };
    let [name, size, sha, url] = fields else {
        return Err(bad("expected <file-name> <size> <sha256> <url>"));
    };
    if name.is_empty()
        || name.contains('/')
        || name.contains('\\')
        || name.starts_with('.')
        || name.contains("..")
    {
        return Err(bad(
            "file name must be a plain name (no path, no leading dot)",
        ));
    }
    let size: u64 = size.parse().map_err(|_| bad("size is not a number"))?;
    let sha256 = hex_decode::<32>(sha).ok_or_else(|| bad("sha256 is not 64 hex digits"))?;
    Ok(FileEntry {
        name: name.to_string(),
        size,
        sha256,
        url: url.to_string(),
    })
}

impl Manifest {
    /// Parses and checks a manifest, including its signature line against `trust`.
    pub fn parse(text: &str, trust: &Trust) -> Result<Manifest, ManifestError> {
        let mut lines = text.split_inclusive('\n').enumerate();
        let (_, first) = lines.next().ok_or(ManifestError::Empty)?;
        if first.trim_end() != "aienos-run-manifest 1" {
            return Err(ManifestError::BadHeader(first.trim_end().to_string()));
        }
        let mut m = Manifest {
            channel: String::new(),
            images: Vec::new(),
            runners: Vec::new(),
            signature: SignatureState::Unsigned,
        };
        let mut offset = first.len();
        let mut signed_len = None;
        let mut sig_fields: Option<(String, String)> = None;
        for (i, raw) in lines {
            let line = i + 1;
            let this_offset = offset;
            offset += raw.len();
            let l = raw.trim();
            if l.is_empty() || l.starts_with('#') {
                continue;
            }
            if sig_fields.is_some() {
                return Err(ManifestError::SignatureNotLast { line });
            }
            let f: Vec<&str> = l.split_ascii_whitespace().collect();
            match f[0] {
                "channel" if f.len() == 2 => m.channel = f[1].to_string(),
                "image" if f.len() == 7 => {
                    let arch = Arch::parse(f[1]).ok_or_else(|| ManifestError::BadLine {
                        line,
                        why: format!("unknown arch {:?}", f[1]),
                    })?;
                    let kind = ImageKind::parse(f[2]).ok_or_else(|| ManifestError::BadLine {
                        line,
                        why: format!("unknown image kind {:?}", f[2]),
                    })?;
                    if m.images.iter().any(|e| e.arch == arch && e.kind == kind) {
                        return Err(ManifestError::Duplicate {
                            line,
                            what: format!("image {} {}", f[1], f[2]),
                        });
                    }
                    m.images.push(ImageEntry {
                        arch,
                        kind,
                        file: file_entry(&f[3..], line)?,
                    });
                }
                "runner" if f.len() == 6 => {
                    if m.runners.iter().any(|r| r.platform == f[1]) {
                        return Err(ManifestError::Duplicate {
                            line,
                            what: format!("runner {}", f[1]),
                        });
                    }
                    m.runners.push(RunnerEntry {
                        platform: f[1].to_string(),
                        file: file_entry(&f[2..], line)?,
                    });
                }
                "signature" if f.len() == 4 && f[1] == "ed25519" => {
                    signed_len = Some(this_offset);
                    sig_fields = Some((f[2].to_string(), f[3].to_string()));
                }
                _ => {
                    return Err(ManifestError::BadLine {
                        line,
                        why: format!("unknown or malformed record {:?}", f[0]),
                    })
                }
            }
        }
        match (sig_fields, signed_len) {
            (Some((pk_hex, sig_hex)), Some(len)) => {
                m.signature = verify_signature(&text.as_bytes()[..len], &pk_hex, &sig_hex, trust)?;
            }
            _ if trust.require_signature => {
                return Err(ManifestError::BadLine {
                    line: 0,
                    why: "no signature line and a signature is required".to_string(),
                })
            }
            _ => {}
        }
        Ok(m)
    }

    pub fn image(&self, arch: Arch, kinds: &[ImageKind]) -> Option<&ImageEntry> {
        kinds
            .iter()
            .find_map(|k| self.images.iter().find(|e| e.arch == arch && e.kind == *k))
    }

    pub fn runner(&self, platform: &str) -> Option<&RunnerEntry> {
        self.runners.iter().find(|r| r.platform == platform)
    }
}

fn verify_signature(
    body: &[u8],
    pk_hex: &str,
    sig_hex: &str,
    trust: &Trust,
) -> Result<SignatureState, ManifestError> {
    use ed25519_dalek::{Signature, Verifier, VerifyingKey};
    let pk = hex_decode::<32>(pk_hex).ok_or(ManifestError::SignatureInvalid)?;
    let sig = hex_decode::<64>(sig_hex).ok_or(ManifestError::SignatureInvalid)?;
    let key = VerifyingKey::from_bytes(&pk).map_err(|_| ManifestError::SignatureInvalid)?;
    key.verify(body, &Signature::from_bytes(&sig))
        .map_err(|_| ManifestError::SignatureInvalid)?;
    let is_test = hex_decode::<32>(TEST_PUBLIC_KEY_HEX) == Some(pk);
    if is_test {
        if trust.allow_test_key {
            return Ok(SignatureState::TestKey);
        }
        return Err(ManifestError::TestKeyRefused);
    }
    if trust.pinned.contains(&pk) {
        return Ok(SignatureState::Pinned(pk));
    }
    Err(ManifestError::UntrustedKey(pk_hex.to_string()))
}

/// `C:\x` or `C:/x` or `\\server\share`.
fn is_windows_absolute(s: &str) -> bool {
    let b = s.as_bytes();
    (b.len() > 2 && b[0].is_ascii_alphabetic() && b[1] == b':' && (b[2] == b'\\' || b[2] == b'/'))
        || s.starts_with("\\\\")
}

/// Resolves a manifest entry URL against the manifest's own location.
pub fn resolve_url(base: &str, url: &str) -> String {
    if url.contains("://") || url.starts_with('/') || is_windows_absolute(url) {
        return url.to_string();
    }
    // A local Windows manifest path (`C:\dl\manifest.txt`) separates with backslashes.
    let sep = |c: char| c == '/' || (!base.contains("://") && c == '\\');
    match base.rfind(sep) {
        Some(i) => format!("{}{}", &base[..=i], url),
        None => url.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};

    const SHA: &str = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";

    fn body() -> String {
        format!(
            "aienos-run-manifest 1\n# comment\nchannel test\n\
             image aarch64 uefi-app BOOTAA64.EFI 4 {SHA} images/BOOTAA64.EFI\n\
             runner linux-aarch64 aienos-run 4 {SHA} https://example.invalid/aienos-run\n"
        )
    }

    // RFC 8032 section 7.1 TEST 1 secret key.
    const TEST_SECRET_HEX: &str =
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";

    fn signed(key: &SigningKey, body: &str) -> String {
        let sig = key.sign(body.as_bytes());
        format!(
            "{body}signature ed25519 {} {}\n",
            hex_encode(key.verifying_key().as_bytes()),
            hex_encode(&sig.to_bytes())
        )
    }

    #[test]
    fn parses_unsigned() {
        let m = Manifest::parse(&body(), &Trust::default()).unwrap();
        assert_eq!(m.channel, "test");
        assert_eq!(m.signature, SignatureState::Unsigned);
        let img = m.image(Arch::Aarch64, &[ImageKind::UefiApp]).unwrap();
        assert_eq!(img.file.name, "BOOTAA64.EFI");
        assert_eq!(img.file.size, 4);
        assert_eq!(hex_encode(&img.file.sha256), SHA);
        assert!(m.image(Arch::X86_64, &[ImageKind::UefiApp]).is_none());
        assert_eq!(m.runner("linux-aarch64").unwrap().file.name, "aienos-run");
    }

    #[test]
    fn refuses_bad_input() {
        let t = Trust::default();
        assert_eq!(Manifest::parse("", &t), Err(ManifestError::Empty));
        assert!(matches!(
            Manifest::parse("hello\n", &t),
            Err(ManifestError::BadHeader(_))
        ));
        let bad_sha = body().replace(SHA, "zz");
        assert!(matches!(
            Manifest::parse(&bad_sha, &t),
            Err(ManifestError::BadLine { line: 4, .. })
        ));
        let path_name = body().replace("BOOTAA64.EFI 4", "../x 4");
        assert!(matches!(
            Manifest::parse(&path_name, &t),
            Err(ManifestError::BadLine { .. })
        ));
        let dup = format!("{}image aarch64 uefi-app B.EFI 4 {SHA} B.EFI\n", body());
        assert!(matches!(
            Manifest::parse(&dup, &t),
            Err(ManifestError::Duplicate { .. })
        ));
        let unknown = format!("{}frobnicate 1\n", body());
        assert!(matches!(
            Manifest::parse(&unknown, &t),
            Err(ManifestError::BadLine { .. })
        ));
        let require = Trust {
            require_signature: true,
            ..Trust::default()
        };
        assert!(Manifest::parse(&body(), &require).is_err());
    }

    #[test]
    fn test_key_needs_opt_in() {
        let key = SigningKey::from_bytes(&hex_decode::<32>(TEST_SECRET_HEX).unwrap());
        assert_eq!(
            hex_encode(key.verifying_key().as_bytes()),
            TEST_PUBLIC_KEY_HEX
        );
        let text = signed(&key, &body());
        assert_eq!(
            Manifest::parse(&text, &Trust::default()),
            Err(ManifestError::TestKeyRefused)
        );
        let allow = Trust {
            allow_test_key: true,
            ..Trust::default()
        };
        assert_eq!(
            Manifest::parse(&text, &allow).unwrap().signature,
            SignatureState::TestKey
        );
        // One flipped byte in the signed body must fail.
        let tampered = text.replacen("channel test", "channel tesu", 1);
        assert_eq!(
            Manifest::parse(&tampered, &allow),
            Err(ManifestError::SignatureInvalid)
        );
        // Anything after the signature line is refused.
        let trailing = format!("{text}channel x\n");
        assert!(matches!(
            Manifest::parse(&trailing, &allow),
            Err(ManifestError::SignatureNotLast { .. })
        ));
    }

    #[test]
    fn pinned_key_accepted_unknown_refused() {
        let key = SigningKey::from_bytes(&[7u8; 32]);
        let text = signed(&key, &body());
        assert!(matches!(
            Manifest::parse(&text, &Trust::default()),
            Err(ManifestError::UntrustedKey(_))
        ));
        let pinned = Trust {
            pinned: vec![*key.verifying_key().as_bytes()],
            ..Trust::default()
        };
        assert!(matches!(
            Manifest::parse(&text, &pinned).unwrap().signature,
            SignatureState::Pinned(_)
        ));
    }

    #[test]
    fn resolves_relative_urls() {
        assert_eq!(
            resolve_url("https://www.aienos.com/try/manifest.txt", "img/a.efi"),
            "https://www.aienos.com/try/img/a.efi"
        );
        assert_eq!(
            resolve_url("file:///tmp/m/manifest.txt", "file:///x"),
            "file:///x"
        );
        assert_eq!(resolve_url("/srv/m.txt", "a.efi"), "/srv/a.efi");
        assert_eq!(
            resolve_url(r"C:\dl\manifest.txt", "aienos-qemu-win64.zip"),
            r"C:\dl\aienos-qemu-win64.zip"
        );
        assert_eq!(resolve_url(r"C:\dl\m.txt", r"D:\x.qcow2"), r"D:\x.qcow2");
        assert_eq!(
            resolve_url("https://h/a\\b/m.txt", "x"),
            "https://h/a\\b/x",
            "backslashes in a URL are not separators"
        );
    }
}
