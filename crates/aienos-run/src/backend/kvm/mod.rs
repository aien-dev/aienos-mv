//! Our own VMM on /dev/kvm (Linux, aarch64 host, aarch64 AienOS image).
//!
//! Direct boot, no firmware: the VMM loads the Aienux kernel's PE image into guest RAM,
//! writes the ACPI tables, memory map and direct-boot record (`boot.rs`), puts the boot
//! vCPU at EL1h with an identity map and caches on (what UEFI leaves), and runs it.
//! KVM provides the CPU, the GICv3 (in-kernel vGIC), the generic timer and PSCI (HVC);
//! the VMM emulates the PL011 serial port and turns PSCI SYSTEM_OFF/RESET into the end
//! of the run. References read (read-only, for the ioctl sequence; no code copied):
//! the Linux KVM API document (Documentation/virt/kvm/api.rst, GPL-2.0 docs), kvmtool
//! (GPL-2.0) and Firecracker's arm64 vCPU/GIC setup (Apache-2.0).

pub mod boot;
pub mod pl011;
pub mod sys;

use super::{Backend, BackendError, VmExit, VmSpec};
use crate::console::Console;
use crate::manifest::{Arch, ImageKind};
use boot::*;
use pl011::Pl011;
use std::os::fd::{AsRawFd, OwnedFd};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;
use sys::*;

#[derive(Default)]
pub struct KvmBackend;

/// Guest RAM, an anonymous host mapping registered with KVM.
struct GuestRam {
    ptr: *mut u8,
    len: usize,
}

unsafe impl Send for GuestRam {}
unsafe impl Sync for GuestRam {}

impl GuestRam {
    fn new(len: usize) -> std::io::Result<GuestRam> {
        let p = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                len,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE,
                -1,
                0,
            )
        };
        if p == libc::MAP_FAILED {
            return Err(std::io::Error::last_os_error());
        }
        Ok(GuestRam {
            ptr: p as *mut u8,
            len,
        })
    }
    fn slice(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.ptr, self.len) }
    }
}

impl Drop for GuestRam {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.ptr as *mut libc::c_void, self.len);
        }
    }
}

/// One vCPU: its fd and its shared `struct kvm_run` mapping.
struct Vcpu {
    fd: OwnedFd,
    run: *mut u8,
    run_len: usize,
}

unsafe impl Send for Vcpu {}
unsafe impl Sync for Vcpu {}

impl Drop for Vcpu {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.run as *mut libc::c_void, self.run_len);
        }
    }
}

impl Vcpu {
    fn rd_u32(&self, off: usize) -> u32 {
        unsafe { std::ptr::read_volatile(self.run.add(off) as *const u32) }
    }
    fn rd_u64(&self, off: usize) -> u64 {
        unsafe { std::ptr::read_volatile(self.run.add(off) as *const u64) }
    }
    fn mmio_data(&self) -> &mut [u8; 8] {
        unsafe { &mut *(self.run.add(RUN_MMIO_DATA) as *mut [u8; 8]) }
    }
    fn set_immediate_exit(&self) {
        unsafe { std::ptr::write_volatile(self.run.add(RUN_IMMEDIATE_EXIT), 1u8) }
    }
}

/// How the run ended, first writer wins.
const END_NONE: u32 = 0;
const END_OFF: u32 = 1;
const END_RESET: u32 = 2;
const END_STOP: u32 = 3;
const END_FAIL: u32 = 4;

struct Shared<'a> {
    console: &'a Console,
    uart: Mutex<Pl011>,
    stop: AtomicBool,
    end: AtomicU32,
    fail: OnceLock<String>,
    unhandled: Mutex<Vec<(u64, bool)>>,
}

impl Shared<'_> {
    fn finish(&self, how: u32, why: Option<String>) {
        if self
            .end
            .compare_exchange(END_NONE, how, Ordering::SeqCst, Ordering::SeqCst)
            .is_ok()
        {
            if let Some(w) = why {
                let _ = self.fail.set(w);
            }
        }
        self.stop.store(true, Ordering::SeqCst);
    }
}

extern "C" fn on_kick(_: libc::c_int) {}

fn vmm_err(e: std::io::Error) -> BackendError {
    BackendError::Vmm(e.to_string())
}

fn open_kvm() -> Result<OwnedFd, BackendError> {
    let path = c"/dev/kvm";
    let fd = unsafe { libc::open(path.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
    if fd < 0 {
        return Err(BackendError::Unavailable(format!(
            "cannot open /dev/kvm: {} (is your user in the kvm group?)",
            std::io::Error::last_os_error()
        )));
    }
    let kvm = owned(fd);
    let v = ioctl_val(kvm.as_raw_fd(), KVM_GET_API_VERSION, 0, "KVM_GET_API_VERSION")
        .map_err(|e| BackendError::Unavailable(e.to_string()))?;
    if v != 12 {
        return Err(BackendError::Unavailable(format!("KVM API version {v}, need 12")));
    }
    Ok(kvm)
}

impl Backend for KvmBackend {
    fn name(&self) -> &'static str {
        "kvm"
    }

    fn probe(&self, spec: &VmSpec) -> Result<(), BackendError> {
        if spec.arch != Arch::Aarch64 {
            return Err(BackendError::Unavailable(format!(
                "the own VMM runs aarch64 images on this aarch64 host; {} needs the qemu backend (emulated)",
                spec.arch.as_str()
            )));
        }
        if spec.image_kind != ImageKind::UefiApp {
            return Err(BackendError::Unavailable(format!(
                "the own VMM boots the kernel image (uefi-app) directly; {} needs the qemu backend",
                spec.image_kind.as_str()
            )));
        }
        let kvm = open_kvm()?;
        for (cap, name) in [(KVM_CAP_ONE_REG, "ONE_REG"), (KVM_CAP_ARM_PSCI_0_2, "ARM_PSCI_0_2")] {
            let r = ioctl_val(kvm.as_raw_fd(), KVM_CHECK_EXTENSION, cap, "KVM_CHECK_EXTENSION")
                .map_err(|e| BackendError::Unavailable(e.to_string()))?;
            if r <= 0 {
                return Err(BackendError::Unavailable(format!("KVM lacks {name}")));
            }
        }
        Ok(())
    }

    fn run(&mut self, spec: &VmSpec, console: &Console) -> Result<VmExit, BackendError> {
        self.probe(spec)?;
        let kvm = open_kvm()?;
        let vm = owned(
            ioctl_val(kvm.as_raw_fd(), KVM_CREATE_VM, 0, "KVM_CREATE_VM").map_err(vmm_err)?,
        );
        let ram_bytes = (spec.mem_mib as u64) << 20;
        let mut ram = GuestRam::new(ram_bytes as usize).map_err(vmm_err)?;
        let mut region = UserMemoryRegion {
            slot: 0,
            flags: 0,
            guest_phys_addr: RAM_BASE,
            memory_size: ram_bytes,
            userspace_addr: ram.ptr as u64,
        };
        ioctl_ptr(vm.as_raw_fd(), KVM_SET_USER_MEMORY_REGION, &mut region, "KVM_SET_USER_MEMORY_REGION")
            .map_err(vmm_err)?;

        // The kernel image.
        let file = std::fs::read(&spec.image).map_err(vmm_err)?;
        let mem = ram.slice();
        let pe = load_pe(&file, &mut mem[IMAGE_OFF as usize..]).map_err(BackendError::Vmm)?;

        // vCPUs (all created and initialised before the vGIC, per the KVM API document).
        let run_len = ioctl_val(kvm.as_raw_fd(), KVM_GET_VCPU_MMAP_SIZE, 0, "KVM_GET_VCPU_MMAP_SIZE")
            .map_err(vmm_err)? as usize;
        let mut init = VcpuInit::default();
        ioctl_ptr(vm.as_raw_fd(), KVM_ARM_PREFERRED_TARGET, &mut init, "KVM_ARM_PREFERRED_TARGET")
            .map_err(vmm_err)?;
        let mut vcpus = Vec::new();
        let mut mpidrs = Vec::new();
        for i in 0..spec.cpus {
            let fd = owned(
                ioctl_val(vm.as_raw_fd(), KVM_CREATE_VCPU, i as u64, "KVM_CREATE_VCPU")
                    .map_err(vmm_err)?,
            );
            let run = unsafe {
                libc::mmap(
                    std::ptr::null_mut(),
                    run_len,
                    libc::PROT_READ | libc::PROT_WRITE,
                    libc::MAP_SHARED,
                    fd.as_raw_fd(),
                    0,
                )
            };
            if run == libc::MAP_FAILED {
                return Err(vmm_err(std::io::Error::last_os_error()));
            }
            let mut vi = init;
            vi.features[0] |= 1 << KVM_ARM_VCPU_PSCI_0_2;
            if i > 0 {
                vi.features[0] |= 1 << KVM_ARM_VCPU_POWER_OFF;
            }
            ioctl_ptr(fd.as_raw_fd(), KVM_ARM_VCPU_INIT, &mut vi, "KVM_ARM_VCPU_INIT")
                .map_err(vmm_err)?;
            mpidrs.push(get_one_reg(&fd, REG_MPIDR_EL1).map_err(vmm_err)?);
            vcpus.push(Vcpu {
                fd,
                run: run as *mut u8,
                run_len,
            });
        }

        // In-kernel GICv3.
        let mut cd = CreateDevice {
            type_: KVM_DEV_TYPE_ARM_VGIC_V3,
            fd: 0,
            flags: 0,
        };
        ioctl_ptr(vm.as_raw_fd(), KVM_CREATE_DEVICE, &mut cd, "KVM_CREATE_DEVICE(vgic-v3)")
            .map_err(vmm_err)?;
        let gic = owned(cd.fd as libc::c_int);
        let dist = GICD_BASE;
        let redist = GICR_BASE;
        set_device_attr(&gic, KVM_DEV_ARM_VGIC_GRP_ADDR, KVM_VGIC_V3_ADDR_TYPE_DIST, &dist as *const u64 as u64)
            .map_err(vmm_err)?;
        set_device_attr(&gic, KVM_DEV_ARM_VGIC_GRP_ADDR, KVM_VGIC_V3_ADDR_TYPE_REDIST, &redist as *const u64 as u64)
            .map_err(vmm_err)?;
        let nr_irqs: u32 = 128;
        set_device_attr(&gic, KVM_DEV_ARM_VGIC_GRP_NR_IRQS, 0, &nr_irqs as *const u32 as u64)
            .map_err(vmm_err)?;
        set_device_attr(&gic, KVM_DEV_ARM_VGIC_GRP_CTRL, KVM_DEV_ARM_VGIC_CTRL_INIT, 0)
            .map_err(vmm_err)?;

        // Firmware-side tables in RAM.
        let acpi = &mut mem[ACPI_OFF as usize..(ACPI_OFF + ACPI_SIZE) as usize];
        let rsdp = build_acpi(acpi, RAM_BASE + ACPI_OFF, &mpidrs).map_err(BackendError::Vmm)?;
        let map = memory_map(ram_bytes, pe.size);
        let rec = RECORD_OFF as usize;
        let map_off = rec + 0x100;
        let vendor_off = rec + 0x1000;
        mem[map_off..map_off + map.len()].copy_from_slice(&map);
        let vendor = b"aienos-run kvm (own VMM, direct boot)\0";
        mem[vendor_off..vendor_off + vendor.len()].copy_from_slice(vendor);
        let record = direct_record(
            rsdp,
            RAM_BASE + map_off as u64,
            map.len() as u64,
            RAM_BASE + vendor_off as u64,
        );
        mem[rec..rec + record.len()].copy_from_slice(&record);
        let l1 = identity_l1(RAM_BASE + ram_bytes);
        mem[PT_OFF as usize..PT_OFF as usize + 4096].copy_from_slice(&l1);

        // The boot vCPU, as UEFI would leave it, entering the PE entry point.
        let v0 = &vcpus[0].fd;
        let pa_range = get_one_reg(v0, REG_ID_AA64MMFR0_EL1).map_err(vmm_err)? & 0xf;
        for (id, val) in [
            (REG_MAIR_EL1, MAIR),
            (REG_TCR_EL1, tcr(pa_range)),
            (REG_TTBR0_EL1, RAM_BASE + PT_OFF),
            (REG_CPACR_EL1, CPACR),
            (REG_SCTLR_EL1, SCTLR),
            (REG_PSTATE, PSTATE_EL1H_DAIF),
            (REG_SP_EL1, RAM_BASE + STACK_OFF + STACK_SIZE),
            (REG_X0, DIRECT_BOOT_MAGIC),
            (REG_X1, RAM_BASE + RECORD_OFF),
            (REG_PC, RAM_BASE + IMAGE_OFF + pe.entry),
        ] {
            set_one_reg(v0, id, val).map_err(vmm_err)?;
        }

        // Kick signal: interrupts KVM_RUN without SA_RESTART.
        unsafe {
            let mut sa: libc::sigaction = std::mem::zeroed();
            sa.sa_sigaction = on_kick as extern "C" fn(libc::c_int) as usize;
            libc::sigemptyset(&mut sa.sa_mask);
            sa.sa_flags = 0;
            libc::sigaction(libc::SIGUSR1, &sa, std::ptr::null_mut());
        }

        let shared = Shared {
            console,
            uart: Mutex::new(Pl011::default()),
            stop: AtomicBool::new(false),
            end: AtomicU32::new(END_NONE),
            fail: OnceLock::new(),
            unhandled: Mutex::new(Vec::new()),
        };
        let threads: Mutex<Vec<libc::pthread_t>> = Mutex::new(Vec::new());
        std::thread::scope(|s| {
            for v in &vcpus {
                let (shared, threads) = (&shared, &threads);
                s.spawn(move || {
                    threads.lock().unwrap().push(unsafe { libc::pthread_self() });
                    vcpu_loop(v, shared);
                });
            }
            // Supervisor: flushes partial serial lines, turns a stop request into a kick.
            loop {
                std::thread::sleep(Duration::from_millis(1));
                shared.uart.lock().unwrap().flush(console);
                if console.stop_requested() {
                    shared.finish(END_STOP, None);
                }
                if shared.stop.load(Ordering::SeqCst) {
                    for v in &vcpus {
                        v.set_immediate_exit();
                    }
                    for t in threads.lock().unwrap().iter() {
                        unsafe {
                            libc::pthread_kill(*t, libc::SIGUSR1);
                        }
                    }
                    if threads.lock().unwrap().len() == vcpus.len() {
                        break;
                    }
                }
            }
        });
        shared.uart.lock().unwrap().flush(console);
        drop(gic);
        let unhandled = shared.unhandled.lock().unwrap().clone();
        if !unhandled.is_empty() {
            let list: Vec<String> = unhandled
                .iter()
                .map(|(a, w)| format!("0x{a:x}{}", if *w { " (write)" } else { "" }))
                .collect();
            eprintln!("aienos-run: kvm: unemulated MMIO touched: {}", list.join(", "));
        }
        Ok(match shared.end.load(Ordering::SeqCst) {
            END_OFF => VmExit::PowerOff,
            END_RESET => VmExit::Reset,
            END_STOP => VmExit::Stopped,
            _ => VmExit::Failed(shared.fail.get().cloned().unwrap_or_else(|| "unknown".into())),
        })
    }
}

fn vcpu_loop(v: &Vcpu, sh: &Shared) {
    loop {
        if sh.stop.load(Ordering::SeqCst) {
            return;
        }
        let r = unsafe { libc::ioctl(v.fd.as_raw_fd(), KVM_RUN as _, 0) };
        if r < 0 {
            let e = std::io::Error::last_os_error();
            if e.raw_os_error() == Some(libc::EINTR) || e.raw_os_error() == Some(libc::EAGAIN) {
                continue;
            }
            sh.finish(END_FAIL, Some(format!("KVM_RUN: {e}")));
            return;
        }
        match v.rd_u32(RUN_EXIT_REASON) {
            KVM_EXIT_MMIO => {
                let addr = v.rd_u64(RUN_MMIO_PHYS_ADDR);
                let len = (v.rd_u32(RUN_MMIO_LEN) as usize).min(8);
                let is_write = unsafe { *v.run.add(RUN_MMIO_IS_WRITE) } != 0;
                let data = v.mmio_data();
                if (UART_BASE..UART_BASE + UART_SIZE).contains(&addr) {
                    let off = addr - UART_BASE;
                    let mut u = sh.uart.lock().unwrap();
                    if is_write {
                        let mut b = [0u8; 4];
                        b[..len.min(4)].copy_from_slice(&data[..len.min(4)]);
                        if u.write(off, u32::from_le_bytes(b)) {
                            u.flush(sh.console);
                        }
                    } else {
                        let val = u.read(off, sh.console).to_le_bytes();
                        data[..len.min(4)].copy_from_slice(&val[..len.min(4)]);
                    }
                } else {
                    if !is_write {
                        data[..len].fill(0);
                    }
                    let mut list = sh.unhandled.lock().unwrap();
                    if list.len() < 16 && !list.iter().any(|(a, _)| *a == addr) {
                        list.push((addr, is_write));
                    }
                }
            }
            KVM_EXIT_SYSTEM_EVENT => {
                match v.rd_u32(RUN_SYSTEM_EVENT_TYPE) {
                    KVM_SYSTEM_EVENT_SHUTDOWN => sh.finish(END_OFF, None),
                    KVM_SYSTEM_EVENT_RESET => sh.finish(END_RESET, None),
                    t => sh.finish(END_FAIL, Some(format!("guest system event {t}"))),
                }
                return;
            }
            KVM_EXIT_INTR => continue,
            KVM_EXIT_HLT | KVM_EXIT_SHUTDOWN => {
                sh.finish(END_OFF, None);
                return;
            }
            KVM_EXIT_FAIL_ENTRY => {
                sh.finish(END_FAIL, Some(format!("KVM_EXIT_FAIL_ENTRY reason 0x{:x}", v.rd_u64(0x20))));
                return;
            }
            KVM_EXIT_INTERNAL_ERROR => {
                sh.finish(END_FAIL, Some(format!("KVM_EXIT_INTERNAL_ERROR suberror {}", v.rd_u32(0x20))));
                return;
            }
            other => {
                sh.finish(END_FAIL, Some(format!("unexpected KVM exit reason {other}")));
                return;
            }
        }
    }
}
