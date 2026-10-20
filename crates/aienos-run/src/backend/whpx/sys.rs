//! The Windows side: WinHvPlatform.dll and WinHvEmulation.dll, loaded at run time from
//! System32 (so the exe starts, and says what to enable, on a PC without the feature).
//! Function signatures from WinHvPlatform.h and WinHvEmulation.h.

#![allow(non_snake_case, non_upper_case_globals)]

use super::abi::*;
use super::{drive, Exit, Machine, Vcpu};
use crate::backend::x86::boot::CpuState;
use crate::backend::x86::uefi::{CallRegs, CALL_PORT};
use crate::backend::VmExit;
use crate::console::Console;
use crate::win;
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

type Handle = *mut c_void;

type GetCapability = unsafe extern "system" fn(u32, *mut c_void, u32, *mut u32) -> HRESULT;
type CreatePartition = unsafe extern "system" fn(*mut Handle) -> HRESULT;
type PartitionOp = unsafe extern "system" fn(Handle) -> HRESULT;
type SetPartitionProperty = unsafe extern "system" fn(Handle, u32, *const c_void, u32) -> HRESULT;
type MapGpaRange = unsafe extern "system" fn(Handle, *mut c_void, u64, u64, u32) -> HRESULT;
type CreateVp = unsafe extern "system" fn(Handle, u32, u32) -> HRESULT;
type DeleteVp = unsafe extern "system" fn(Handle, u32) -> HRESULT;
type RunVp = unsafe extern "system" fn(Handle, u32, *mut c_void, u32) -> HRESULT;
type CancelRunVp = unsafe extern "system" fn(Handle, u32, u32) -> HRESULT;
type GetVpRegisters =
    unsafe extern "system" fn(Handle, u32, *const u32, u32, *mut RegValue) -> HRESULT;
type SetVpRegisters =
    unsafe extern "system" fn(Handle, u32, *const u32, u32, *const RegValue) -> HRESULT;
type TranslateGva =
    unsafe extern "system" fn(Handle, u32, u64, u32, *mut TranslateResult, *mut u64) -> HRESULT;

type IoCallback = unsafe extern "system" fn(*mut c_void, *mut EmuIoAccess) -> HRESULT;
type MemCallback = unsafe extern "system" fn(*mut c_void, *mut EmuMemAccess) -> HRESULT;
type GetRegsCallback =
    unsafe extern "system" fn(*mut c_void, *const u32, u32, *mut RegValue) -> HRESULT;
type SetRegsCallback =
    unsafe extern "system" fn(*mut c_void, *const u32, u32, *const RegValue) -> HRESULT;
type TranslateCallback =
    unsafe extern "system" fn(*mut c_void, u64, u32, *mut u32, *mut u64) -> HRESULT;

/// WHV_EMULATOR_CALLBACKS.
#[repr(C)]
struct EmuCallbacks {
    size: u32,
    reserved: u32,
    io: IoCallback,
    memory: MemCallback,
    get_regs: GetRegsCallback,
    set_regs: SetRegsCallback,
    translate: TranslateCallback,
}

/// Static, as the emulator may keep the pointer (QEMU passes a static table too).
static CALLBACKS: EmuCallbacks = EmuCallbacks {
    size: std::mem::size_of::<EmuCallbacks>() as u32,
    reserved: 0,
    io: emu_io,
    memory: emu_memory,
    get_regs: emu_get_regs,
    set_regs: emu_set_regs,
    translate: emu_translate,
};

type EmuCreate = unsafe extern "system" fn(*const EmuCallbacks, *mut Handle) -> HRESULT;
type EmuDestroy = unsafe extern "system" fn(Handle) -> HRESULT;
type EmuTry = unsafe extern "system" fn(
    Handle,
    *mut c_void,
    *const VpExitContext,
    *const u8,
    *mut u32,
) -> HRESULT;

struct Api {
    get_capability: GetCapability,
    create_partition: CreatePartition,
    setup_partition: PartitionOp,
    delete_partition: PartitionOp,
    set_partition_property: SetPartitionProperty,
    map_gpa_range: MapGpaRange,
    create_vp: CreateVp,
    delete_vp: DeleteVp,
    run_vp: RunVp,
    cancel_run_vp: CancelRunVp,
    get_regs: GetVpRegisters,
    set_regs: SetVpRegisters,
    translate_gva: TranslateGva,
    emu_create: EmuCreate,
    emu_destroy: EmuDestroy,
    emu_try_io: EmuTry,
    emu_try_mmio: EmuTry,
}

fn load_dll(name: &str) -> Result<Handle, String> {
    let w = win::wide(name);
    // SAFETY: a NUL-terminated name; System32 only.
    let h = unsafe {
        win::LoadLibraryExW(
            w.as_ptr(),
            std::ptr::null_mut(),
            win::LOAD_LIBRARY_SEARCH_SYSTEM32,
        )
    };
    if h.is_null() {
        Err(format!(
            "{name} is missing: turn on the Windows feature \"Windows Hypervisor Platform\" and restart"
        ))
    } else {
        Ok(h)
    }
}

fn sym(dll: Handle, name: &str) -> Result<*mut c_void, String> {
    let c: Vec<u8> = name.bytes().chain(std::iter::once(0)).collect();
    // SAFETY: a loaded module and a NUL-terminated name.
    let p = unsafe { win::GetProcAddress(dll, c.as_ptr()) };
    if p.is_null() {
        Err(format!("{name} not found (Windows too old for this VMM?)"))
    } else {
        Ok(p)
    }
}

impl Api {
    fn load() -> Result<Api, String> {
        let hv = load_dll("WinHvPlatform.dll")?;
        let em = load_dll("WinHvEmulation.dll")?;
        macro_rules! f {
            ($dll:expr, $name:literal, $t:ty) => {
                // SAFETY: the symbol has the signature $t (the Windows SDK headers named
                // above).
                unsafe { std::mem::transmute::<*mut c_void, $t>(sym($dll, $name)?) }
            };
        }
        Ok(Api {
            get_capability: f!(hv, "WHvGetCapability", GetCapability),
            create_partition: f!(hv, "WHvCreatePartition", CreatePartition),
            setup_partition: f!(hv, "WHvSetupPartition", PartitionOp),
            delete_partition: f!(hv, "WHvDeletePartition", PartitionOp),
            set_partition_property: f!(hv, "WHvSetPartitionProperty", SetPartitionProperty),
            map_gpa_range: f!(hv, "WHvMapGpaRange", MapGpaRange),
            create_vp: f!(hv, "WHvCreateVirtualProcessor", CreateVp),
            delete_vp: f!(hv, "WHvDeleteVirtualProcessor", DeleteVp),
            run_vp: f!(hv, "WHvRunVirtualProcessor", RunVp),
            cancel_run_vp: f!(hv, "WHvCancelRunVirtualProcessor", CancelRunVp),
            get_regs: f!(hv, "WHvGetVirtualProcessorRegisters", GetVpRegisters),
            set_regs: f!(hv, "WHvSetVirtualProcessorRegisters", SetVpRegisters),
            translate_gva: f!(hv, "WHvTranslateGva", TranslateGva),
            emu_create: f!(em, "WHvEmulatorCreateEmulator", EmuCreate),
            emu_destroy: f!(em, "WHvEmulatorDestroyEmulator", EmuDestroy),
            emu_try_io: f!(em, "WHvEmulatorTryIoEmulation", EmuTry),
            emu_try_mmio: f!(em, "WHvEmulatorTryMmioEmulation", EmuTry),
        })
    }
}

fn check(what: &str, hr: HRESULT) -> Result<(), String> {
    if hr < 0 {
        Err(format!("{what} failed: HRESULT 0x{:08x}", hr as u32))
    } else {
        Ok(())
    }
}

fn capability(api: &Api, code: u32) -> Result<u64, String> {
    let (mut v, mut n) = (0u64, 0u32);
    // SAFETY: an 8-byte buffer for a BOOL or UINT64 capability.
    check("WHvGetCapability", unsafe {
        (api.get_capability)(code, (&mut v as *mut u64).cast(), 8, &mut n)
    })?;
    Ok(v)
}

/// Ok when this PC can run the own VMM: the DLLs load, the hypervisor runs, and it
/// emulates the local APIC the kernel's timer needs.
pub fn probe() -> Result<(), String> {
    probe_api(&Api::load()?)
}

fn probe_api(api: &Api) -> Result<(), String> {
    if capability(api, CapHypervisorPresent)? & 1 == 0 {
        return Err("the Windows hypervisor is not running: turn on \"Windows Hypervisor Platform\", restart, and check virtualization is on in the firmware setup".into());
    }
    if capability(api, CapFeatures)? & FEATURE_LOCAL_APIC_EMULATION == 0 {
        return Err("this Windows hypervisor does not emulate the local APIC".into());
    }
    Ok(())
}

/// A partition with one vCPU; deleted on drop.
struct Partition {
    api: Api,
    h: Handle,
    vp: bool,
    emulator: Handle,
}

impl Drop for Partition {
    fn drop(&mut self) {
        // SAFETY: handles created by this value, released once.
        unsafe {
            if !self.emulator.is_null() {
                (self.api.emu_destroy)(self.emulator);
            }
            if self.vp {
                (self.api.delete_vp)(self.h, 0);
            }
            (self.api.delete_partition)(self.h);
        }
    }
}

impl Partition {
    fn create(m: &Machine) -> Result<Partition, String> {
        let api = Api::load()?;
        probe_api(&api)?;
        let mut h: Handle = std::ptr::null_mut();
        // SAFETY: out pointer to a handle.
        check("WHvCreatePartition", unsafe {
            (api.create_partition)(&mut h)
        })?;
        let mut p = Partition {
            api,
            h,
            vp: false,
            emulator: std::ptr::null_mut(),
        };
        let set = |p: &Partition, code: u32, v: u32, what: &str| {
            // SAFETY: a 4-byte property value.
            check(what, unsafe {
                (p.api.set_partition_property)(p.h, code, (&v as *const u32).cast(), 4)
            })
        };
        set(&p, PropProcessorCount, 1, "processor count")?;
        set(
            &p,
            PropLocalApicEmulationMode,
            LocalApicEmulationXApic,
            "xAPIC emulation",
        )?;
        // SAFETY: a configured partition handle.
        check("WHvSetupPartition", unsafe { (p.api.setup_partition)(p.h) })?;
        for (gpa, host, len) in m.mem.regions() {
            // SAFETY: page-aligned host memory owned by `m.mem`, which outlives the
            // partition (both live in `run`, the partition is dropped first).
            check("WHvMapGpaRange", unsafe {
                (p.api.map_gpa_range)(p.h, host.cast(), gpa, len, MapRead | MapWrite | MapExecute)
            })?;
        }
        // SAFETY: a set-up partition.
        check("WHvCreateVirtualProcessor", unsafe {
            (p.api.create_vp)(p.h, 0, 0)
        })?;
        p.vp = true;
        // SAFETY: a static callback table; out pointer to a handle.
        check("WHvEmulatorCreateEmulator", unsafe {
            (p.api.emu_create)(&CALLBACKS, &mut p.emulator)
        })?;
        Ok(p)
    }

    fn set_regs(&self, names: &[u32], values: &[RegValue]) -> Result<(), String> {
        // SAFETY: equal-length arrays.
        check("WHvSetVirtualProcessorRegisters", unsafe {
            (self.api.set_regs)(
                self.h,
                0,
                names.as_ptr(),
                names.len() as u32,
                values.as_ptr(),
            )
        })
    }

    fn get_regs(&self, names: &[u32]) -> Result<Vec<RegValue>, String> {
        let mut v = vec![RegValue::default(); names.len()];
        // SAFETY: equal-length arrays.
        check("WHvGetVirtualProcessorRegisters", unsafe {
            (self.api.get_regs)(
                self.h,
                0,
                names.as_ptr(),
                names.len() as u32,
                v.as_mut_ptr(),
            )
        })?;
        Ok(v)
    }

    fn set_state(&self, c: &CpuState) -> Result<(), String> {
        let names = [
            Rip, Rsp, Rcx, Rdx, Rflags, Cr0, Cr3, Cr4, Efer, Cs, Ds, Es, Fs, Gs, Ss, Tr, Ldtr,
            Gdtr, Idtr,
        ];
        let u = RegValue::u64;
        let values = [
            u(c.rip),
            u(c.rsp),
            u(c.rcx),
            u(c.rdx),
            u(c.rflags),
            u(c.cr0),
            u(c.cr3),
            u(c.cr4),
            u(c.efer),
            RegValue::seg(&c.cs),
            RegValue::seg(&c.ds),
            RegValue::seg(&c.es),
            RegValue::seg(&c.fs),
            RegValue::seg(&c.gs),
            RegValue::seg(&c.ss),
            RegValue::seg(&c.tr),
            RegValue::seg(&c.ldtr),
            RegValue::table(&c.gdtr),
            RegValue::table(&c.idtr),
        ];
        self.set_regs(&names, &values)
    }
}

/// What the emulator callbacks reach through their context pointer.
struct EmuCtx<'p, 'm, 'c> {
    p: &'p Partition,
    m: &'m mut Machine<'c>,
}

unsafe extern "system" fn emu_io(ctx: *mut c_void, a: *mut EmuIoAccess) -> HRESULT {
    // SAFETY: the context is the EmuCtx passed to WHvEmulatorTry*Emulation, alive for the
    // call; `a` is the emulator's access record.
    let (c, a) = unsafe { (&mut *(ctx as *mut EmuCtx), &mut *a) };
    let v =
        c.m.io(a.port, a.access_size as u8, a.direction == 1, a.data);
    if a.direction == 0 {
        a.data = v;
    }
    0
}

unsafe extern "system" fn emu_memory(ctx: *mut c_void, a: *mut EmuMemAccess) -> HRESULT {
    // SAFETY: as in emu_io.
    let (c, a) = unsafe { (&mut *(ctx as *mut EmuCtx), &mut *a) };
    c.m.memory(a.gpa, a.access_size, a.direction == 1, &mut a.data);
    0
}

unsafe extern "system" fn emu_get_regs(
    ctx: *mut c_void,
    names: *const u32,
    n: u32,
    out: *mut RegValue,
) -> HRESULT {
    // SAFETY: as in emu_io; the arrays have `n` entries.
    unsafe {
        let c = &*(ctx as *const EmuCtx);
        (c.p.api.get_regs)(c.p.h, 0, names, n, out)
    }
}

unsafe extern "system" fn emu_set_regs(
    ctx: *mut c_void,
    names: *const u32,
    n: u32,
    values: *const RegValue,
) -> HRESULT {
    // SAFETY: as in emu_get_regs.
    unsafe {
        let c = &*(ctx as *const EmuCtx);
        (c.p.api.set_regs)(c.p.h, 0, names, n, values)
    }
}

unsafe extern "system" fn emu_translate(
    ctx: *mut c_void,
    gva: u64,
    flags: u32,
    result: *mut u32,
    gpa: *mut u64,
) -> HRESULT {
    // SAFETY: as in emu_io; `result` and `gpa` are the emulator's out parameters.
    unsafe {
        let c = &*(ctx as *const EmuCtx);
        let mut r = TranslateResult::default();
        let hr = (c.p.api.translate_gva)(c.p.h, 0, gva, flags, &mut r, gpa);
        *result = r.code;
        hr
    }
}

struct WhpxVcpu<'p> {
    p: &'p Partition,
    /// RIP after the `out` of the pending UEFI call.
    resume: Option<u64>,
}

impl Vcpu for WhpxVcpu<'_> {
    fn run(&mut self, m: &mut Machine) -> Result<Exit, String> {
        let mut x = RunExitContext::default();
        // SAFETY: a 224-byte exit context.
        check("WHvRunVirtualProcessor", unsafe {
            (self.p.api.run_vp)(
                self.p.h,
                0,
                (&mut x as *mut RunExitContext).cast(),
                std::mem::size_of::<RunExitContext>() as u32,
            )
        })?;
        let at = |what: &str| format!("{what} at rip 0x{:x}", x.vp.rip);
        match x.exit_reason {
            ExitIoPortAccess if x.io().port == CALL_PORT && x.io().write && !x.io().string => {
                let r = self.p.get_regs(&[R10, Rax, Rcx, Rdx, R8, R9, Rsp])?;
                self.resume = Some(x.vp.rip + x.vp.instruction_length());
                Ok(Exit::Call(CallRegs {
                    id: r[0].0[0] & 0xffff_ffff,
                    rax: r[1].0[0],
                    rcx: r[2].0[0],
                    rdx: r[3].0[0],
                    r8: r[4].0[0],
                    r9: r[5].0[0],
                    rsp: r[6].0[0],
                }))
            }
            ExitIoPortAccess | ExitMemoryAccess => {
                let mut ctx = EmuCtx { p: self.p, m };
                let mut status = 0u32;
                let try_ = if x.exit_reason == ExitIoPortAccess {
                    self.p.api.emu_try_io
                } else {
                    self.p.api.emu_try_mmio
                };
                // SAFETY: the emulator calls back into `ctx` only during this call.
                check("instruction emulation", unsafe {
                    try_(
                        self.p.emulator,
                        (&mut ctx as *mut EmuCtx).cast(),
                        &x.vp,
                        x.union_ptr(),
                        &mut status,
                    )
                })?;
                if status & EMULATION_SUCCESSFUL == 0 {
                    let what = if x.exit_reason == ExitIoPortAccess {
                        format!("port 0x{:x} access", x.io().port)
                    } else {
                        format!("memory access to 0x{:x}", x.memory().0)
                    };
                    return Ok(Exit::Fatal(at(&format!(
                        "the emulator could not complete a {what} (status 0x{status:x})"
                    ))));
                }
                Ok(Exit::Handled)
            }
            ExitHalt => Ok(Exit::Halt {
                interrupts: x.vp.rflags & 1 << 9 != 0,
            }),
            ExitCanceled => Ok(Exit::Canceled),
            // Not requested from the hypervisor; harmless if it reports them anyway.
            ExitInterruptWindow | ExitApicEoi => Ok(Exit::Handled),
            ExitUnrecoverableException => Ok(Exit::Fatal(at("triple fault"))),
            ExitInvalidVpRegisterValue => Ok(Exit::Fatal(at("invalid register state"))),
            ExitUnsupportedFeature => Ok(Exit::Fatal(at("unsupported feature"))),
            ExitMsrAccess | ExitCpuid | ExitException => Ok(Exit::Fatal(at(&format!(
                "unexpected exit 0x{:x}",
                x.exit_reason
            )))),
            r => Ok(Exit::Fatal(at(&format!("unknown exit 0x{r:x}")))),
        }
    }

    fn finish_call(&mut self, status: u64) -> Result<(), String> {
        let rip = self.resume.take().ok_or("no UEFI call pending")?;
        self.p
            .set_regs(&[Rax, Rip], &[RegValue::u64(status), RegValue::u64(rip)])
    }
}

/// The partition handle for the stop watcher.
struct Canceller(Handle, CancelRunVp);
// SAFETY: WHvCancelRunVirtualProcessor is meant to be called from another thread.
unsafe impl Send for Canceller {}

pub fn run(m: &mut Machine, cpu: &CpuState, con: &Console) -> Result<VmExit, String> {
    let p = Partition::create(m)?;
    p.set_state(cpu)?;
    let done = Arc::new(AtomicBool::new(false));
    let c = Canceller(p.h, p.api.cancel_run_vp);
    let watcher = std::thread::scope(|s| {
        let d = done.clone();
        s.spawn(move || {
            let c = c;
            while !d.load(Ordering::SeqCst) {
                if con.stop_requested() {
                    // SAFETY: the partition outlives this scope.
                    unsafe { (c.1)(c.0, 0, 0) };
                }
                std::thread::sleep(Duration::from_millis(20));
            }
        });
        con.vmm_started();
        let mut vcpu = WhpxVcpu {
            p: &p,
            resume: None,
        };
        let exit = drive(&mut vcpu, m, con);
        done.store(true, Ordering::SeqCst);
        exit
    });
    Ok(watcher)
}
