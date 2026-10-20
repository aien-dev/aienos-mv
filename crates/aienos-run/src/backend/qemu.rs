//! QEMU backend: qemu-system-aarch64 with the host accelerator (hvf on macOS,
//! kvm on aarch64 Linux, tcg elsewhere) and edk2/AAVMF UEFI firmware. The
//! fallback and the measured comparison point for the native backends.

use super::{Backend, Console, NetMode, Proto, RunningVm, StoreBus, VmSpec};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

pub struct Qemu {
    pub binary: String,
    pub accel: &'static str,
    pub qmp: PathBuf,
}

const FIRMWARE: &[&str] = &[
    "/opt/homebrew/share/qemu/edk2-aarch64-code.fd",
    "/usr/local/share/qemu/edk2-aarch64-code.fd",
    "/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd",
    "/usr/share/AAVMF/AAVMF_CODE.fd",
    "/usr/share/qemu-efi-aarch64/QEMU_EFI.fd",
    "/usr/share/qemu/edk2-aarch64-code.fd",
];

pub fn firmware() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("AIENOS_RUN_QEMU_FIRMWARE") {
        return Some(PathBuf::from(p));
    }
    FIRMWARE.iter().map(PathBuf::from).find(|p| p.exists())
}

impl Qemu {
    pub fn for_host() -> Self {
        let accel = if cfg!(target_os = "macos") && cfg!(target_arch = "aarch64") {
            "hvf"
        } else if cfg!(target_os = "linux") && cfg!(target_arch = "aarch64") && Path::new("/dev/kvm").exists() {
            "kvm"
        } else {
            "tcg"
        };
        let qmp = std::env::temp_dir().join(format!("aienos-run-qmp-{}.sock", std::process::id()));
        Qemu { binary: std::env::var("AIENOS_RUN_QEMU").unwrap_or_else(|_| "qemu-system-aarch64".into()), accel, qmp }
    }

    /// The full argument list (unit-tested; no process needed).
    pub fn args(&self, spec: &VmSpec, fw: &Path) -> Vec<String> {
        let mut a: Vec<String> = Vec::new();
        let mut push = |s: &str| a.push(s.to_string());
        push("-M");
        push("virt,gic-version=3");
        push("-accel");
        push(self.accel);
        push("-cpu");
        push(if self.accel == "tcg" { "max" } else { "host" });
        push("-smp");
        push(&spec.cpus.to_string());
        push("-m");
        push(&spec.mem_mib.to_string());
        push("-drive");
        push(&format!("if=pflash,format=raw,readonly=on,file={}", fw.display()));
        push("-drive");
        push(&format!("if=pflash,format=raw,file={}", spec.efi_vars.display()));
        push("-drive");
        push(&format!(
            "if=none,id=boot,format=raw,file={}{}",
            spec.boot_disk.display(),
            if spec.boot_read_only { ",readonly=on" } else { "" }
        ));
        push("-device");
        push("virtio-blk-pci,drive=boot,bootindex=0");
        if let Some(s) = &spec.store_disk {
            push("-drive");
            push(&format!("if=none,id=store,format=raw,file={}", s.display()));
            push("-device");
            push(match spec.store_bus {
                StoreBus::Nvme => "nvme,serial=aienos-store,drive=store",
                StoreBus::VirtioBlk => "virtio-blk-pci,drive=store",
            });
        }
        match spec.net {
            NetMode::None => {
                push("-nic");
                push("none");
            }
            NetMode::Nat => {
                let mut nd = String::from("user,id=n0");
                for f in &spec.forwards {
                    let p = if f.proto == Proto::Udp { "udp" } else { "tcp" };
                    nd.push_str(&format!(",hostfwd={p}:127.0.0.1:{}-:{}", f.host, f.guest));
                }
                push("-netdev");
                push(&nd);
                push("-device");
                push(&match &spec.mac {
                    Some(m) => format!("virtio-net-pci,netdev=n0,mac={m}"),
                    None => "virtio-net-pci,netdev=n0".into(),
                });
            }
        }
        push("-device");
        push("ramfb");
        push("-display");
        push(if spec.framebuffer { "cocoa" } else { "none" });
        push("-serial");
        push("stdio");
        push("-monitor");
        push("none");
        push("-qmp");
        push(&format!("unix:{},server=on,wait=off", self.qmp.display()));
        push("-no-reboot");
        a
    }
}

impl Backend for Qemu {
    fn name(&self) -> &'static str {
        "qemu"
    }

    fn available(&self) -> Result<(), String> {
        let out = Command::new(&self.binary).arg("--version").output();
        match out {
            Ok(o) if o.status.success() => {}
            _ => return Err(format!("{} not found (install QEMU, e.g. `brew install qemu`)", self.binary)),
        }
        firmware().map(|_| ()).ok_or_else(|| "no aarch64 UEFI firmware (edk2-aarch64-code.fd) found".into())
    }

    fn boot(&self, spec: &VmSpec) -> Result<(Box<dyn RunningVm>, Console), String> {
        let fw = firmware().ok_or("no aarch64 UEFI firmware found")?;
        if !spec.efi_vars.exists() {
            let f = std::fs::File::create(&spec.efi_vars).map_err(|e| format!("{}: {e}", spec.efi_vars.display()))?;
            f.set_len(64 << 20).map_err(|e| format!("vars: {e}"))?;
        }
        let _ = std::fs::remove_file(&self.qmp);
        let mut child = Command::new(&self.binary)
            .args(self.args(spec, &fw))
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .map_err(|e| format!("start {}: {e}", self.binary))?;
        let from = child.stdout.take().ok_or("qemu stdout")?;
        let to = child.stdin.take().ok_or("qemu stdin")?;
        Ok((
            Box::new(QemuVm { child, qmp: self.qmp.clone() }),
            Console { from_guest: Box::new(from), to_guest: Box::new(to) },
        ))
    }
}

struct QemuVm {
    child: Child,
    qmp: PathBuf,
}

impl RunningVm for QemuVm {
    fn started(&mut self) -> Option<Result<(), String>> {
        Some(Ok(()))
    }

    fn poll_exit(&mut self) -> Option<Result<(), String>> {
        match self.child.try_wait() {
            Ok(Some(st)) if st.success() => Some(Ok(())),
            Ok(Some(st)) => Some(Err(format!("qemu exited with {st}"))),
            Ok(None) => None,
            Err(e) => Some(Err(format!("qemu wait: {e}"))),
        }
    }

    fn request_stop(&mut self) -> bool {
        #[cfg(unix)]
        {
            use std::os::unix::net::UnixStream;
            if let Ok(mut s) = UnixStream::connect(&self.qmp) {
                let _ = s.write_all(b"{\"execute\":\"qmp_capabilities\"}\n{\"execute\":\"system_powerdown\"}\n");
                return true;
            }
        }
        false
    }

    fn force_stop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_file(&self.qmp);
    }
}

#[cfg(test)]
mod tests {
    use super::super::{default_forwards, NetMode, StoreBus, VmSpec};
    use super::*;

    fn spec() -> VmSpec {
        VmSpec {
            boot_disk: "/c/boot.img".into(),
            boot_read_only: true,
            store_disk: Some("/c/store.img".into()),
            store_bus: StoreBus::Nvme,
            efi_vars: "/c/vars.fd".into(),
            cpus: 2,
            mem_mib: 1024,
            net: NetMode::Nat,
            mac: None,
            forwards: default_forwards(),
            framebuffer: false,
        }
    }

    #[test]
    fn args_carry_disks_forward_and_console() {
        let q = Qemu { binary: "qemu".into(), accel: "hvf", qmp: "/t/q.sock".into() };
        let a = q.args(&spec(), Path::new("/fw/code.fd")).join(" ");
        assert!(a.contains("-accel hvf -cpu host -smp 2 -m 1024"), "{a}");
        assert!(a.contains("file=/c/boot.img,readonly=on"), "{a}");
        assert!(a.contains("nvme,serial=aienos-store,drive=store"), "{a}");
        assert!(a.contains("hostfwd=udp:127.0.0.1:47030-:47030"), "{a}");
        assert!(a.contains("-serial stdio") && a.contains("-display none"), "{a}");
        assert!(a.contains("-qmp unix:/t/q.sock,server=on,wait=off"), "{a}");
    }

    #[test]
    fn tcg_uses_cpu_max() {
        let q = Qemu { binary: "qemu".into(), accel: "tcg", qmp: "/t/q.sock".into() };
        let a = q.args(&spec(), Path::new("/fw")).join(" ");
        assert!(a.contains("-accel tcg -cpu max"), "{a}");
    }
}
