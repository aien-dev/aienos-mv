//! The one seam every host plugs into: a [`Backend`] boots a [`VmSpec`] and moves bytes
//! between the guest's serial port and the shared [`Console`].
//!
//! Backends:
//! - `kvm`  (Linux, aarch64 host + aarch64 image): our own VMM on /dev/kvm (`kvm/`).
//! - `qemu` (any host with QEMU installed): QEMU with the best accelerator the host has
//!   (KVM on Linux, HVF on macOS, WHPX on Windows, TCG otherwise). The day-one fallback.
//! - `whpx` (Windows, ledger L261) and `vz` (macOS Virtualization.framework) implement the
//!   same trait in their own modules.

use crate::console::Console;
use crate::manifest::{Arch, ImageKind};
use crate::net::Forward;
use std::fmt;
use std::path::PathBuf;

pub mod qemu;

#[cfg(all(target_os = "linux", target_arch = "aarch64"))]
pub mod kvm;

#[cfg(target_os = "macos")]
pub mod objc;
#[cfg(target_os = "macos")]
pub mod vz;

/// Everything a backend needs to boot AienOS. Backend-neutral: no backend reads a
/// field it does not understand as a hint to do something else.
#[derive(Debug, Clone)]
pub struct VmSpec {
    pub arch: Arch,
    pub image: PathBuf,
    pub image_kind: ImageKind,
    /// Persistent Store disk (raw), attached read-write.
    pub store_disk: Option<PathBuf>,
    /// Writable copy of the UEFI variable store (firmware backends only).
    pub firmware_vars: Option<PathBuf>,
    pub mem_mib: u32,
    pub cpus: u32,
    pub forwards: Vec<Forward>,
    /// Scratch directory the backend may use for this run (deleted by the caller).
    pub scratch: PathBuf,
}

/// How a VM run ended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VmExit {
    /// The guest powered itself off (PSCI SYSTEM_OFF / ACPI off).
    PowerOff,
    /// The guest asked for a reset (aienos-run does not reboot it: like `-no-reboot`).
    Reset,
    /// The user (or a ready marker in measurement mode) asked for a stop.
    Stopped,
    /// The VM died: the text says why (guest crash, VMM error, emulator exit status).
    Failed(String),
}

impl VmExit {
    /// Process exit code for this outcome (documented in docs/AIENOS_RUN_LINUX.md).
    pub fn exit_code(&self) -> i32 {
        match self {
            VmExit::PowerOff | VmExit::Reset | VmExit::Stopped => 0,
            VmExit::Failed(_) => 1,
        }
    }
}

#[derive(Debug)]
pub enum BackendError {
    /// This backend cannot run on this host or for this image (the text says why).
    Unavailable(String),
    /// Setting up or running the VM failed.
    Vmm(String),
}

impl fmt::Display for BackendError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BackendError::Unavailable(s) => write!(f, "backend unavailable: {s}"),
            BackendError::Vmm(s) => write!(f, "vm error: {s}"),
        }
    }
}

impl std::error::Error for BackendError {}

pub trait Backend {
    /// Short name used by `--backend` (`kvm`, `qemu`, `whpx`, `vz`).
    fn name(&self) -> &'static str;
    /// Ok when this backend can boot `spec` on this host; otherwise why not.
    fn probe(&self, spec: &VmSpec) -> Result<(), BackendError>;
    /// Boots `spec` and runs until the guest powers off or resets, the VM fails, or
    /// `console.stop_requested()` turns true (then stops the VM cleanly and returns
    /// [`VmExit::Stopped`]). Guest serial output goes to `console.guest_output`, user
    /// input comes from `console.pop_input`.
    fn run(&mut self, spec: &VmSpec, console: &Console) -> Result<VmExit, BackendError>;
}

/// Backends this build knows, in `auto` preference order.
#[allow(clippy::vec_init_then_push)] // the pushes are per-host cfg
pub fn all() -> Vec<Box<dyn Backend>> {
    #[allow(unused_mut)]
    let mut v: Vec<Box<dyn Backend>> = Vec::new();
    #[cfg(all(target_os = "linux", target_arch = "aarch64"))]
    v.push(Box::new(kvm::KvmBackend::default()));
    #[cfg(target_os = "macos")]
    v.push(Box::new(vz::VzBackend));
    v.push(Box::new(qemu::QemuBackend::default()));
    v
}

/// Picks the backend: by name, or the first one whose probe passes.
pub fn select(name: &str, spec: &VmSpec) -> Result<Box<dyn Backend>, BackendError> {
    let mut why = Vec::new();
    for b in all() {
        if name != "auto" && b.name() != name {
            continue;
        }
        match b.probe(spec) {
            Ok(()) => return Ok(b),
            Err(e) => why.push(format!("{}: {e}", b.name())),
        }
    }
    if why.is_empty() {
        return Err(BackendError::Unavailable(format!(
            "no backend named {name:?} in this build"
        )));
    }
    Err(BackendError::Unavailable(why.join("; ")))
}
