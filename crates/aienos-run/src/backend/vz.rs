//! macOS backend: Apple Virtualization.framework (macOS 13+ for the EFI boot
//! loader; Apple Silicon runs the aarch64 guest natively, no emulation).
//!
//! Machine: VZGenericPlatformConfiguration + VZEFIBootLoader with a per-user
//! EFI variable store (VZ's own format, so a file of its own next to QEMU's).
//! A `uefi-app` image is put on a small raw GPT disk whose ESP holds
//! \EFI\BOOT\BOOTAA64.EFI (crate::esp); a `disk-raw` image boots as is.
//! Devices: virtio console (the shared Console), virtio-blk boot disk
//! (read-only), NVMe Store disk (the kernel's Store driver is NVMe),
//! virtio-net on the runner's user-mode network (crate::usernet), virtio
//! entropy (M1/M2 guests have no RNDR). Every VM call runs on one serial
//! dispatch queue (the queue the VM was created with).
//!
//! The binary must carry the com.apple.security.virtualization entitlement
//! (docs/AIENOS_RUN_MACOS.md: codesign); without it the VM does not start.
//! Unlike KVM and QEMU here, VZ reboots the guest in place on a PSCI reset.

use super::objc::{self, alloc, file_handle, file_url, msg, new_obj, nsarray, Id, Queue};
use super::{Backend, BackendError, VmExit, VmSpec};
use crate::console::Console;
use crate::manifest::{Arch, ImageKind};
use crate::usernet::UserNet;
use std::fs::File;
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// VZVirtualMachineState values (VZVirtualMachine.h).
const ST_STOPPED: isize = 0;
const ST_RUNNING: isize = 1;
const ST_ERROR: isize = 3;

/// Boot disk size for a uefi-app image (FAT16 ESP inside).
const BOOT_DISK_MIB: u64 = 64;
/// After a stop request, how long the guest gets before the plug is pulled.
const STOP_GRACE: Duration = Duration::from_secs(3);

#[derive(Default)]
pub struct VzBackend;

fn vmm(e: impl std::fmt::Display) -> BackendError {
    BackendError::Vmm(e.to_string())
}

fn os_pipe() -> Result<(File, File), BackendError> {
    let mut fds = [0i32; 2];
    // SAFETY: pipe writes two descriptors into fds.
    if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 {
        return Err(vmm(format!("pipe: {}", std::io::Error::last_os_error())));
    }
    // SAFETY: both descriptors are fresh and owned by the returned Files.
    Ok(unsafe { (File::from_raw_fd(fds[0]), File::from_raw_fd(fds[1])) })
}

fn check_err(what: &str, obj: Id, err: Id) -> Result<Id, String> {
    if obj.is_null() || !err.is_null() {
        return Err(format!("{what}: {}", objc::describe(err)));
    }
    Ok(obj)
}

/// VZ's EFI variable store differs from edk2's vars.fd: its own file.
fn vz_vars(spec: &VmSpec) -> PathBuf {
    match &spec.firmware_vars {
        Some(v) => v.with_file_name("efi-vars-vz.bin"),
        None => spec.scratch.join("efi-vars-vz.bin"),
    }
}

impl Backend for VzBackend {
    fn name(&self) -> &'static str {
        "vz"
    }

    fn probe(&self, spec: &VmSpec) -> Result<(), BackendError> {
        if !cfg!(target_arch = "aarch64") || spec.arch != Arch::Aarch64 {
            return Err(BackendError::Unavailable(format!(
                "vz runs aarch64 guests on Apple Silicon only (guest {})",
                spec.arch.as_str()
            )));
        }
        if !matches!(spec.image_kind, ImageKind::UefiApp | ImageKind::DiskRaw) {
            return Err(BackendError::Unavailable(format!(
                "vz boots uefi-app and disk-raw images, not {}",
                spec.image_kind.as_str()
            )));
        }
        if let Some(f) = spec
            .forwards
            .iter()
            .find(|f| f.proto != crate::net::Proto::Udp)
        {
            return Err(BackendError::Unavailable(format!(
                "vz forwards UDP only (yet), not {f}"
            )));
        }
        let _p = objc::Pool::new();
        if !objc::has_class("VZEFIBootLoader") {
            return Err(BackendError::Unavailable(
                "Virtualization.framework has no EFI boot loader here (needs macOS 13 or later)"
                    .into(),
            ));
        }
        // SAFETY: class method, no arguments.
        let ok: bool = unsafe { msg!(objc::class("VZVirtualMachine"), "isSupported"; bool) };
        if !ok {
            return Err(BackendError::Unavailable(
                "Virtualization.framework reports isSupported = NO (no hypervisor here)".into(),
            ));
        }
        Ok(())
    }

    fn run(&mut self, spec: &VmSpec, console: &Console) -> Result<VmExit, BackendError> {
        let _p = objc::Pool::new();
        let boot = match spec.image_kind {
            ImageKind::UefiApp => {
                let d = spec.scratch.join("vz-boot.img");
                crate::esp::write_boot_disk(&d, &spec.image, BOOT_DISK_MIB).map_err(vmm)?;
                d
            }
            _ => spec.image.clone(),
        };
        let vars = vz_vars(spec);
        if let Some(d) = vars.parent() {
            std::fs::create_dir_all(d).map_err(vmm)?;
        }
        let (mut out_r, out_w) = os_pipe()?; // guest -> host
        let (in_r, mut in_w) = os_pipe()?; // host -> guest
                                           // The runner's own user-mode network on a datagram socket pair, so the
                                           // guest sees the 10.0.2.x addresses its kernel expects (vmnet NAT would
                                           // hand it a DHCP subnet instead).
                                           // It also answers the kernel's network self-test in-process (crate::selfecho).
        let token = crate::selfecho::new_token().map_err(|e| vmm(format!("network: {e}")))?;
        let (net, vm_end) = UserNet::start(&spec.forwards, Some(token))
            .map_err(|e| vmm(format!("network: {e}")))?;
        let q = Queue::new("com.aienos.run.vz");
        let dev = Devices {
            boot: &boot,
            vars: &vars,
            out_w: out_w.as_raw_fd(),
            in_r: in_r.as_raw_fd(),
            net: vm_end.as_raw_fd(),
        };
        let vm = q
            .sync(|| -> Result<Id, String> {
                // SAFETY: plain Objective-C calls with the documented signatures;
                // every object is checked before use.
                unsafe { build_and_start(spec, &q, &dev) }
            })
            .map_err(vmm)?;
        console.vm_started();
        let mut vm = VzVm { q, vm };
        set_nonblocking(&out_r)?;

        let t_start = Instant::now();
        let mut stop_at: Option<Instant> = None;
        let mut buf = [0u8; 4096];
        let exit = loop {
            // Guest output: drain what is there (poll keeps this loop cheap).
            let mut pfd = libc::pollfd {
                fd: out_r.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            };
            // SAFETY: one valid pollfd, 2 ms timeout.
            unsafe { libc::poll(&mut pfd, 1, 2) };
            while let Ok(n) = out_r.read(&mut buf) {
                if n == 0 {
                    break;
                }
                console.guest_output(&buf[..n]);
            }
            let mut typed = Vec::new();
            while let Some(b) = console.pop_input() {
                typed.push(b);
            }
            if !typed.is_empty() {
                let _ = in_w.write_all(&typed);
            }
            if let Some(Err(e)) = objc::START_DONE.poll() {
                break VmExit::Failed(format!("start failed: {e}{}", entitlement_hint(&e)));
            }
            match vm.state() {
                ST_STOPPED if objc::START_DONE.poll().is_some() => {
                    break if stop_at.is_some() {
                        VmExit::Stopped
                    } else {
                        VmExit::PowerOff
                    };
                }
                ST_ERROR => {
                    break VmExit::Failed("the virtual machine entered the error state".into())
                }
                _ => {}
            }
            if objc::START_DONE.poll().is_none() && t_start.elapsed() > Duration::from_secs(30) {
                break VmExit::Failed(
                    "the virtual machine did not confirm its start within 30 s".into(),
                );
            }
            if console.stop_requested() {
                match stop_at {
                    None => {
                        stop_at = Some(Instant::now());
                        if !vm.request_stop() {
                            vm.force_stop();
                        }
                    }
                    Some(t) if t.elapsed() > STOP_GRACE => {
                        vm.force_stop();
                        break VmExit::Stopped;
                    }
                    _ => {}
                }
            }
        };
        if !matches!(exit, VmExit::PowerOff) {
            vm.force_stop();
        }
        // What is still in the pipe belongs to this run.
        while let Ok(n) = out_r.read(&mut buf) {
            if n == 0 {
                break;
            }
            console.guest_output(&buf[..n]);
        }
        drop(net);
        drop(vm_end);
        drop((out_w, in_r));
        Ok(exit)
    }
}

fn entitlement_hint(e: &str) -> &'static str {
    if e.contains("entitlement") || e.contains("Invalid virtual machine configuration") {
        " (is the binary signed with the com.apple.security.virtualization entitlement? see docs/AIENOS_RUN_MACOS.md)"
    } else {
        ""
    }
}

fn set_nonblocking(f: &File) -> Result<(), BackendError> {
    // SAFETY: fcntl on an open descriptor we own.
    let ok = unsafe {
        let fl = libc::fcntl(f.as_raw_fd(), libc::F_GETFL);
        fl >= 0 && libc::fcntl(f.as_raw_fd(), libc::F_SETFL, fl | libc::O_NONBLOCK) == 0
    };
    if ok {
        Ok(())
    } else {
        Err(vmm(format!("fcntl: {}", std::io::Error::last_os_error())))
    }
}

/// Host descriptors and files the VM's devices attach to.
struct Devices<'a> {
    boot: &'a Path,
    vars: &'a Path,
    out_w: i32,
    in_r: i32,
    net: i32,
}

fn dup(fd: i32) -> Result<i32, String> {
    // SAFETY: dup returns a new descriptor or -1.
    let d = unsafe { libc::dup(fd) };
    if d < 0 {
        Err(format!("dup: {}", std::io::Error::last_os_error()))
    } else {
        Ok(d)
    }
}

unsafe fn build_and_start(spec: &VmSpec, q: &Queue, dev: &Devices) -> Result<Id, String> {
    let mut err: Id = std::ptr::null_mut();
    let cfg = new_obj("VZVirtualMachineConfiguration");
    msg!(cfg, "setPlatform:", new_obj("VZGenericPlatformConfiguration") => Id; ());

    // EFI boot loader with a persistent variable store (created on first run).
    let vars = file_url(&dev.vars.to_string_lossy());
    let store = if dev.vars.exists() {
        msg!(alloc("VZEFIVariableStore"), "initWithURL:", vars => Id; Id)
    } else {
        let s = msg!(alloc("VZEFIVariableStore"), "initCreatingVariableStoreAtURL:options:error:",
                     vars => Id, 0usize => usize, &mut err => *mut Id; Id);
        check_err("EFI variable store", s, err)?
    };
    let bl = new_obj("VZEFIBootLoader");
    msg!(bl, "setVariableStore:", store => Id; ());
    msg!(cfg, "setBootLoader:", bl => Id; ());
    msg!(cfg, "setCPUCount:", spec.cpus as usize => usize; ());
    msg!(cfg, "setMemorySize:", u64::from(spec.mem_mib) << 20 => u64; ());

    // Disks: boot disk (virtio-blk, read-only: the cached image stays pristine)
    // then the Store disk on NVMe.
    let mut disks: Vec<Id> = Vec::new();
    let att = msg!(alloc("VZDiskImageStorageDeviceAttachment"), "initWithURL:readOnly:error:",
                   file_url(&dev.boot.to_string_lossy()) => Id, true => bool, &mut err => *mut Id; Id);
    let att = check_err("boot disk", att, err)?;
    disks.push(
        msg!(alloc("VZVirtioBlockDeviceConfiguration"), "initWithAttachment:", att => Id; Id),
    );
    if let Some(store_disk) = &spec.store_disk {
        let att = msg!(alloc("VZDiskImageStorageDeviceAttachment"), "initWithURL:readOnly:error:",
                       file_url(&store_disk.to_string_lossy()) => Id, false => bool, &mut err => *mut Id; Id);
        let att = check_err("store disk", att, err)?;
        disks.push(msg!(alloc("VZNVMExpressControllerDeviceConfiguration"), "initWithAttachment:", att => Id; Id));
    }
    msg!(cfg, "setStorageDevices:", nsarray(&disks) => Id; ());

    // Console: virtio console port on the runner's pipes (the VM keeps dups).
    let serial = new_obj("VZVirtioConsoleDeviceSerialPortConfiguration");
    let fa = msg!(alloc("VZFileHandleSerialPortAttachment"), "initWithFileHandleForReading:fileHandleForWriting:",
                  file_handle(dup(dev.in_r)?) => Id, file_handle(dup(dev.out_w)?) => Id; Id);
    msg!(serial, "setAttachment:", fa => Id; ());
    msg!(cfg, "setSerialPorts:", nsarray(&[serial]) => Id; ());

    // Entropy: the guest has no RNDR under Hypervisor.framework on M1/M2; a VM
    // edition kernel draws from this virtio entropy device (core/vrng.c).
    msg!(cfg, "setEntropyDevices:", nsarray(&[new_obj("VZVirtioEntropyDeviceConfiguration")]) => Id; ());

    // Network: raw Ethernet frames on the runner's socket (one datagram per
    // frame, MTU 1500); crate::usernet on the other end routes them.
    let n = new_obj("VZVirtioNetworkDeviceConfiguration");
    let a = msg!(alloc("VZFileHandleNetworkDeviceAttachment"), "initWithFileHandle:", file_handle(dev.net) => Id; Id);
    if a.is_null() {
        return Err("network: VZFileHandleNetworkDeviceAttachment init returned nil".into());
    }
    msg!(n, "setAttachment:", a => Id; ());
    msg!(cfg, "setNetworkDevices:", nsarray(&[n]) => Id; ());

    let ok: bool = msg!(cfg, "validateWithError:", &mut err => *mut Id; bool);
    if !ok {
        return Err(format!("VM configuration refused: {}", objc::describe(err)));
    }
    let vm = msg!(alloc("VZVirtualMachine"), "initWithConfiguration:queue:", cfg => Id, q.raw() => Id; Id);
    if vm.is_null() {
        return Err("VZVirtualMachine init returned nil".into());
    }
    objc::START_DONE.reset();
    msg!(vm, "startWithCompletionHandler:", objc::start_block().ptr() => Id; ());
    Ok(vm)
}

struct VzVm {
    q: Queue,
    vm: Id,
}

impl VzVm {
    fn state(&self) -> isize {
        let vm = self.vm as usize;
        // SAFETY: state is read on the VM's queue.
        self.q.sync(|| unsafe { msg!(vm as Id, "state"; isize) })
    }

    /// Asks the guest to power off (VZ requestStop: a power-button event).
    fn request_stop(&mut self) -> bool {
        let vm = self.vm as usize;
        // SAFETY: canRequestStop / requestStopWithError: on the VM queue.
        self.q.sync(|| unsafe {
            let vm = vm as Id;
            let can: bool = msg!(vm, "canRequestStop"; bool);
            if !can {
                return false;
            }
            let mut err: Id = std::ptr::null_mut();
            msg!(vm, "requestStopWithError:", &mut err => *mut Id; bool)
        })
    }

    /// Pulls the plug and waits (bounded) for VZ to confirm.
    fn force_stop(&mut self) {
        if self.state() != ST_RUNNING {
            return;
        }
        let vm = self.vm as usize;
        objc::STOP_DONE.reset();
        // SAFETY: stopWithCompletionHandler: (macOS 12+) on the VM queue.
        self.q.sync(|| unsafe {
            msg!(vm as Id, "stopWithCompletionHandler:", objc::stop_block().ptr() => Id; ())
        });
        for _ in 0..100 {
            if objc::STOP_DONE.poll().is_some() {
                break;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
    }
}
