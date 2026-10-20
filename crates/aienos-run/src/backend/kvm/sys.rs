//! The KVM user API, arm64. Every number below was printed from the host's own headers
//! (/usr/include/linux/kvm.h and asm/kvm.h, Linux 7.0 uapi) by a C program compiled
//! against them on the Spark, 2026-10-20; nothing is typed from memory.

use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};

pub const KVM_GET_API_VERSION: u64 = 0xae00;
pub const KVM_CREATE_VM: u64 = 0xae01;
pub const KVM_CHECK_EXTENSION: u64 = 0xae03;
pub const KVM_GET_VCPU_MMAP_SIZE: u64 = 0xae04;
pub const KVM_CREATE_VCPU: u64 = 0xae41;
pub const KVM_SET_USER_MEMORY_REGION: u64 = 0x4020ae46;
pub const KVM_RUN: u64 = 0xae80;
pub const KVM_GET_ONE_REG: u64 = 0x4010aeab;
pub const KVM_SET_ONE_REG: u64 = 0x4010aeac;
pub const KVM_ARM_VCPU_INIT: u64 = 0x4020aeae;
pub const KVM_ARM_PREFERRED_TARGET: u64 = 0x8020aeaf;
pub const KVM_CREATE_DEVICE: u64 = 0xc00caee0;
pub const KVM_SET_DEVICE_ATTR: u64 = 0x4018aee1;

pub const KVM_CAP_ONE_REG: u64 = 0x46;
pub const KVM_CAP_ARM_PSCI_0_2: u64 = 0x66;

pub const KVM_DEV_TYPE_ARM_VGIC_V3: u32 = 7;
pub const KVM_DEV_ARM_VGIC_GRP_ADDR: u32 = 0;
pub const KVM_VGIC_V3_ADDR_TYPE_DIST: u64 = 2;
pub const KVM_VGIC_V3_ADDR_TYPE_REDIST: u64 = 3;
pub const KVM_DEV_ARM_VGIC_GRP_NR_IRQS: u32 = 3;
pub const KVM_DEV_ARM_VGIC_GRP_CTRL: u32 = 4;
pub const KVM_DEV_ARM_VGIC_CTRL_INIT: u64 = 0;

pub const KVM_ARM_VCPU_POWER_OFF: u32 = 0;
pub const KVM_ARM_VCPU_PSCI_0_2: u32 = 2;

pub const KVM_EXIT_HLT: u32 = 5;
pub const KVM_EXIT_MMIO: u32 = 6;
pub const KVM_EXIT_SHUTDOWN: u32 = 8;
pub const KVM_EXIT_FAIL_ENTRY: u32 = 9;
pub const KVM_EXIT_INTR: u32 = 10;
pub const KVM_EXIT_INTERNAL_ERROR: u32 = 17;
pub const KVM_EXIT_SYSTEM_EVENT: u32 = 24;
pub const KVM_SYSTEM_EVENT_SHUTDOWN: u32 = 1;
pub const KVM_SYSTEM_EVENT_RESET: u32 = 2;

// struct kvm_run offsets.
pub const RUN_IMMEDIATE_EXIT: usize = 0x1;
pub const RUN_EXIT_REASON: usize = 0x8;
pub const RUN_MMIO_PHYS_ADDR: usize = 0x20;
pub const RUN_MMIO_DATA: usize = 0x28;
pub const RUN_MMIO_LEN: usize = 0x30;
pub const RUN_MMIO_IS_WRITE: usize = 0x34;
pub const RUN_SYSTEM_EVENT_TYPE: usize = 0x20;

// Register ids (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | ...).
pub const REG_X0: u64 = 0x6030000000100000;
pub const REG_X1: u64 = 0x6030000000100002;
pub const REG_PC: u64 = 0x6030000000100040;
pub const REG_PSTATE: u64 = 0x6030000000100042;
pub const REG_SP_EL1: u64 = 0x6030000000100044;
pub const REG_SCTLR_EL1: u64 = 0x603000000013c080; // S3_0_C1_C0_0
pub const REG_CPACR_EL1: u64 = 0x603000000013c082; // S3_0_C1_C0_2
pub const REG_TTBR0_EL1: u64 = 0x603000000013c100; // S3_0_C2_C0_0
pub const REG_TCR_EL1: u64 = 0x603000000013c102; // S3_0_C2_C0_2
pub const REG_MAIR_EL1: u64 = 0x603000000013c510; // S3_0_C10_C2_0
pub const REG_MPIDR_EL1: u64 = 0x603000000013c005; // S3_0_C0_C0_5
pub const REG_ID_AA64MMFR0_EL1: u64 = 0x603000000013c038; // S3_0_C0_C7_0

#[repr(C)]
pub struct UserMemoryRegion {
    pub slot: u32,
    pub flags: u32,
    pub guest_phys_addr: u64,
    pub memory_size: u64,
    pub userspace_addr: u64,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct VcpuInit {
    pub target: u32,
    pub features: [u32; 7],
}

#[repr(C)]
pub struct OneReg {
    pub id: u64,
    pub addr: u64,
}

#[repr(C)]
pub struct CreateDevice {
    pub type_: u32,
    pub fd: u32,
    pub flags: u32,
}

#[repr(C)]
pub struct DeviceAttr {
    pub flags: u32,
    pub group: u32,
    pub attr: u64,
    pub addr: u64,
}

fn check(r: libc::c_int, what: &str) -> io::Result<libc::c_int> {
    if r < 0 {
        let e = io::Error::last_os_error();
        return Err(io::Error::new(e.kind(), format!("{what}: {e}")));
    }
    Ok(r)
}

/// ioctl with an integer argument.
pub fn ioctl_val(fd: RawFd, req: u64, arg: u64, what: &str) -> io::Result<libc::c_int> {
    check(
        unsafe { libc::ioctl(fd, req as _, arg as libc::c_ulong) },
        what,
    )
}

/// ioctl with a pointer argument.
pub fn ioctl_ptr<T>(fd: RawFd, req: u64, arg: *mut T, what: &str) -> io::Result<libc::c_int> {
    check(unsafe { libc::ioctl(fd, req as _, arg) }, what)
}

pub fn owned(fd: libc::c_int) -> OwnedFd {
    unsafe { OwnedFd::from_raw_fd(fd) }
}

pub fn set_one_reg(vcpu: &OwnedFd, id: u64, value: u64) -> io::Result<()> {
    let mut v = value;
    let mut r = OneReg {
        id,
        addr: &mut v as *mut u64 as u64,
    };
    ioctl_ptr(vcpu.as_raw_fd(), KVM_SET_ONE_REG, &mut r, "KVM_SET_ONE_REG").map(|_| ())
}

pub fn get_one_reg(vcpu: &OwnedFd, id: u64) -> io::Result<u64> {
    let mut v = 0u64;
    let mut r = OneReg {
        id,
        addr: &mut v as *mut u64 as u64,
    };
    ioctl_ptr(vcpu.as_raw_fd(), KVM_GET_ONE_REG, &mut r, "KVM_GET_ONE_REG")?;
    Ok(v)
}

pub fn set_device_attr(dev: &OwnedFd, group: u32, attr: u64, addr: u64) -> io::Result<()> {
    let mut a = DeviceAttr {
        flags: 0,
        group,
        attr,
        addr,
    };
    ioctl_ptr(
        dev.as_raw_fd(),
        KVM_SET_DEVICE_ATTR,
        &mut a,
        "KVM_SET_DEVICE_ATTR",
    )
    .map(|_| ())
}
