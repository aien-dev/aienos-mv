//! QEMU backend: the day-one path on every host that has QEMU. It uses the host's
//! hardware accelerator when the guest architecture matches (KVM on Linux, HVF on macOS,
//! WHPX on Windows) and plain TCG emulation otherwise (for example x86_64 images on the
//! aarch64 Spark: emulated, never accelerated).
//!
//! Where QEMU comes from: `AIENOS_RUN_QEMU_DIR`, then an `aienos-qemu` folder next to the
//! aienos-run executable, then `<cache>/aienos-qemu` (the bundle `aienos-run` unpacks on
//! Windows, see `crate::bundle`), then the standard Windows install folder, then `PATH`.
//! UEFI firmware is taken from that QEMU's own `share` folder first.
//!
//! The kernel-as-UEFI-app image is booted the way the repo's QEMU gates boot it: UEFI
//! firmware (edk2 AAVMF/OVMF) in pflash, the app on a virtual FAT ESP.
//!
//! I/O: the guest's serial port and QEMU's QMP monitor are loopback TCP connections that
//! QEMU opens to listeners aienos-run created first (QEMU is the client, so there is no
//! port race and no named-pipe code). The same path runs on Windows, macOS and Linux;
//! `AIENOS_RUN_QEMU_SERIAL=stdio` keeps the serial port on QEMU's stdin/stdout instead.
//! A stop request runs the clean-stop sequence in [`super::qmp`].

use super::qmp::{Qmp, StopAction, StopPlan, Stopper};
use super::{Backend, BackendError, VmExit, VmSpec};
use crate::console::Console;
use crate::manifest::{Arch, ImageKind};
use crate::net::Forward;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

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

    pub fn parse(s: &str) -> Option<Accel> {
        match s {
            "kvm" => Some(Accel::Kvm),
            "hvf" => Some(Accel::Hvf),
            "whpx" => Some(Accel::Whpx),
            "tcg" => Some(Accel::Tcg),
            _ => None,
        }
    }
}

/// How the guest's serial port reaches aienos-run.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SerialMode {
    /// Loopback TCP, QEMU connects to aienos-run (default on every host).
    Tcp,
    /// QEMU's own stdin/stdout.
    Stdio,
}

/// Concrete endpoints for one run (separate from [`SerialMode`] so tests can build a
/// command line without sockets).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SerialEndpoint {
    Stdio,
    Tcp(u16),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Io {
    pub serial: SerialEndpoint,
    /// Port of aienos-run's QMP listener; None runs without QMP (stop = kill).
    pub qmp: Option<u16>,
}

pub struct QemuBackend {
    /// Force an accelerator (tests and measurements); None picks the best one.
    pub accel: Option<Accel>,
    /// Folders searched for QEMU before `PATH`, in order.
    pub search_dirs: Vec<PathBuf>,
    pub serial: SerialMode,
    pub stop_plan: StopPlan,
}

impl Default for QemuBackend {
    fn default() -> Self {
        QemuBackend {
            accel: None,
            search_dirs: Vec::new(),
            serial: SerialMode::Tcp,
            stop_plan: StopPlan::default(),
        }
    }
}

impl QemuBackend {
    /// The backend as `aienos-run` configures it from the environment:
    /// `AIENOS_RUN_QEMU_DIR`, `AIENOS_RUN_QEMU_ACCEL` (kvm|hvf|whpx|tcg),
    /// `AIENOS_RUN_QEMU_SERIAL` (tcp|stdio).
    pub fn from_env() -> QemuBackend {
        let get = |k: &str| std::env::var(k).ok().filter(|v| !v.is_empty());
        let exe_dir = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(Path::to_path_buf));
        let cache = crate::paths::Paths::from_env(|k| std::env::var(k).ok())
            .ok()
            .map(|p| p.cache);
        QemuBackend {
            accel: get("AIENOS_RUN_QEMU_ACCEL").and_then(|a| Accel::parse(&a)),
            search_dirs: search_dirs(
                get("AIENOS_RUN_QEMU_DIR").map(PathBuf::from),
                exe_dir.as_deref(),
                cache.as_deref(),
            ),
            serial: match get("AIENOS_RUN_QEMU_SERIAL").as_deref() {
                Some("stdio") => SerialMode::Stdio,
                _ => SerialMode::Tcp,
            },
            stop_plan: StopPlan::default(),
        }
    }
}

/// Name of the folder the Windows QEMU bundle unpacks to.
pub const BUNDLE_DIR: &str = "aienos-qemu";

/// The QEMU search order (see the module comment).
pub fn search_dirs(
    explicit: Option<PathBuf>,
    exe_dir: Option<&Path>,
    cache: Option<&Path>,
) -> Vec<PathBuf> {
    let mut v = Vec::new();
    v.extend(explicit);
    if let Some(d) = exe_dir {
        v.push(d.join(BUNDLE_DIR));
    }
    if let Some(c) = cache {
        v.push(c.join(BUNDLE_DIR));
    }
    if cfg!(windows) {
        v.push(PathBuf::from(r"C:\Program Files\qemu"));
    }
    v
}

pub fn qemu_binary(arch: Arch) -> &'static str {
    match arch {
        Arch::Aarch64 => "qemu-system-aarch64",
        Arch::X86_64 => "qemu-system-x86_64",
    }
}

fn exe_name(bin: &str) -> String {
    if cfg!(windows) {
        format!("{bin}.exe")
    } else {
        bin.to_string()
    }
}

/// Full path of the QEMU system emulator for `arch`: the first search folder that has it,
/// else the first `PATH` entry that has it.
pub fn find_qemu(arch: Arch, dirs: &[PathBuf]) -> Option<PathBuf> {
    let name = exe_name(qemu_binary(arch));
    let path_dirs: Vec<PathBuf> = std::env::var_os("PATH")
        .map(|p| std::env::split_paths(&p).collect())
        .unwrap_or_default();
    dirs.iter()
        .chain(path_dirs.iter())
        .map(|d| d.join(&name))
        .find(|p| p.is_file())
}

/// edk2 file names inside a QEMU installation's share folder: (code, vars template).
fn bundled_firmware_names(arch: Arch) -> (&'static str, &'static str) {
    match arch {
        Arch::Aarch64 => ("edk2-aarch64-code.fd", "edk2-arm-vars.fd"),
        Arch::X86_64 => ("edk2-x86_64-code.fd", "edk2-i386-vars.fd"),
    }
}

/// Known system firmware locations: (code, vars template).
fn firmware_candidates(arch: Arch) -> &'static [(&'static str, &'static str)] {
    match arch {
        Arch::Aarch64 => &[
            (
                "/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd",
                "/usr/share/AAVMF/AAVMF_VARS.fd",
            ),
            (
                "/usr/share/AAVMF/AAVMF_CODE.fd",
                "/usr/share/AAVMF/AAVMF_VARS.fd",
            ),
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
            (
                "/usr/share/OVMF/OVMF_CODE_4M.fd",
                "/usr/share/OVMF/OVMF_VARS_4M.fd",
            ),
            (
                "/usr/share/OVMF/OVMF_CODE.fd",
                "/usr/share/OVMF/OVMF_VARS.fd",
            ),
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

/// Firmware for `arch`: from the QEMU installation at `qemu` (its `share` folder, or
/// `../share/qemu` for a Unix prefix) first, then the system locations.
pub fn find_firmware(arch: Arch, qemu: Option<&Path>) -> Option<(PathBuf, PathBuf)> {
    let (code, vars) = bundled_firmware_names(arch);
    let mut cands: Vec<(PathBuf, PathBuf)> = Vec::new();
    if let Some(dir) = qemu.and_then(Path::parent) {
        for share in [dir.join("share"), dir.join("../share/qemu")] {
            cands.push((share.join(code), share.join(vars)));
        }
    }
    cands.extend(
        firmware_candidates(arch)
            .iter()
            .map(|(c, v)| (PathBuf::from(c), PathBuf::from(v))),
    );
    cands.into_iter().find(|(c, v)| c.is_file() && v.is_file())
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

/// A path inside a QEMU `-drive` option: commas are doubled (QEMU's option escaping).
pub fn opt_path(p: &Path) -> String {
    p.display().to_string().replace(',', ",,")
}

/// CPU model per accelerator. WHPX takes no `host` model in QEMU (KVM and HVF only), so
/// it gets QEMU's default `qemu64`, the model the repo's x86 gates also boot.
fn cpu_model(accel: Accel) -> &'static str {
    match accel {
        Accel::Tcg => "max",
        Accel::Whpx => "qemu64",
        Accel::Kvm | Accel::Hvf => "host",
    }
}

/// The full QEMU command line for `spec` (separate so tests can check it).
pub fn command_line(
    spec: &VmSpec,
    accel: Accel,
    firmware_code: &Path,
    vars: &Path,
    esp_dir: Option<&Path>,
    io: Io,
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
    push(if accel == Accel::Tcg {
        "tcg,thread=single"
    } else {
        accel.as_str()
    });
    push("-cpu");
    push(cpu_model(accel));
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
        opt_path(firmware_code)
    ));
    push("-drive");
    push(&format!("if=pflash,format=raw,file={}", opt_path(vars)));
    match spec.image_kind {
        ImageKind::UefiApp => {
            let esp = esp_dir.expect("uefi-app needs an ESP directory");
            push("-drive");
            push(&format!(
                "if=none,id=esp,format=raw,file=fat:rw:{}",
                opt_path(esp)
            ));
            push("-device");
            push("virtio-blk-pci,drive=esp");
        }
        ImageKind::Iso => {
            push("-drive");
            push(&format!(
                "if=none,id=os,media=cdrom,readonly=on,file={}",
                opt_path(&spec.image)
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
                opt_path(&spec.image)
            ));
            push("-device");
            push("virtio-blk-pci,drive=os");
        }
    }
    if let Some(store) = &spec.store_disk {
        push("-drive");
        push(&format!(
            "if=none,id=store,format=raw,file={}",
            opt_path(store)
        ));
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
    match io.serial {
        SerialEndpoint::Stdio => push("stdio,id=s0,signal=off"),
        SerialEndpoint::Tcp(p) => push(&format!("socket,id=s0,host=127.0.0.1,port={p},nodelay=on")),
    }
    push("-serial");
    push("chardev:s0");
    if let Some(p) = io.qmp {
        push("-chardev");
        push(&format!("socket,id=qmp0,host=127.0.0.1,port={p}"));
        push("-mon");
        push("chardev=qmp0,mode=control");
    }
    push("-monitor");
    push("none");
    a
}

/// Waits for QEMU to connect to `l`; gives up when QEMU exits or `limit` passes.
fn accept_from(l: &TcpListener, child: &mut Child, limit: Duration) -> Result<TcpStream, String> {
    l.set_nonblocking(true).map_err(|e| e.to_string())?;
    let t0 = Instant::now();
    loop {
        match l.accept() {
            Ok((s, _)) => {
                s.set_nonblocking(false).map_err(|e| e.to_string())?;
                let _ = s.set_nodelay(true);
                return Ok(s);
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {}
            Err(e) => return Err(e.to_string()),
        }
        if let Ok(Some(st)) = child.try_wait() {
            return Err(format!("QEMU exited ({st}) before connecting"));
        }
        if t0.elapsed() > limit {
            return Err("QEMU did not connect in time".into());
        }
        std::thread::sleep(Duration::from_millis(2));
    }
}

fn listener() -> Result<(TcpListener, u16), BackendError> {
    let l = TcpListener::bind("127.0.0.1:0")
        .map_err(|e| BackendError::Vmm(format!("cannot listen on loopback: {e}")))?;
    let p = l
        .local_addr()
        .map_err(|e| BackendError::Vmm(e.to_string()))?
        .port();
    Ok((l, p))
}

impl QemuBackend {
    fn qemu(&self, arch: Arch) -> Option<PathBuf> {
        find_qemu(arch, &self.search_dirs)
    }
}

impl Backend for QemuBackend {
    fn name(&self) -> &'static str {
        "qemu"
    }

    fn probe(&self, spec: &VmSpec) -> Result<(), BackendError> {
        let Some(bin) = self.qemu(spec.arch) else {
            return Err(BackendError::Unavailable(format!(
                "{} is not installed",
                qemu_binary(spec.arch)
            )));
        };
        if find_firmware(spec.arch, Some(&bin)).is_none() {
            return Err(BackendError::Unavailable(format!(
                "no UEFI firmware for {} found (install qemu-efi-aarch64 / ovmf)",
                spec.arch.as_str()
            )));
        }
        Ok(())
    }

    fn run(&mut self, spec: &VmSpec, console: &Console) -> Result<VmExit, BackendError> {
        let bin = self
            .qemu(spec.arch)
            .ok_or_else(|| BackendError::Unavailable("QEMU not found".into()))?;
        let (code, vars_template) = find_firmware(spec.arch, Some(&bin))
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
        let serial_l = match self.serial {
            SerialMode::Tcp => Some(listener()?),
            SerialMode::Stdio => None,
        };
        let (qmp_l, qmp_port) = listener()?;
        let io = Io {
            serial: match &serial_l {
                Some((_, p)) => SerialEndpoint::Tcp(*p),
                None => SerialEndpoint::Stdio,
            },
            qmp: Some(qmp_port),
        };
        let args = command_line(spec, accel, &code, &vars, esp.as_deref(), io);
        let stdio_serial = serial_l.is_none();
        let mut cmd = Command::new(&bin);
        cmd.args(&args)
            .stdin(if stdio_serial {
                Stdio::piped()
            } else {
                Stdio::null()
            })
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        {
            // CREATE_NO_WINDOW: QEMU gets no console of its own and no Ctrl+C from ours
            // (aienos-run turns Ctrl+C into the clean-stop sequence instead).
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x0800_0000);
        }
        let mut child = cmd
            .spawn()
            .map_err(|e| BackendError::Vmm(format!("cannot start {}: {e}", bin.display())))?;
        let mut stdin = child.stdin.take();
        let mut stdout = child.stdout.take().expect("piped stdout");
        let mut stderr = child.stderr.take().expect("piped stderr");
        let connect_limit = Duration::from_secs(30);
        // QEMU opens its chardevs in command-line order: serial first, then QMP.
        let mut serial = match &serial_l {
            Some((l, _)) => match accept_from(l, &mut child, connect_limit) {
                Ok(s) => Some(s),
                Err(e) => {
                    let _ = child.kill();
                    let mut t = String::new();
                    let _ = stderr.read_to_string(&mut t);
                    return Ok(VmExit::Failed(format!("serial: {e}: {}", t.trim())));
                }
            },
            None => None,
        };
        let mut qmp = accept_from(&qmp_l, &mut child, connect_limit)
            .ok()
            .and_then(|s| Qmp::handshake(s, Duration::from_secs(5)).ok());
        console.vmm_started();
        let serial_reader = serial
            .as_ref()
            .map(|s| s.try_clone())
            .transpose()
            .map_err(vmm)?;
        let mut err_text = String::new();
        let mut out_text = Vec::new();
        let mut stop_at: Option<Instant> = None;
        let mut stopper = Stopper::new(self.stop_plan);
        let status = std::thread::scope(|s| {
            s.spawn(|| {
                let mut buf = [0u8; 4096];
                match serial_reader {
                    Some(mut r) => {
                        while let Ok(n) = r.read(&mut buf) {
                            if n == 0 {
                                break;
                            }
                            console.guest_output(&buf[..n]);
                        }
                        let _ = stdout.read_to_end(&mut out_text);
                    }
                    None => {
                        while let Ok(n) = stdout.read(&mut buf) {
                            if n == 0 {
                                break;
                            }
                            console.guest_output(&buf[..n]);
                        }
                    }
                }
            });
            s.spawn(|| {
                let _ = stderr.read_to_string(&mut err_text);
            });
            loop {
                if let Some(st) = child.try_wait().map_err(vmm)? {
                    return Ok(st);
                }
                if console.stop_requested() && stop_at.is_none() {
                    stop_at = Some(Instant::now());
                }
                if let Some(t) = stop_at {
                    if let Some(step) = stopper.poll(t.elapsed()) {
                        let mut type_line = |line: &[u8]| {
                            if let Some(w) = serial.as_mut() {
                                let _ = w.write_all(line);
                            } else if let Some(w) = stdin.as_mut() {
                                let _ = w.write_all(line);
                                let _ = w.flush();
                            }
                        };
                        match step {
                            StopAction::PowerButton => {
                                if let Some(q) = qmp.as_mut() {
                                    let _ = q.execute("system_powerdown");
                                }
                            }
                            StopAction::TypePoweroff => type_line(b"poweroff\r"),
                            StopAction::Quit => {
                                if let Some(q) = qmp.as_mut() {
                                    let _ = q.send("quit");
                                }
                            }
                            StopAction::Kill => {
                                let _ = child.kill();
                            }
                        }
                    }
                }
                let mut pending = Vec::new();
                while let Some(b) = console.pop_input() {
                    pending.push(b);
                }
                if !pending.is_empty() {
                    if let Some(w) = serial.as_mut() {
                        let _ = w.write_all(&pending);
                    } else if let Some(w) = stdin.as_mut() {
                        let _ = w.write_all(&pending);
                        let _ = w.flush();
                    }
                }
                std::thread::sleep(Duration::from_millis(2));
            }
        })?;
        drop(serial);
        if stop_at.is_some() {
            return Ok(VmExit::Stopped);
        }
        if status.success() {
            return Ok(VmExit::PowerOff);
        }
        let out = String::from_utf8_lossy(&out_text);
        Ok(VmExit::Failed(format!(
            "QEMU exit {status}: {} {}",
            err_text.trim(),
            out.trim()
        )))
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

    const STDIO: Io = Io {
        serial: SerialEndpoint::Stdio,
        qmp: None,
    };

    #[test]
    fn kvm_command_line() {
        let s = spec(ImageKind::UefiApp, Arch::Aarch64);
        let a = command_line(
            &s,
            Accel::Kvm,
            Path::new("/fw/c"),
            Path::new("/fw/v"),
            Some(Path::new("/s/esp")),
            STDIO,
        );
        let j = a.join(" ");
        assert!(
            j.contains("-M virt,gic-version=3 -accel kvm -cpu host"),
            "{j}"
        );
        assert!(j.contains("file=fat:rw:/s/esp"));
        assert!(j.contains("hostfwd=udp:127.0.0.1:47030-:47030"));
        assert!(j.contains("nvme,serial=aienos-store,drive=store"));
        assert!(j.contains("-no-reboot"));
        assert!(j.contains("stdio,id=s0,signal=off"));
    }

    #[test]
    fn tcg_x86_disk_command_line() {
        let s = spec(ImageKind::DiskQcow2, Arch::X86_64);
        let a = command_line(
            &s,
            Accel::Tcg,
            Path::new("/fw/c"),
            Path::new("/fw/v"),
            None,
            STDIO,
        );
        let j = a.join(" ");
        assert!(
            j.contains("-M q35 -accel tcg,thread=single -cpu max"),
            "{j}"
        );
        assert!(j.contains("format=qcow2,snapshot=on,file=/img/a"));
    }

    /// The Windows path: WHPX, serial and QMP on loopback sockets QEMU dials.
    #[test]
    fn whpx_tcp_command_line() {
        let mut s = spec(ImageKind::DiskQcow2, Arch::X86_64);
        s.image = PathBuf::from(r"C:\Users\a,b\AppData\Local\AienOS\cache\images\x.qcow2");
        let io = Io {
            serial: SerialEndpoint::Tcp(50001),
            qmp: Some(50002),
        };
        let a = command_line(
            &s,
            Accel::Whpx,
            Path::new("/fw/c"),
            Path::new("/fw/v"),
            None,
            io,
        );
        let j = a.join(" ");
        assert!(j.contains("-M q35 -accel whpx -cpu qemu64"), "{j}");
        assert!(j.contains(
            "-chardev socket,id=s0,host=127.0.0.1,port=50001,nodelay=on -serial chardev:s0"
        ));
        assert!(j.contains(
            "-chardev socket,id=qmp0,host=127.0.0.1,port=50002 -mon chardev=qmp0,mode=control"
        ));
        assert!(
            j.contains(r"file=C:\Users\a,,b\AppData"),
            "commas doubled: {j}"
        );
        assert!(!j.contains("stdio"));
    }

    #[test]
    fn foreign_arch_is_emulated() {
        let other = match Arch::host() {
            Arch::Aarch64 => Arch::X86_64,
            Arch::X86_64 => Arch::Aarch64,
        };
        assert_eq!(best_accel(other), Accel::Tcg);
    }

    #[test]
    fn search_order_and_bundle_layout() {
        let d = std::env::temp_dir().join(format!("aienos-run-qemu-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let dirs = search_dirs(None, Some(&d.join("exe")), Some(&d.join("cache")));
        assert_eq!(dirs[0], d.join("exe").join(BUNDLE_DIR));
        assert_eq!(dirs[1], d.join("cache").join(BUNDLE_DIR));
        // A bundle in the cache: binary at the top, firmware in share/.
        let b = d.join("cache").join(BUNDLE_DIR);
        std::fs::create_dir_all(b.join("share")).unwrap();
        let bin = b.join(exe_name("qemu-system-x86_64"));
        std::fs::write(&bin, b"").unwrap();
        std::fs::write(b.join("share/edk2-x86_64-code.fd"), b"c").unwrap();
        std::fs::write(b.join("share/edk2-i386-vars.fd"), b"v").unwrap();
        assert_eq!(find_qemu(Arch::X86_64, &dirs), Some(bin.clone()));
        let (c, v) = find_firmware(Arch::X86_64, Some(&bin)).unwrap();
        assert_eq!(c, b.join("share/edk2-x86_64-code.fd"));
        assert_eq!(v, b.join("share/edk2-i386-vars.fd"));
        // An explicit folder wins.
        let first = search_dirs(Some(d.join("mine")), None, Some(&d.join("cache")));
        assert_eq!(first[0], d.join("mine"));
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn accel_names_round_trip() {
        for a in [Accel::Kvm, Accel::Hvf, Accel::Whpx, Accel::Tcg] {
            assert_eq!(Accel::parse(a.as_str()), Some(a));
        }
        assert_eq!(Accel::parse("vbox"), None);
    }
}
