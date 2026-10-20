//! macOS backend: Apple Virtualization.framework (macOS 13+ for the EFI boot
//! loader; Apple Silicon runs the aarch64 guest natively, no emulation).
//!
//! Machine: VZGenericPlatformConfiguration + VZEFIBootLoader with a per-user
//! EFI variable store; the boot disk is a raw GPT disk whose ESP holds
//! \EFI\BOOT\BOOTAA64.EFI (the same UEFI image QEMU virt boots). Devices:
//! virtio console (the runner's console pipe), virtio-blk boot disk, NVMe or
//! virtio-blk Store disk, virtio-net. Every VM call runs on one serial
//! dispatch queue (the queue the VM was created with).

use super::objc::{self, alloc, file_handle, file_url, msg, new_obj, nsarray, nsstring, Id, Queue};
use super::{Backend, Console, NetMode, RunningVm, StoreBus, VmSpec};
use std::fs::File;
use std::os::fd::FromRawFd;
use std::time::Duration;

extern "C" {
    fn pipe(fds: *mut i32) -> i32;
}

fn os_pipe() -> Result<(File, File), String> {
    let mut fds = [0i32; 2];
    // SAFETY: pipe writes two descriptors into fds.
    if unsafe { pipe(fds.as_mut_ptr()) } != 0 {
        return Err(format!("pipe: {}", std::io::Error::last_os_error()));
    }
    // SAFETY: both descriptors are fresh and owned by the returned Files.
    Ok(unsafe { (File::from_raw_fd(fds[0]), File::from_raw_fd(fds[1])) })
}

pub struct Vz;

/// VZVirtualMachineState values (VZVirtualMachine.h).
const ST_STOPPED: isize = 0;
const ST_RUNNING: isize = 1;
const ST_ERROR: isize = 3;

fn check_err(what: &str, obj: Id, err: Id) -> Result<Id, String> {
    if obj.is_null() || !err.is_null() {
        return Err(format!("{what}: {}", objc::describe(err)));
    }
    Ok(obj)
}

impl Backend for Vz {
    fn name(&self) -> &'static str {
        "vz"
    }

    fn available(&self) -> Result<(), String> {
        let _p = objc::Pool::new();
        // SAFETY: class method, no arguments.
        let ok: bool = unsafe { msg!(objc::class("VZVirtualMachine"), "isSupported"; bool) };
        if ok {
            Ok(())
        } else {
            Err("Virtualization.framework reports VZVirtualMachine.isSupported = NO (no hypervisor, or a VM without nested virtualization)".into())
        }
    }

    fn boot(&self, spec: &VmSpec) -> Result<(Box<dyn RunningVm>, Console), String> {
        let q = Queue::new("com.aienos.run.vz");
        let (out_r, out_w) = os_pipe()?; // guest -> host
        let (in_r, in_w) = os_pipe()?; // host -> guest
        let vm = q.sync(|| -> Result<Id, String> {
            // SAFETY: plain Objective-C calls with the documented signatures;
            // every object is checked before use.
            unsafe { build_and_start(spec, &q, &out_w, &in_r) }
        })?;
        // The VM holds its own duplicates through the NSFileHandles.
        drop(out_w);
        drop(in_r);
        Ok((Box::new(VzVm { q, vm, _keep: () }), Console { from_guest: Box::new(out_r), to_guest: Box::new(in_w) }))
    }
}

unsafe fn build_and_start(spec: &VmSpec, q: &Queue, out_w: &File, in_r: &File) -> Result<Id, String> {
    use std::os::fd::AsRawFd;
    let mut err: Id = std::ptr::null_mut();
    let cfg = new_obj("VZVirtualMachineConfiguration");
    msg!(cfg, "setPlatform:", new_obj("VZGenericPlatformConfiguration") => Id; ());

    // EFI boot loader with a persistent variable store (created on first run).
    let vars = file_url(&spec.efi_vars.to_string_lossy());
    let store = if spec.efi_vars.exists() {
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
    msg!(cfg, "setMemorySize:", spec.mem_mib << 20 => u64; ());

    // Disks: boot disk (virtio-blk) then the Store disk.
    let mut disks: Vec<Id> = Vec::new();
    let att = msg!(alloc("VZDiskImageStorageDeviceAttachment"), "initWithURL:readOnly:error:",
                   file_url(&spec.boot_disk.to_string_lossy()) => Id, spec.boot_read_only => bool, &mut err => *mut Id; Id);
    let att = check_err("boot disk", att, err)?;
    disks.push(msg!(alloc("VZVirtioBlockDeviceConfiguration"), "initWithAttachment:", att => Id; Id));
    if let Some(store_disk) = &spec.store_disk {
        let att = msg!(alloc("VZDiskImageStorageDeviceAttachment"), "initWithURL:readOnly:error:",
                       file_url(&store_disk.to_string_lossy()) => Id, false => bool, &mut err => *mut Id; Id);
        let att = check_err("store disk", att, err)?;
        let dev = match spec.store_bus {
            StoreBus::Nvme => msg!(alloc("VZNVMExpressControllerDeviceConfiguration"), "initWithAttachment:", att => Id; Id),
            StoreBus::VirtioBlk => msg!(alloc("VZVirtioBlockDeviceConfiguration"), "initWithAttachment:", att => Id; Id),
        };
        disks.push(dev);
    }
    msg!(cfg, "setStorageDevices:", nsarray(&disks) => Id; ());

    // Console: virtio console port on the runner's pipes.
    let serial = new_obj("VZVirtioConsoleDeviceSerialPortConfiguration");
    let fa = msg!(alloc("VZFileHandleSerialPortAttachment"), "initWithFileHandleForReading:fileHandleForWriting:",
                  file_handle(dup(in_r.as_raw_fd())?) => Id, file_handle(dup(out_w.as_raw_fd())?) => Id; Id);
    msg!(serial, "setAttachment:", fa => Id; ());
    msg!(cfg, "setSerialPorts:", nsarray(&[serial]) => Id; ());

    // Network: NAT (vmnet shared, guest gets DHCP on the host-only subnet).
    if let NetMode::Nat = spec.net {
        let n = new_obj("VZVirtioNetworkDeviceConfiguration");
        msg!(n, "setAttachment:", new_obj("VZNATNetworkDeviceAttachment") => Id; ());
        if let Some(mac) = &spec.mac {
            let m = msg!(alloc("VZMACAddress"), "initWithString:", nsstring(mac) => Id; Id);
            if !m.is_null() {
                msg!(n, "setMACAddress:", m => Id; ());
            }
        }
        msg!(cfg, "setNetworkDevices:", nsarray(&[n]) => Id; ());
    }

    // Optional framebuffer: a virtio GPU gives the firmware a GOP; the
    // kernel draws its console on it (shown only when a window is attached).
    if spec.framebuffer {
        let g = new_obj("VZVirtioGraphicsDeviceConfiguration");
        let so = msg!(alloc("VZVirtioGraphicsScanoutConfiguration"), "initWithWidthInPixels:heightInPixels:",
                      1024isize => isize, 768isize => isize; Id);
        msg!(g, "setScanouts:", nsarray(&[so]) => Id; ());
        msg!(cfg, "setGraphicsDevices:", nsarray(&[g]) => Id; ());
    }

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

fn dup(fd: i32) -> Result<i32, String> {
    extern "C" {
        fn dup(fd: i32) -> i32;
    }
    // SAFETY: dup returns a new descriptor or -1.
    let d = unsafe { dup(fd) };
    if d < 0 {
        Err(format!("dup: {}", std::io::Error::last_os_error()))
    } else {
        Ok(d)
    }
}

struct VzVm {
    q: Queue,
    vm: Id,
    _keep: (),
}
unsafe impl Send for VzVm {}

impl VzVm {
    fn state(&self) -> isize {
        let vm = self.vm as usize;
        // SAFETY: state is read on the VM's queue.
        self.q.sync(|| unsafe { msg!(vm as Id, "state"; isize) })
    }
}

impl RunningVm for VzVm {
    fn started(&mut self) -> Option<Result<(), String>> {
        objc::START_DONE.poll()
    }

    fn poll_exit(&mut self) -> Option<Result<(), String>> {
        if let Some(Err(e)) = objc::START_DONE.poll() {
            return Some(Err(format!("start failed: {e}")));
        }
        match self.state() {
            ST_STOPPED if objc::START_DONE.poll().is_some() => Some(Ok(())),
            ST_ERROR => Some(Err("virtual machine entered the error state".into())),
            _ => None,
        }
    }

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

    fn force_stop(&mut self) {
        if self.state() != ST_RUNNING {
            return;
        }
        let vm = self.vm as usize;
        objc::STOP_DONE.reset();
        // SAFETY: stopWithCompletionHandler: (macOS 12+) on the VM queue.
        self.q.sync(|| unsafe { msg!(vm as Id, "stopWithCompletionHandler:", objc::stop_block().ptr() => Id; ()) });
        for _ in 0..100 {
            if objc::STOP_DONE.poll().is_some() {
                break;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
    }
}
