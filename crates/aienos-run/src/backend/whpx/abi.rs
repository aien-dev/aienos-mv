//! The Windows Hypervisor Platform ABI subset the own VMM uses, as plain data so the
//! layouts are checked by tests on every host. Values and layouts from the Windows SDK
//! headers WinHvPlatformDefs.h and WinHvEmulation.h (cross-checked against mingw-w64).

#![allow(non_upper_case_globals, clippy::upper_case_acronyms)]

use crate::backend::x86::boot::{Dtr, Seg};

pub type HRESULT = i32;

// WHV_CAPABILITY_CODE
pub const CapHypervisorPresent: u32 = 0x0000_0000;
pub const CapFeatures: u32 = 0x0000_0001;
/// WHV_CAPABILITY_FEATURES bit 1.
pub const FEATURE_LOCAL_APIC_EMULATION: u64 = 1 << 1;

// WHV_PARTITION_PROPERTY_CODE
pub const PropProcessorCount: u32 = 0x0000_1fff;
pub const PropLocalApicEmulationMode: u32 = 0x0000_1005;
/// WHV_X64_LOCAL_APIC_EMULATION_MODE
pub const LocalApicEmulationXApic: u32 = 1;

// WHV_MAP_GPA_RANGE_FLAGS
pub const MapRead: u32 = 1;
pub const MapWrite: u32 = 2;
pub const MapExecute: u32 = 4;

// WHV_TRANSLATE_GVA_FLAGS
pub const TranslateValidateRead: u32 = 1;
pub const TranslateValidateWrite: u32 = 2;
pub const TranslatePrivilegeExempt: u32 = 8;

// WHV_REGISTER_NAME
pub const Rax: u32 = 0x00;
pub const Rcx: u32 = 0x01;
pub const Rdx: u32 = 0x02;
pub const Rsp: u32 = 0x04;
pub const R8: u32 = 0x08;
pub const R9: u32 = 0x09;
pub const R10: u32 = 0x0a;
pub const Rip: u32 = 0x10;
pub const Rflags: u32 = 0x11;
pub const Es: u32 = 0x12;
pub const Cs: u32 = 0x13;
pub const Ss: u32 = 0x14;
pub const Ds: u32 = 0x15;
pub const Fs: u32 = 0x16;
pub const Gs: u32 = 0x17;
pub const Ldtr: u32 = 0x18;
pub const Tr: u32 = 0x19;
pub const Idtr: u32 = 0x1a;
pub const Gdtr: u32 = 0x1b;
pub const Cr0: u32 = 0x1c;
pub const Cr3: u32 = 0x1e;
pub const Cr4: u32 = 0x1f;
pub const Efer: u32 = 0x2001;

// WHV_RUN_VP_EXIT_REASON
pub const ExitMemoryAccess: u32 = 0x0000_0001;
pub const ExitIoPortAccess: u32 = 0x0000_0002;
pub const ExitUnrecoverableException: u32 = 0x0000_0004;
pub const ExitInvalidVpRegisterValue: u32 = 0x0000_0005;
pub const ExitUnsupportedFeature: u32 = 0x0000_0006;
pub const ExitInterruptWindow: u32 = 0x0000_0007;
pub const ExitHalt: u32 = 0x0000_0008;
pub const ExitApicEoi: u32 = 0x0000_0009;
pub const ExitMsrAccess: u32 = 0x0000_1000;
pub const ExitCpuid: u32 = 0x0000_1001;
pub const ExitException: u32 = 0x0000_1002;
pub const ExitCanceled: u32 = 0x0000_2001;

/// WHV_EMULATOR_STATUS bit 0.
pub const EMULATION_SUCCESSFUL: u32 = 1;

/// WHV_REGISTER_VALUE: a 16-byte union.
#[repr(C, align(16))]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RegValue(pub [u64; 2]);

impl RegValue {
    pub fn u64(v: u64) -> RegValue {
        RegValue([v, 0])
    }

    /// WHV_X64_SEGMENT_REGISTER: Base u64, Limit u32, Selector u16, Attributes u16.
    pub fn seg(s: &Seg) -> RegValue {
        RegValue([
            s.base,
            u64::from(s.limit) | u64::from(s.selector) << 32 | u64::from(s.attr) << 48,
        ])
    }

    /// WHV_X64_TABLE_REGISTER: Pad[3] u16, Limit u16, Base u64.
    pub fn table(t: &Dtr) -> RegValue {
        RegValue([u64::from(t.limit) << 48, t.base])
    }
}

/// WHV_VP_EXIT_CONTEXT.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct VpExitContext {
    pub execution_state: u16,
    /// InstructionLength in bits 0..3, Cr8 in bits 4..7.
    pub instruction_length_cr8: u8,
    pub reserved: u8,
    pub reserved2: u32,
    pub cs: [u64; 2],
    pub rip: u64,
    pub rflags: u64,
}

impl VpExitContext {
    pub fn instruction_length(&self) -> u64 {
        u64::from(self.instruction_length_cr8 & 0xf)
    }
}

/// WHV_RUN_VP_EXIT_CONTEXT: reason, VP context, then a union read with the decoders below.
#[repr(C, align(16))]
#[derive(Clone, Copy)]
pub struct RunExitContext {
    pub exit_reason: u32,
    pub reserved: u32,
    pub vp: VpExitContext,
    pub u: [u8; 176],
}

impl Default for RunExitContext {
    fn default() -> RunExitContext {
        RunExitContext {
            exit_reason: 0,
            reserved: 0,
            vp: VpExitContext::default(),
            u: [0; 176],
        }
    }
}

fn le16(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn le32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().expect("4"))
}
fn le64(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().expect("8"))
}

/// The fields of WHV_X64_IO_PORT_ACCESS_CONTEXT the VMM looks at.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct IoExit {
    pub port: u16,
    pub write: bool,
    pub size: u8,
    pub string: bool,
    pub rax: u64,
}

impl RunExitContext {
    pub fn io(&self) -> IoExit {
        let a = le32(&self.u, 20);
        IoExit {
            port: le16(&self.u, 24),
            write: a & 1 != 0,
            size: ((a >> 1) & 7) as u8,
            string: a & 1 << 4 != 0,
            rax: le64(&self.u, 32),
        }
    }

    /// (guest physical address, access type 0 read / 1 write / 2 execute) of a
    /// WHV_MEMORY_ACCESS_CONTEXT.
    pub fn memory(&self) -> (u64, u32) {
        (le64(&self.u, 24), le32(&self.u, 20) & 3)
    }

    /// Pointer to the union as the IO or memory context the emulator takes.
    pub fn union_ptr(&self) -> *const u8 {
        self.u.as_ptr()
    }
}

/// WHV_EMULATOR_IO_ACCESS_INFO.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct EmuIoAccess {
    /// 0 in, 1 out.
    pub direction: u8,
    pub port: u16,
    pub access_size: u16,
    pub data: u32,
}

/// WHV_EMULATOR_MEMORY_ACCESS_INFO.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct EmuMemAccess {
    pub gpa: u64,
    /// 0 read, 1 write.
    pub direction: u8,
    pub access_size: u8,
    pub data: [u8; 8],
}

/// WHV_TRANSLATE_GVA_RESULT.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct TranslateResult {
    pub code: u32,
    pub reserved: u32,
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn layouts_match_the_sdk() {
        assert_eq!((size_of::<RegValue>(), align_of::<RegValue>()), (16, 16));
        assert_eq!(size_of::<VpExitContext>(), 40);
        assert_eq!(offset_of!(VpExitContext, cs), 8);
        assert_eq!(offset_of!(VpExitContext, rip), 24);
        assert_eq!(offset_of!(VpExitContext, rflags), 32);
        assert_eq!(size_of::<RunExitContext>(), 224);
        assert_eq!(offset_of!(RunExitContext, vp), 8);
        assert_eq!(offset_of!(RunExitContext, u), 48);
        assert_eq!(size_of::<EmuIoAccess>(), 12);
        assert_eq!(offset_of!(EmuIoAccess, port), 2);
        assert_eq!(offset_of!(EmuIoAccess, access_size), 4);
        assert_eq!(offset_of!(EmuIoAccess, data), 8);
        assert_eq!(size_of::<EmuMemAccess>(), 24);
        assert_eq!(offset_of!(EmuMemAccess, direction), 8);
        assert_eq!(offset_of!(EmuMemAccess, data), 10);
        assert_eq!(size_of::<TranslateResult>(), 8);
    }

    #[test]
    fn register_encodings() {
        let s = Seg {
            base: 0x1234,
            limit: 0x67,
            selector: 0x18,
            attr: 0x8b,
        };
        let v = RegValue::seg(&s);
        let b: Vec<u8> = v.0.iter().flat_map(|x| x.to_le_bytes()).collect();
        assert_eq!(le64(&b, 0), 0x1234);
        assert_eq!(le32(&b, 8), 0x67);
        assert_eq!(le16(&b, 12), 0x18);
        assert_eq!(le16(&b, 14), 0x8b);
        let t = RegValue::table(&Dtr {
            base: 0x10_0000,
            limit: 39,
        });
        let b: Vec<u8> = t.0.iter().flat_map(|x| x.to_le_bytes()).collect();
        assert_eq!(le16(&b, 6), 39);
        assert_eq!(le64(&b, 8), 0x10_0000);
    }

    #[test]
    fn exit_decoding() {
        let mut c = RunExitContext {
            exit_reason: ExitIoPortAccess,
            ..Default::default()
        };
        c.vp.instruction_length_cr8 = 0x52; // length 2, CR8 5
        c.u[20..24].copy_from_slice(&(1u32 | 1 << 1).to_le_bytes()); // out, 1 byte
        c.u[24..26].copy_from_slice(&0x3f8u16.to_le_bytes());
        c.u[32..40].copy_from_slice(&0x41u64.to_le_bytes());
        assert_eq!(c.vp.instruction_length(), 2);
        assert_eq!(
            c.io(),
            IoExit {
                port: 0x3f8,
                write: true,
                size: 1,
                string: false,
                rax: 0x41
            }
        );
        c.u[20..24].copy_from_slice(&1u32.to_le_bytes());
        c.u[24..32].copy_from_slice(&0xfed0_00f0u64.to_le_bytes());
        assert_eq!(c.memory(), (0xfed0_00f0, 1));
    }
}
