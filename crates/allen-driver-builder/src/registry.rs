//! The contracts Allen already has, and the decision for each device.
//!
//! A device contract (`.devc`, ADR 0021, compiled by `aienos-devc`) names the
//! functions it binds with `match pci VVVV DDDD [subsys N]` and carries its
//! proof on machines as `proven <platform> <machine-id> <date> <gates> <commit>`
//! lines; a `proven` line whose commit is `pending` is a claim, not a proof
//! (same counting as aienos-devc). Only those two statements are read here: the
//! contract compiler stays the one parser of the language.

use crate::inventory::Device;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Proven {
    pub platform: String,
    pub machine: String,
    pub commit: String,
}

#[derive(Debug, Clone)]
pub struct ContractInfo {
    pub path: PathBuf,
    pub device: String,
    /// (vendor, device, subsystem id when the match names one)
    pub matches: Vec<(u16, u16, Option<u16>)>,
    pub proven: Vec<Proven>,
    pub digest: String,
}

fn num(s: &str) -> Option<u64> {
    match s.strip_prefix("0x") {
        Some(h) => u64::from_str_radix(h, 16).ok(),
        None => s.parse().ok(),
    }
}

/// The identity and proof statements of one contract.
pub fn read_contract(path: &Path, src: &str) -> ContractInfo {
    let mut c = ContractInfo {
        path: path.to_path_buf(),
        device: String::new(),
        matches: Vec::new(),
        proven: Vec::new(),
        digest: crate::sha256_hex(src.as_bytes()),
    };
    for line in src.lines() {
        let line = line.split('#').next().unwrap_or("");
        let w: Vec<&str> = line.split_whitespace().collect();
        match w.as_slice() {
            ["device", name, ..] => c.device = name.to_string(),
            ["match", "pci", v, d, rest @ ..] => {
                let (Some(v), Some(d)) = (num(v), num(d)) else {
                    continue;
                };
                let sub = match rest {
                    ["subsys", s, ..] => num(s).map(|s| s as u16),
                    _ => None,
                };
                c.matches.push((v as u16, d as u16, sub));
            }
            ["proven", platform, machine, _date, _gates, commit, ..] => c.proven.push(Proven {
                platform: platform.to_string(),
                machine: machine.to_string(),
                commit: commit.to_string(),
            }),
            _ => {}
        }
    }
    c
}

/// Every `*.devc` under `dir` (recursively), sorted by path.
pub fn scan(dir: &Path) -> std::io::Result<Vec<ContractInfo>> {
    let mut out = Vec::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d)? {
            let p = e?.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.extension().is_some_and(|x| x == "devc") {
                let src = std::fs::read_to_string(&p)?;
                out.push(read_contract(&p, &src));
            }
        }
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(out)
}

impl ContractInfo {
    pub fn binds(&self, d: &Device) -> bool {
        self.matches.iter().any(|&(v, id, sub)| {
            v == d.vendor && id == d.device && sub.is_none_or(|s| s == d.subsys.1)
        })
    }

    /// Proven on this platform and machine with a real commit.
    pub fn proven_on(&self, platform: &str, machine: &str) -> bool {
        self.proven
            .iter()
            .any(|p| p.platform == platform && p.machine == machine && p.commit != "pending")
    }
}

/// What Allen does with one device (brief L268 (a) (b) (c)).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Decision {
    /// (a) a contract exists and is proven on this machine: load it.
    Load { contract: PathBuf },
    /// (b) a contract exists but has no proof here: enable only attended, with a receipt.
    Attended { contract: PathBuf },
    /// (c) no contract: Allen builds one.
    Build,
    /// Bridges and host functions: nothing to drive.
    Skip,
}

pub fn decide(d: &Device, contracts: &[ContractInfo], platform: &str, machine: &str) -> Decision {
    if d.class >> 16 == 0x06 {
        return Decision::Skip;
    }
    let mine: Vec<&ContractInfo> = contracts.iter().filter(|c| c.binds(d)).collect();
    if let Some(c) = mine.iter().find(|c| c.proven_on(platform, machine)) {
        return Decision::Load {
            contract: c.path.clone(),
        };
    }
    match mine.first() {
        Some(c) => Decision::Attended {
            contract: c.path.clone(),
        },
        None => Decision::Build,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::inventory::parse_line;

    fn dev(id: &str, class: &str) -> Device {
        parse_line(&format!(
            "inventory: pci 0000:00:02.0 {id} class={class} subsys=1af4:0001 rev=01 bars=none driver=none contract=none proven=n/a"
        ))
        .unwrap()
        .unwrap()
    }

    const NET: &str = "devc 1\ndevice virtio-net\nmatch pci 0x1af4 0x1041\nmatch pci 0x1af4 0x1000 subsys 1 # legacy\n\
                       proven qemu-virt-aarch64 spark-qemu 2026-10-20 AIENOS_CK_NET pending\n";

    #[test]
    fn the_three_cases() {
        let pending = read_contract(Path::new("net.devc"), NET);
        assert_eq!(
            pending.matches,
            vec![(0x1af4, 0x1041, None), (0x1af4, 0x1000, Some(1))]
        );
        let net = dev("1af4:1041", "020000");
        // a pending proof is no proof: attended
        assert_eq!(
            decide(
                &net,
                std::slice::from_ref(&pending),
                "qemu-virt-aarch64",
                "spark-qemu"
            ),
            Decision::Attended {
                contract: "net.devc".into()
            }
        );
        let proven = read_contract(Path::new("net.devc"), &NET.replace("pending", "18e2765"));
        assert_eq!(
            decide(
                &net,
                std::slice::from_ref(&proven),
                "qemu-virt-aarch64",
                "spark-qemu"
            ),
            Decision::Load {
                contract: "net.devc".into()
            }
        );
        // proven on another machine only: attended here
        assert!(matches!(
            decide(
                &net,
                std::slice::from_ref(&proven),
                "qemu-virt-aarch64",
                "other"
            ),
            Decision::Attended { .. }
        ));
        assert_eq!(
            decide(
                &dev("1af4:1044", "00ff00"),
                &[proven],
                "qemu-virt-aarch64",
                "spark-qemu"
            ),
            Decision::Build
        );
    }

    #[test]
    fn subsystem_matches_and_bridges() {
        let c = read_contract(Path::new("net.devc"), NET);
        assert!(c.binds(&dev("1af4:1000", "020000")));
        let mut other = dev("1af4:1000", "020000");
        other.subsys.1 = 2;
        assert!(!c.binds(&other));
        assert_eq!(
            decide(&dev("1b36:0008", "060000"), &[c], "p", "m"),
            Decision::Skip
        );
    }
}
