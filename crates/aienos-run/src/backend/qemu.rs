//! QEMU backend: the day-one path on every host that has QEMU installed. It uses the
//! host's hardware accelerator when the guest architecture matches (KVM on Linux, HVF on
//! macOS, WHPX on Windows) and plain TCG emulation otherwise (for example x86_64 images
//! on the aarch64 Spark: emulated, never accelerated).
//!
//! The kernel-as-UEFI-app image is booted the way the repo's QEMU gates boot it: UEFI
//! firmware (edk2 AAVMF/OVMF) in pflash, the app on a virtual FAT ESP.

use super::{Backend, BackendError, VmExit, VmSpec};
use crate::console::Console;
use crate::manifest::{Arch, ImageKind};
use crate::net::Forward;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Accel {
    Kvm,
    Hvf,
    Whpx,
    Tcg,
}

impl Accel {
    pub fn as_str(self) -> &'static str {
        match self {
            Accel::Kvm => "kvm",
            Accel::Hvf => "hvf",
            Accel::Whpx => "whpx",
            Accel::Tcg => "tcg",
        }
    }
}

#[derive(Default)]
pub struct QemuBackend {
    /// Force an accelerator (tests and measurements); None picks the best one.
    pub accel: Option<Accel>,
}

pub fn qemu_binary(arch: Arch) -> &'static str {
    match arch {
        Arch::Aarch64 => "qemu-system-aarch64",
        Arch::X86_64 => "qemu-system-x86_64",
    }
}

/// Known firmware locations: (code, vars template).
fn firmware_candidates(arch: Arch) -> &'static [(&'static str, &'static str)] {
    match arch {
        Arch::Aarch64 => &[
            (
                "/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd",
                "/usr/share/AAVMF/AAVMF_VARS.fd",
            ),
            ("/usr/share/AAVMF/AAVMF_CODE.fd", "/usr/share/AAVMF/AAVMF_VARS.fd"),
            (
                "/usr/share/edk2/aarch64/QEMU_EFI-pflash.raw",
                "/usr/share/edk2/aarch64/vars-template-pflash.raw",
            ),
            (
                "/opt/homebrew/share/qemu/edk2-aarch64-code.fd",
                "/opt/homebrew/share/qemu/edk2-arm-vars.fd",
            ),
            (
                "/usr/local/share/qemu/edk2-aarch64-code.fd",
                "/usr/local/share/qemu/edk2-arm-vars.fd",
            ),
        ],
        Arch::X86_64 => &[
            ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
            ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),
            (
                "/usr/share/edk2/ovmf/OVMF_CODE.fd",
                "/usr/share/edk2/ovmf/OVMF_VARS.fd",
            ),
            (
                "/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
                "/opt/homebrew/share/qemu/edk2-i386-vars.fd",
            ),
        ],
    }
}

pub fn find_firmware(arch: Arch) -> Option<(PathBuf, PathBuf)> {
    firmware_candidates(arch)
        .iter()
        .map(|(c, v)| (PathBuf::from(c), PathBuf::from(v)))
        .find(|(c, v)| c.exists() && v.exists())
}

fn in_path(bin: &str) -> bool {
    let Some(path) = std::env::var_os("PATH") else {
        return false;
    };
    std::env::split_paths(&path).any(|d| {
        d.join(bin).is_file() || (cfg!(windows) && d.join(format!("{bin}.exe")).is_file())
    })
}

fn kvm_usable() -> bool {
    std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open("/dev/kvm")
        .is_ok()
}

/// The accelerator QEMU should use for a guest of `arch` on this host.
pub fn best_accel(arch: Arch) -> Accel {
    if arch != Arch::host() {
        return Accel::Tcg;
    }
    if cfg!(target_os = "linux") && kvm_usable() {
        Accel::Kvm
    } else if cfg!(target_os = "macos") {
        Accel::Hvf
    } else if cfg!(windows) {
        Accel::Whpx
    } else {
        Accel::Tcg
    }
}

fn hostfwd(f: &Forward) -> String {
    format!(
        "hostfwd={}:{}:{}-:{}",
        f.proto.as_str(),
        f.host_addr,
        f.host_port,
        f.guest_port
    )
}

/// The full QEMU command line for `spec` (separate so tests can check it).
pub fn command_line(
    spec: &VmSpec,
    accel: Accel,
    firmware_code: &Path,
    vars: &Path,
    esp_dir: Option<&Path>,
) -> Vec<String> {
    let mut a: Vec<String> = Vec::new();
    let mut push = |s: &str| a.push(s.to_string());
    match spec.arch {
        Arch::Aarch64 => {
            push("-M");
            push(if accel == Accel::Tcg {
                "virt,virtualization=on,gic-version=3"
            } else {
                "virt,gic-version=3"
            });
        }
        Arch::X86_64 => {
            push("-M");
            push("q35");
        }
    }
    push("-accel");
    // Single-threaded TCG: repo issue #61 (MTTCG hung 1 in 40 boots of this kernel).
    push(if accel == Accel::Tcg { "tcg,thread=single" } else { accel.as_str() });
    push("-cpu");
    push(match (accel, spec.arch) {
        (Accel::Tcg, Arch::Aarch64) => "max",
        (Accel::Tcg, Arch::X86_64) => "max",
        _ => "host",
    });
    push("-smp");
    push(&spec.cpus.to_string());
    push("-m");
    push(&spec.mem_mib.to_string());
    push("-nodefaults");
    push("-display");
    push("none");
    push("-no-reboot");
    push("-drive");
    push(&format!(
        "if=pflash,format=raw,readonly=on,file={}",
        firmware_code.display()
    ));
    push("-drive");
    push(&format!("if=pflash,format=raw,file={}", vars.display()));
    match spec.image_kind {
        ImageKind::UefiApp => {
            let esp = esp_dir.expect("uefi-app needs an ESP directory");
            push("-drive");
            push(&format!("if=none,id=esp,format=raw,file=fat:rw:{}", esp.display()));
            push("-device");
            push("virtio-blk-pci,drive=esp");
        }
        ImageKind::Iso => {
            push("-drive");
            push(&format!(
                "if=none,id=os,media=cdrom,readonly=on,file={}",
                spec.image.display()
            ));
            push("-device");
            push("virtio-scsi-pci,id=scsi0");
            push("-device");
            push("scsi-cd,drive=os");
        }
        ImageKind::DiskRaw | ImageKind::DiskQcow2 | ImageKind::DiskVhdx => {
            let fmt = match spec.image_kind {
                ImageKind::DiskQcow2 => "qcow2",
                ImageKind::DiskVhdx => "vhdx",
                _ => "raw",
            };
            // The cached image stays pristine: writes go to a throwaway overlay.
            push("-drive");
            push(&format!(
                "if=none,id=os,format={fmt},snapshot=on,file={}",
                spec.image.display()
            ));
            push("-device");
            push("virtio-blk-pci,drive=os");
        }
    }
    if let Some(store) = &spec.store_disk {
        push("-drive");
        push(&format!("if=none,id=store,format=raw,file={}", store.display()));
        push("-device");
        push("nvme,serial=aienos-store,drive=store");
    }
    let mut netdev = "user,id=n0".to_string();
    for f in &spec.forwards {
        netdev.push(',');
        netdev.push_str(&hostfwd(f));
    }
    push("-netdev");
    push(&netdev);
    push("-device");
    push("virtio-net-pci,netdev=n0");
    push("-chardev");
    push("stdio,id=s0,signal=off");
    push("-serial");
    push("chardev:s0");
    push("-monitor");
    push("none");
    a
}

impl Backend for QemuBackend {
    fn name(&self) -> &'static str {
        "qemu"
    }

    fn probe(&self, spec: &VmSpec) -> Result<(), BackendError> {
        let bin = qemu_binary(spec.arch);
        if !in_path(bin) {
            return Err(BackendError::Unavailable(format!("{bin} is not installed")));
        }
        if find_firmware(spec.arch).is_none() {
            return Err(BackendError::Unavailable(format!(
                "no UEFI firmware for {} found (install qemu-efi-aarch64 / ovmf)",
                spec.arch.as_str()
            )));
        }
        Ok(())
    }

    fn run(&mut self, spec: &VmSpec, console: &Console) -> Result<VmExit, BackendError> {
        let (code, vars_template) = find_firmware(spec.arch)
            .ok_or_else(|| BackendError::Unavailable("no UEFI firmware".into()))?;
        let vars = match &spec.firmware_vars {
            Some(v) => v.clone(),
            None => spec.scratch.join("vars.fd"),
        };
        let vmm = |e: std::io::Error| BackendError::Vmm(e.to_string());
        if !vars.exists() {
            if let Some(d) = vars.parent() {
                std::fs::create_dir_all(d).map_err(vmm)?;
            }
            std::fs::copy(&vars_template, &vars).map_err(vmm)?;
        }
        let esp = if spec.image_kind == ImageKind::UefiApp {
            let boot = spec.scratch.join("esp/EFI/BOOT");
            std::fs::create_dir_all(&boot).map_err(vmm)?;
            std::fs::create_dir_all(spec.scratch.join("esp/EFI/AIENOS")).map_err(vmm)?;
            let name = match spec.arch {
                Arch::Aarch64 => "BOOTAA64.EFI",
                Arch::X86_64 => "BOOTX64.EFI",
            };
            std::fs::copy(&spec.image, boot.join(name)).map_err(vmm)?;
            Some(spec.scratch.join("esp"))
        } else {
            None
        };
        let accel = self.accel.unwrap_or_else(|| best_accel(spec.arch));
        let args = command_line(spec, accel, &code, &vars, esp.as_deref());
        let mut child = Command::new(qemu_binary(spec.arch))
            .args(&args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| BackendError::Vmm(format!("cannot start QEMU: {e}")))?;
        let mut stdin = child.stdin.take().expect("piped stdin");
        let mut stdout = child.stdout.take().expect("piped stdout");
        let mut stderr = child.stderr.take().expect("piped stderr");
        let mut err_text = String::new();
        let mut stopped = false;
        let status = std::thread::scope(|s| {
            s.spawn(|| {
                let mut buf = [0u8; 4096];
                while let Ok(n) = stdout.read(&mut buf) {
                    if n == 0 {
                        break;
                    }
                    console.guest_output(&buf[..n]);
                }
            });
            s.spawn(|| {
                let _ = stderr.read_to_string(&mut err_text);
            });
            loop {
                if let Some(st) = child.try_wait().map_err(vmm)? {
                    return Ok(st);
                }
                if console.stop_requested() && !stopped {
                    stopped = true;
                    #[cfg(unix)]
                    unsafe {
                        libc::kill(child.id() as libc::pid_t, libc::SIGTERM);
                    }
                    #[cfg(not(unix))]
                    let _ = child.kill();
                }
                let mut wrote = false;
                while let Some(b) = console.pop_input() {
                    let _ = stdin.write_all(&[b]);
                    wrote = true;
                }
                if wrote {
                    let _ = stdin.flush();
                }
                std::thread::sleep(Duration::from_millis(2));
            }
        })?;
        if stopped {
            return Ok(VmExit::Stopped);
        }
        if status.success() {
            return Ok(VmExit::PowerOff);
        }
        Ok(VmExit::Failed(format!("QEMU exit {status}: {}", err_text.trim())))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::default_forwards;

    fn spec(kind: ImageKind, arch: Arch) -> VmSpec {
        VmSpec {
            arch,
            image: PathBuf::from("/img/a"),
            image_kind: kind,
            store_disk: Some(PathBuf::from("/d/store.img")),
            firmware_vars: None,
            mem_mib: 1024,
            cpus: 2,
            forwards: default_forwards(),
            scratch: PathBuf::from("/s"),
        }
    }

    #[test]
    fn kvm_command_line() {
        let s = spec(ImageKind::UefiApp, Arch::Aarch64);
        let a = command_line(&s, Accel::Kvm, Path::new("/fw/c"), Path::new("/fw/v"), Some(Path::new("/s/esp")));
        let j = a.join(" ");
        assert!(j.contains("-M virt,gic-version=3 -accel kvm -cpu host"), "{j}");
        assert!(j.contains("file=fat:rw:/s/esp"));
        assert!(j.contains("hostfwd=udp:127.0.0.1:47040-:47040"));
        assert!(j.contains("nvme,serial=aienos-store,drive=store"));
        assert!(j.contains("-no-reboot"));
        assert!(j.contains("stdio,id=s0,signal=off"));
    }

    #[test]
    fn tcg_x86_disk_command_line() {
        let s = spec(ImageKind::DiskQcow2, Arch::X86_64);
        let a = command_line(&s, Accel::Tcg, Path::new("/fw/c"), Path::new("/fw/v"), None);
        let j = a.join(" ");
        assert!(j.contains("-M q35 -accel tcg,thread=single -cpu max"), "{j}");
        assert!(j.contains("format=qcow2,snapshot=on,file=/img/a"));
    }

    #[test]
    fn foreign_arch_is_emulated() {
        let other = match Arch::host() {
            Arch::Aarch64 => Arch::X86_64,
            Arch::X86_64 => Arch::Aarch64,
        };
        assert_eq!(best_accel(other), Accel::Tcg);
    }
}
