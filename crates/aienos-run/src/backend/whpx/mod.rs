//! `whpx`: the own VMM on the Windows Hypervisor Platform (candidate B of ledger L261).
//!
//! The machine is [`crate::backend::x86`]: the kernel's BOOTX64.EFI is loaded straight
//! into guest RAM behind a small UEFI shim, with COM1, HPET, I/O APIC and ACPI power
//! registers modelled here and the local APIC emulated by the hypervisor (xAPIC mode).
//! Port and MMIO instructions are decoded by Microsoft's instruction emulator
//! (WinHvEmulation.dll), as QEMU's WHPX accelerator does; the UEFI call port is answered
//! directly. One vCPU, no disk or network devices yet: it boots to the console only.
//!
//! The run loop ([`drive`]) is host-neutral and tested here against scripted exits; the
//! Windows calls live in `sys` and are compiled only for Windows.

pub mod abi;
#[cfg(windows)]
mod sys;

use super::x86::boot::{self, CpuState};
use super::x86::devices::{Event, Platform, Serial};
use super::x86::esp;
use super::x86::mem::GuestMem;
use super::x86::uefi::{CallRegs, Outcome, Uefi};
use super::{Backend, BackendError, VmExit, VmSpec};
use crate::console::Console;
use crate::manifest::{Arch, ImageKind};
use std::time::{Duration, Instant};

/// COM1 as seen from the guest: bytes go to the console in one write per VM exit.
pub struct ConsoleSerial<'a> {
    con: &'a Console,
    out: Vec<u8>,
}

impl<'a> ConsoleSerial<'a> {
    pub fn new(con: &'a Console) -> ConsoleSerial<'a> {
        ConsoleSerial {
            con,
            out: Vec::new(),
        }
    }

    pub fn flush(&mut self) {
        if !self.out.is_empty() {
            self.con.guest_output(&self.out);
            self.out.clear();
        }
    }
}

impl Serial for ConsoleSerial<'_> {
    fn tx(&mut self, byte: u8) {
        self.out.push(byte);
    }
    fn rx_ready(&mut self) -> bool {
        self.con.has_input()
    }
    fn rx(&mut self) -> Option<u8> {
        self.con.pop_input()
    }
}

/// Everything behind the vCPU: RAM, devices, the UEFI shim.
pub struct Machine<'a> {
    pub mem: GuestMem,
    pub platform: Platform,
    pub uefi: Uefi,
    pub serial: ConsoleSerial<'a>,
    /// Set by a device access that ends the run (ACPI S5, reset port).
    pub event: Event,
    t0: Instant,
}

impl<'a> Machine<'a> {
    pub fn new(mem: GuestMem, uefi: Uefi, cpus: u32, con: &'a Console) -> Machine<'a> {
        Machine {
            mem,
            platform: Platform::new(cpus),
            uefi,
            serial: ConsoleSerial::new(con),
            event: Event::Continue,
            t0: Instant::now(),
        }
    }

    /// A port access the emulator decoded. Returns the value read (0 for writes).
    pub fn io(&mut self, port: u16, size: u8, write: bool, data: u32) -> u32 {
        if write {
            let e = self.platform.io_out(port, size, data, &mut self.serial);
            if e != Event::Continue {
                self.event = e;
            }
            0
        } else {
            self.platform.io_in(port, size, &mut self.serial)
        }
    }

    /// A memory access the emulator decoded: device MMIO, or guest RAM when it emulates
    /// a string port instruction (INS/OUTS) that reads or writes memory.
    pub fn memory(&mut self, gpa: u64, size: u8, write: bool, data: &mut [u8; 8]) {
        let n = usize::from(size.min(8));
        let now = self.t0.elapsed().as_nanos() as u64;
        if write {
            if let Some(ram) = self.mem.slice_mut(gpa, n) {
                ram.copy_from_slice(&data[..n]);
            } else {
                self.platform
                    .mmio_write(gpa, size, u64::from_le_bytes(*data), now);
            }
        } else if let Some(ram) = self.mem.slice(gpa, n) {
            data[..n].copy_from_slice(ram);
        } else {
            *data = self.platform.mmio_read(gpa, size, now).to_le_bytes();
        }
    }
}

/// Why `Vcpu::run` came back, after the device side of the exit is done.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Exit {
    /// An access the machine handled; run again (see `Machine::event`).
    Handled,
    /// The guest called a UEFI service through the call port.
    Call(CallRegs),
    /// HLT. `interrupts` is RFLAGS.IF: with it clear nothing can wake the vCPU.
    Halt { interrupts: bool },
    /// `cancel` was called (stop requested).
    Canceled,
    /// The hypervisor reported something the VMM cannot continue from.
    Fatal(String),
}

/// One virtual processor of a hypervisor.
pub trait Vcpu {
    fn run(&mut self, m: &mut Machine) -> Result<Exit, String>;
    /// Completes the pending UEFI call: RAX = `status`, RIP after the `out`.
    fn finish_call(&mut self, status: u64) -> Result<(), String>;
}

/// Runs the vCPU until the guest powers off or resets, the VM fails, or a stop is
/// requested (the caller cancels the vCPU from another thread so this sees it promptly).
pub fn drive(cpu: &mut dyn Vcpu, m: &mut Machine, con: &Console) -> VmExit {
    loop {
        if con.stop_requested() {
            m.serial.flush();
            return VmExit::Stopped;
        }
        let exit = cpu.run(m);
        m.serial.flush();
        match m.event {
            Event::PowerOff => return VmExit::PowerOff,
            Event::Reset => return VmExit::Reset,
            Event::Continue => {}
        }
        match exit {
            Err(e) => return VmExit::Failed(format!("whpx: {e}")),
            Ok(Exit::Handled) | Ok(Exit::Canceled) => {}
            Ok(Exit::Call(r)) => {
                let out = m.uefi.call(&mut m.mem, &r, &mut m.serial);
                m.serial.flush();
                match out {
                    Outcome::Return(status) => {
                        if let Err(e) = cpu.finish_call(status) {
                            return VmExit::Failed(format!("whpx: {e}"));
                        }
                    }
                    Outcome::PowerOff => return VmExit::PowerOff,
                    Outcome::Reset => return VmExit::Reset,
                    Outcome::AppReturned(s) => {
                        return VmExit::Failed(format!(
                            "the kernel returned to the firmware with status 0x{s:x}"
                        ))
                    }
                }
            }
            Ok(Exit::Halt { interrupts: true }) => std::thread::sleep(Duration::from_millis(1)),
            Ok(Exit::Halt { interrupts: false }) => {
                return VmExit::Failed("the guest halted with interrupts off".into())
            }
            Ok(Exit::Fatal(e)) => return VmExit::Failed(format!("whpx: {e}")),
        }
    }
}

/// Guest RAM, the machine and the first register state for `spec`.
pub fn build<'a>(spec: &VmSpec, con: &'a Console) -> Result<(Machine<'a>, CpuState), String> {
    let efi = esp::boot_efi(&spec.image, spec.image_kind)?;
    let mut mem = GuestMem::new(u64::from(spec.mem_mib) << 20)?;
    let b = boot::prepare(&mut mem, &efi, spec.cpus)?;
    Ok((Machine::new(mem, b.uefi, spec.cpus, con), b.cpu))
}

/// What the own VMM can boot, independent of the host.
fn check_spec(spec: &VmSpec) -> Result<(), String> {
    if spec.arch != Arch::X86_64 {
        return Err("the own WHPX VMM boots x86_64 images".into());
    }
    if spec.cpus != 1 {
        return Err(format!(
            "the own WHPX VMM runs 1 vCPU (asked for {})",
            spec.cpus
        ));
    }
    if !matches!(
        spec.image_kind,
        ImageKind::UefiApp | ImageKind::DiskRaw | ImageKind::DiskQcow2
    ) {
        return Err(format!(
            "the own WHPX VMM cannot read a {:?} image",
            spec.image_kind
        ));
    }
    GuestMem::check_size(u64::from(spec.mem_mib) << 20)
}

#[derive(Default)]
pub struct WhpxBackend;

impl Backend for WhpxBackend {
    fn name(&self) -> &'static str {
        "whpx"
    }

    fn probe(&self, spec: &VmSpec) -> Result<(), BackendError> {
        check_spec(spec).map_err(BackendError::Unavailable)?;
        #[cfg(windows)]
        return sys::probe().map_err(BackendError::Unavailable);
        #[cfg(not(windows))]
        Err(BackendError::Unavailable(
            "the Windows Hypervisor Platform exists only on Windows".into(),
        ))
    }

    fn run(&mut self, spec: &VmSpec, console: &Console) -> Result<VmExit, BackendError> {
        check_spec(spec).map_err(BackendError::Unavailable)?;
        if spec.store_disk.is_some() || !spec.forwards.is_empty() {
            eprintln!("whpx: no disk or network devices yet; the Store disk and port forwards are not attached");
        }
        #[cfg(windows)]
        {
            let (mut m, cpu) = build(spec, console).map_err(BackendError::Vmm)?;
            sys::run(&mut m, &cpu, console).map_err(BackendError::Vmm)
        }
        #[cfg(not(windows))]
        {
            let _ = console;
            Err(BackendError::Unavailable(
                "the Windows Hypervisor Platform exists only on Windows".into(),
            ))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::backend::x86::boot::tests::tiny_pe;
    use crate::backend::x86::devices::{COM1, PM1A_CNT, S5_TYP};
    use crate::backend::x86::uefi::{self, BS_FIRST, SUCCESS};
    use std::collections::VecDeque;
    use std::path::PathBuf;

    /// Plays the kernel: a script of what each `run` does to the machine.
    enum Step {
        Call(u32, [u64; 5]),
        Out(u16, u8, u32),
        In(u16, u8, u32),
        Halt(bool),
        Fatal,
    }

    struct Scripted {
        steps: VecDeque<Step>,
        statuses: Vec<u64>,
        pending_call: bool,
    }

    impl Vcpu for Scripted {
        fn run(&mut self, m: &mut Machine) -> Result<Exit, String> {
            assert!(!self.pending_call, "ran again before the call was finished");
            Ok(match self.steps.pop_front().expect("script ran out") {
                Step::Call(id, [rcx, rdx, r8, r9, rsp]) => {
                    self.pending_call = true;
                    Exit::Call(CallRegs {
                        id: u64::from(id),
                        rax: 0,
                        rcx,
                        rdx,
                        r8,
                        r9,
                        rsp,
                    })
                }
                Step::Out(port, size, v) => {
                    m.io(port, size, true, v);
                    Exit::Handled
                }
                Step::In(port, size, want) => {
                    assert_eq!(m.io(port, size, false, 0), want, "in 0x{port:x}");
                    Exit::Handled
                }
                Step::Halt(i) => Exit::Halt { interrupts: i },
                Step::Fatal => Exit::Fatal("unrecoverable exception".into()),
            })
        }

        fn finish_call(&mut self, status: u64) -> Result<(), String> {
            assert!(self.pending_call);
            self.pending_call = false;
            self.statuses.push(status);
            Ok(())
        }
    }

    fn machine(con: &Console) -> Machine<'_> {
        let mut mem = GuestMem::new(64 << 20).unwrap();
        let b = boot::prepare(&mut mem, &tiny_pe(&[0xf4], 0, None), 1).unwrap();
        Machine::new(mem, b.uefi, 1, con)
    }

    fn put(text: &str) -> Vec<Step> {
        text.bytes()
            .map(|c| Step::Out(COM1, 1, u32::from(c)))
            .collect()
    }

    #[test]
    fn boots_to_the_console_and_powers_off() {
        let con = Console::new(None, &["console: ready".into()], Instant::now()).keep_transcript();
        let mut m = machine(&con);
        con.push_input(b"x");
        let (key, size) = (0x1000u64, 0x1008u64);
        let mut s = vec![Step::Call(BS_FIRST + 29, [5000, 0, 0, 0, 0])]; // SetWatchdogTimer
                                                                         // GetMemoryMap(&size, map, &key, &dsize, &ver) with a big enough buffer
        m.mem.write_u64(size, 0x4000).unwrap();
        s.push(Step::Call(
            BS_FIRST + 4,
            [size, 0x2000, key, 0x1010, 0x7fff0],
        ));
        m.mem.write_u64(0x7fff0 + 0x28, 0x1018).unwrap(); // 5th argument on the stack
        s.extend(put("console: ready\r\naienos> "));
        s.push(Step::In(COM1 + 5, 1, 0x61)); // LSR: THR empty and a byte waiting
        s.push(Step::In(COM1, 1, u32::from(b'x')));
        s.push(Step::Halt(true));
        s.push(Step::Out(PM1A_CNT, 2, u32::from(S5_TYP) << 10 | 1 << 13));
        s.push(Step::Fatal); // never reached
        let mut cpu = Scripted {
            steps: s.into(),
            statuses: Vec::new(),
            pending_call: false,
        };
        assert_eq!(drive(&mut cpu, &mut m, &con), VmExit::PowerOff);
        assert_eq!(cpu.statuses, vec![SUCCESS, SUCCESS]);
        assert_eq!(m.mem.read_u64(key), Some(m.uefi.map_key), "map key written");
        assert_eq!(con.transcript(), b"console: ready\r\naienos> ");
        assert!(con.times().ready().is_some(), "ready marker seen");
        assert_eq!(cpu.steps.len(), 1);
    }

    #[test]
    fn stop_reset_halt_and_failures() {
        let con = Console::new(None, &[], Instant::now());
        let run = |steps: Vec<Step>, con: &Console| {
            let mut m = machine(con);
            let mut cpu = Scripted {
                steps: steps.into(),
                statuses: Vec::new(),
                pending_call: false,
            };
            drive(&mut cpu, &mut m, con)
        };
        assert_eq!(run(vec![Step::Out(0xcf9, 1, 6)], &con), VmExit::Reset);
        let off = run(vec![Step::Call(uefi::RT_FIRST + 10, [2, 0, 0, 0, 0])], &con);
        assert_eq!(off, VmExit::PowerOff, "ResetSystem(EfiResetShutdown)");
        let ret = run(vec![Step::Call(uefi::APP_RETURNED, [0; 5])], &con);
        assert!(matches!(ret, VmExit::Failed(s) if s.contains("returned")));
        let halted = run(vec![Step::Halt(false)], &con);
        assert!(matches!(halted, VmExit::Failed(s) if s.contains("interrupts off")));
        assert!(matches!(run(vec![Step::Fatal], &con), VmExit::Failed(_)));
        con.request_stop();
        assert_eq!(run(vec![], &con), VmExit::Stopped);
    }

    fn spec(arch: Arch, cpus: u32, mem_mib: u32, kind: ImageKind) -> VmSpec {
        VmSpec {
            arch,
            image: PathBuf::from("x"),
            image_kind: kind,
            store_disk: None,
            firmware_vars: None,
            mem_mib,
            cpus,
            forwards: Vec::new(),
            scratch: PathBuf::from("."),
        }
    }

    #[test]
    fn probe_says_what_it_cannot_do() {
        let b = WhpxBackend;
        let why = |s: VmSpec| match b.probe(&s) {
            Err(BackendError::Unavailable(w)) => w,
            other => panic!("{other:?}"),
        };
        assert!(why(spec(Arch::Aarch64, 1, 1024, ImageKind::UefiApp)).contains("x86_64"));
        assert!(why(spec(Arch::X86_64, 2, 1024, ImageKind::UefiApp)).contains("1 vCPU"));
        assert!(why(spec(Arch::X86_64, 1, 1024, ImageKind::Iso)).contains("Iso"));
        assert!(why(spec(Arch::X86_64, 1, 16, ImageKind::DiskQcow2)).contains("MiB"));
        #[cfg(not(windows))]
        assert!(why(spec(Arch::X86_64, 1, 1024, ImageKind::DiskQcow2)).contains("only on Windows"));
    }

    /// The stand-in image builds a bootable machine (kernel loaded, relocated, entered
    /// like firmware would) when this machine has one.
    #[test]
    fn standin_image_builds_a_machine_if_present() {
        let Some(home) = std::env::var_os("HOME") else {
            return;
        };
        let img = PathBuf::from(home).join(
            ".hermes/cache/scratch/aienos-run-work/images/aienos-standin-imackern-x86_64.qcow2",
        );
        if !img.exists() {
            eprintln!("skipped: no stand-in image");
            return;
        }
        let con = Console::new(None, &[], Instant::now());
        let mut s = spec(Arch::X86_64, 1, 1024, ImageKind::DiskQcow2);
        s.image = img;
        let t = Instant::now();
        let (m, cpu) = build(&s, &con).unwrap();
        eprintln!(
            "whpx build (image read, 1024 MiB RAM, tables, kernel load): {:?}",
            t.elapsed()
        );
        assert_eq!(
            &m.mem.slice(cpu.rip - 0x1000, 2).unwrap(),
            b"MZ",
            "IMACKERN entry 0x1000"
        );
        assert_eq!(cpu.rcx, uefi::IMAGE_HANDLE);
    }
}
