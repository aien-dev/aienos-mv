//! The Backend trait: one per hypervisor. The core (CLI, image cache, Store
//! disk, console pump, port forwards) is the same on every host; a backend only
//! turns a `VmSpec` into a running VM and a console byte stream.

use std::io::{Read, Write};
use std::path::PathBuf;

#[cfg(target_os = "macos")]
pub mod objc;
pub mod qemu;
#[cfg(target_os = "macos")]
pub mod vz;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Proto {
    Udp,
    Tcp,
}

/// Host port -> guest port. The signed transport's UDP port is forwarded by
/// default (`DEFAULT_FORWARDS`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PortForward {
    pub proto: Proto,
    pub host: u16,
    pub guest: u16,
}

/// CK_NET_LOCAL_PORT of the signed transport (L260/L261 contract note).
pub const SIGNED_TRANSPORT_UDP: u16 = 47030;

pub fn default_forwards() -> Vec<PortForward> {
    vec![PortForward { proto: Proto::Udp, host: SIGNED_TRANSPORT_UDP, guest: SIGNED_TRANSPORT_UDP }]
}

impl PortForward {
    /// "udp:47030:47030" or "tcp:8080:80".
    pub fn parse(s: &str) -> Result<Self, String> {
        let p: Vec<&str> = s.split(':').collect();
        if p.len() != 3 {
            return Err(format!("port forward {s:?}: want proto:host:guest, e.g. udp:47030:47030"));
        }
        let proto = match p[0] {
            "udp" => Proto::Udp,
            "tcp" => Proto::Tcp,
            o => return Err(format!("port forward {s:?}: protocol {o:?} is not udp or tcp")),
        };
        let port = |v: &str| -> Result<u16, String> {
            match v.parse::<u16>() {
                Ok(0) | Err(_) => Err(format!("port forward {s:?}: {v:?} is not a port 1..65535")),
                Ok(n) => Ok(n),
            }
        };
        Ok(PortForward { proto, host: port(p[1])?, guest: port(p[2])? })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NetMode {
    None,
    /// User-mode NAT, guest 10.0.2.15, gateway 10.0.2.2 = host loopback.
    /// QEMU: slirp with hostfwd rules. VZ: the runner's own UDP network
    /// (crate::usernet) on a VZFileHandleNetworkDeviceAttachment.
    Nat,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StoreBus {
    /// The AienOS Store driver is NVMe (native/kernel dev/nvme): default.
    Nvme,
    VirtioBlk,
}

#[derive(Clone, Debug)]
pub struct VmSpec {
    /// Raw GPT disk with an ESP holding \EFI\BOOT\BOOTAA64.EFI.
    pub boot_disk: PathBuf,
    pub boot_read_only: bool,
    /// Per-user persistent Store disk (raw), blank on first run.
    pub store_disk: Option<PathBuf>,
    pub store_bus: StoreBus,
    /// Per-user EFI variable store (VZ format for vz, edk2 vars.fd for qemu).
    pub efi_vars: PathBuf,
    pub cpus: u32,
    pub mem_mib: u64,
    pub net: NetMode,
    pub mac: Option<String>,
    pub forwards: Vec<PortForward>,
    pub framebuffer: bool,
}

/// The guest console as two byte streams.
pub struct Console {
    pub from_guest: Box<dyn Read + Send>,
    pub to_guest: Box<dyn Write + Send>,
}

pub trait RunningVm: Send {
    /// None until the hypervisor confirms the start.
    fn started(&mut self) -> Option<Result<(), String>>;
    /// None while running; Some(Ok) after a clean guest stop.
    fn poll_exit(&mut self) -> Option<Result<(), String>>;
    /// Ask the guest to power off (ACPI power button / VZ requestStop).
    fn request_stop(&mut self) -> bool;
    /// Pull the plug.
    fn force_stop(&mut self);
}

pub trait Backend {
    fn name(&self) -> &'static str;
    fn available(&self) -> Result<(), String>;
    fn boot(&self, spec: &VmSpec) -> Result<(Box<dyn RunningVm>, Console), String>;
}

/// Backends this host can run, fastest first (by measurement, see docs).
pub fn for_host(name: Option<&str>) -> Result<Box<dyn Backend>, String> {
    match name {
        #[cfg(target_os = "macos")]
        Some("vz") | None => Ok(Box::new(vz::Vz)),
        Some("qemu") => Ok(Box::new(qemu::Qemu::for_host())),
        #[cfg(not(target_os = "macos"))]
        None => Ok(Box::new(qemu::Qemu::for_host())),
        Some(o) => Err(format!("unknown backend {o:?} (this build has: {})", names().join(", "))),
    }
}

pub fn names() -> Vec<&'static str> {
    vec![
        #[cfg(target_os = "macos")]
        "vz",
        "qemu",
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn forward_parse_accepts_udp_and_tcp() {
        assert_eq!(
            PortForward::parse("udp:47030:47030").unwrap(),
            PortForward { proto: Proto::Udp, host: 47030, guest: 47030 }
        );
        assert_eq!(PortForward::parse("tcp:8080:80").unwrap(), PortForward { proto: Proto::Tcp, host: 8080, guest: 80 });
    }

    #[test]
    fn forward_parse_refuses_bad_input() {
        for bad in ["", "udp:1", "icmp:1:1", "udp:0:1", "udp:1:70000", "udp:a:1", "udp:1:2:3"] {
            assert!(PortForward::parse(bad).is_err(), "{bad} accepted");
        }
    }

    #[test]
    fn default_forward_is_the_signed_transport_port() {
        assert_eq!(default_forwards(), vec![PortForward { proto: Proto::Udp, host: 47030, guest: 47030 }]);
    }
}
