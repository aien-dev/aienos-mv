//! The device model of the own x86 VMM: what the guest reaches through I/O ports and
//! MMIO exits. Everything is polled; no device raises an interrupt (the kernel polls COM1
//! and masks every I/O APIC line, native/kernel/x86/kmain_x86.c ioapic_mask_all; its
//! timer is the local APIC's, which the hypervisor emulates).
//!
//! - COM1 16550 at 0x3f8: transmit to the console, receive from the keyboard, scratch
//!   register (the kernel's probe), divisor latch, LSR/IIR/MSR.
//! - HPET at 0xfed00000: 100 MHz main counter from the host clock (the kernel calibrates
//!   the TSC against it); comparators are stored, not fired.
//! - I/O APIC at 0xfec00000: the register window (id, version, 24 redirection entries).
//! - ACPI PM1a event/control at 0x600/0x604: SCI_EN reads as set; SLP_EN with the \_S5
//!   type powers off.
//! - Reset: 0xcf9 bit 2 and the 8042 command 0xfe on port 0x64.
//! - PCI configuration ports 0xcf8/0xcfc: no devices (all ones). The kernel enumerates
//!   PCI through ECAM from the ACPI MCFG table, which this machine does not offer yet.
//!
//! Anything else reads as all ones and ignores writes; the first few such addresses are
//! kept in [`Platform::unknown`] for the run's diagnostics.

use std::collections::BTreeSet;

pub const COM1: u16 = 0x3f8;
pub const PM1A_EVT: u16 = 0x600;
pub const PM1A_CNT: u16 = 0x604;
pub const S5_TYP: u8 = 5;
pub const RESET_PORT: u16 = 0xcf9;
pub const RESET_VALUE: u8 = 0x06;
pub const KBC_CMD: u16 = 0x64;
pub const IOAPIC_BASE: u64 = 0xfec0_0000;
pub const HPET_BASE: u64 = 0xfed0_0000;
pub const LAPIC_BASE: u64 = 0xfee0_0000;
/// HPET counter period: 10 ns (100 MHz), as QEMU's HPET.
pub const HPET_PERIOD_FS: u64 = 10_000_000;
const IOAPIC_PINS: usize = 24;
const HPET_TIMERS: usize = 3;
const UNKNOWN_KEEP: usize = 16;

/// What the guest's last access asks of the VMM.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Event {
    Continue,
    PowerOff,
    Reset,
}

/// The console side of COM1.
pub trait Serial {
    fn tx(&mut self, byte: u8);
    fn rx_ready(&mut self) -> bool;
    fn rx(&mut self) -> Option<u8>;
}

#[derive(Debug, Default)]
pub struct Uart {
    ier: u8,
    lcr: u8,
    mcr: u8,
    scr: u8,
    dll: u8,
    dlm: u8,
    fifo: bool,
}

impl Uart {
    fn dlab(&self) -> bool {
        self.lcr & 0x80 != 0
    }

    pub fn read(&mut self, reg: u16, s: &mut dyn Serial) -> u8 {
        match reg {
            0 if self.dlab() => self.dll,
            0 => s.rx().unwrap_or(0),
            1 if self.dlab() => self.dlm,
            1 => self.ier,
            2 => 0x01 | if self.fifo { 0xc0 } else { 0 }, // no interrupt pending
            3 => self.lcr,
            4 => self.mcr,
            5 => 0x60 | u8::from(s.rx_ready()), // THRE + TEMT, DR when a key waits
            6 => 0xb0,                          // DCD, DSR, CTS
            _ => self.scr,
        }
    }

    pub fn write(&mut self, reg: u16, v: u8, s: &mut dyn Serial) {
        match reg {
            0 if self.dlab() => self.dll = v,
            0 => s.tx(v),
            1 if self.dlab() => self.dlm = v,
            1 => self.ier = v & 0x0f,
            2 => self.fifo = v & 1 != 0,
            3 => self.lcr = v,
            4 => self.mcr = v & 0x1f,
            5 | 6 => {}
            _ => self.scr = v,
        }
    }
}

#[derive(Debug, Default)]
pub struct Hpet {
    conf: u64,
    /// Host time (ns) when the counter was last enabled, and its value then.
    enabled_at: Option<u64>,
    base: u64,
    timer_conf: [u64; HPET_TIMERS],
    timer_cmp: [u64; HPET_TIMERS],
}

impl Hpet {
    fn caps() -> u64 {
        HPET_PERIOD_FS << 32
            | 0x8086 << 16
            | 1 << 15
            | 1 << 13
            | ((HPET_TIMERS as u64 - 1) << 8)
            | 1
    }

    pub fn counter(&self, now_ns: u64) -> u64 {
        match self.enabled_at {
            Some(t) => {
                let ticks =
                    u128::from(now_ns.saturating_sub(t)) * 1_000_000 / u128::from(HPET_PERIOD_FS);
                self.base.wrapping_add(ticks as u64)
            }
            None => self.base,
        }
    }

    fn reg(&self, off: u64, now_ns: u64) -> u64 {
        match off {
            0x000 => Self::caps(),
            0x010 => self.conf,
            0x0f0 => self.counter(now_ns),
            0x100..=0x15f => {
                let (n, r) = (((off - 0x100) / 0x20) as usize, (off - 0x100) % 0x20);
                match r {
                    // 64-bit capable, periodic capable on timer 0, may route to any of 0..23
                    0x00 => {
                        self.timer_conf[n] | 1 << 5 | u64::from(n == 0) << 4 | 0x00ff_ffff << 32
                    }
                    0x08 => self.timer_cmp[n],
                    _ => 0,
                }
            }
            _ => 0,
        }
    }

    pub fn read(&self, off: u64, size: u8, now_ns: u64) -> u64 {
        let v = self.reg(off & !7, now_ns) >> ((off & 4) * 8);
        mask(v, size)
    }

    pub fn write(&mut self, off: u64, size: u8, val: u64, now_ns: u64) {
        let r = off & !7;
        let v = if size == 8 {
            val
        } else {
            let shift = (off & 4) * 8;
            let old = self.reg(r, now_ns);
            (old & !(0xffff_ffff << shift)) | (mask(val, 4) << shift)
        };
        match r {
            0x010 => {
                let was = self.conf & 1 != 0;
                let on = v & 1 != 0;
                if on && !was {
                    self.enabled_at = Some(now_ns);
                } else if was && !on {
                    self.base = self.counter(now_ns);
                    self.enabled_at = None;
                }
                self.conf = v & 3;
            }
            0x0f0 if self.enabled_at.is_none() => self.base = v,
            0x100..=0x15f => {
                let (n, rr) = (((r - 0x100) / 0x20) as usize, (r - 0x100) % 0x20);
                match rr {
                    0x00 => self.timer_conf[n] = v & 0x7f4e,
                    0x08 => self.timer_cmp[n] = v,
                    _ => {}
                }
            }
            _ => {}
        }
    }
}

#[derive(Debug)]
pub struct IoApic {
    id: u8,
    sel: u8,
    redir: [u64; IOAPIC_PINS],
}

impl IoApic {
    pub fn new(id: u8) -> IoApic {
        IoApic {
            id,
            sel: 0,
            redir: [1 << 16; IOAPIC_PINS], // masked at reset
        }
    }

    fn reg(&self) -> u32 {
        match self.sel {
            0 | 2 => u32::from(self.id) << 24,
            1 => ((IOAPIC_PINS as u32 - 1) << 16) | 0x11,
            r @ 0x10..=0x3f => {
                let e = self.redir[usize::from(r - 0x10) / 2];
                if r & 1 == 0 {
                    e as u32
                } else {
                    (e >> 32) as u32
                }
            }
            _ => 0,
        }
    }

    pub fn read(&self, off: u64) -> u32 {
        match off {
            0x00 => u32::from(self.sel),
            0x10 => self.reg(),
            _ => 0,
        }
    }

    pub fn write(&mut self, off: u64, v: u32) {
        match off {
            0x00 => self.sel = v as u8,
            0x10 => match self.sel {
                0 => self.id = (v >> 24) as u8 & 0x0f,
                r @ 0x10..=0x3f => {
                    let e = &mut self.redir[usize::from(r - 0x10) / 2];
                    *e = if r & 1 == 0 {
                        (*e & !0xffff_ffff) | u64::from(v & !(1 << 12 | 1 << 14))
                    // RO bits
                    } else {
                        (*e & 0xffff_ffff) | u64::from(v) << 32
                    };
                }
                _ => {}
            },
            _ => {}
        }
    }

    pub fn masked(&self, pin: usize) -> bool {
        self.redir[pin] & 1 << 16 != 0
    }
}

#[derive(Debug)]
pub struct Pm {
    sts: u16,
    en: u16,
    cnt: u16,
}

impl Default for Pm {
    fn default() -> Pm {
        Pm {
            sts: 0,
            en: 0,
            cnt: 1, // SCI_EN: ACPI mode on, nothing to enable
        }
    }
}

fn mask(v: u64, size: u8) -> u64 {
    match size {
        1 => v & 0xff,
        2 => v & 0xffff,
        4 => v & 0xffff_ffff,
        _ => v,
    }
}

fn ones(size: u8) -> u64 {
    mask(u64::MAX, size)
}

/// The whole device model.
#[derive(Debug)]
pub struct Platform {
    pub uart: Uart,
    pub hpet: Hpet,
    pub ioapic: IoApic,
    pub pm: Pm,
    /// First unknown ports ("io 0x..") and MMIO addresses ("mmio 0x..") the guest touched.
    pub unknown: BTreeSet<String>,
    pci_addr: u32,
}

impl Platform {
    pub fn new(cpus: u32) -> Platform {
        Platform {
            uart: Uart::default(),
            hpet: Hpet::default(),
            ioapic: IoApic::new(cpus as u8),
            pm: Pm::default(),
            unknown: BTreeSet::new(),
            pci_addr: 0,
        }
    }

    fn note(&mut self, what: String) {
        if self.unknown.len() < UNKNOWN_KEEP {
            self.unknown.insert(what);
        }
    }

    pub fn io_in(&mut self, port: u16, size: u8, s: &mut dyn Serial) -> u32 {
        match port {
            COM1..=0x3ff => u32::from(self.uart.read(port - COM1, s)),
            PM1A_EVT..=0x607 => {
                let regs = [self.pm.sts, self.pm.en, self.pm.cnt, 0];
                let off = usize::from(port - PM1A_EVT);
                let word = u32::from(regs[off / 2]) | u32::from(regs[(off / 2 + 1).min(3)]) << 16;
                mask(u64::from(word >> ((off % 2) * 8)), size) as u32
            }
            0xcf8 if size == 4 => self.pci_addr,
            0xcfc..=0xcff => ones(size) as u32, // no PCI function answers
            KBC_CMD | 0x60 => 0,                // 8042: nothing to read, input buffer empty
            RESET_PORT => 0,
            _ => {
                self.note(format!("io 0x{port:x}"));
                ones(size) as u32
            }
        }
    }

    pub fn io_out(&mut self, port: u16, size: u8, v: u32, s: &mut dyn Serial) -> Event {
        match port {
            COM1..=0x3ff => self.uart.write(port - COM1, v as u8, s),
            PM1A_EVT..=0x603 => {
                if port < PM1A_EVT + 2 {
                    self.pm.sts &= !(v as u16); // write 1 to clear
                } else {
                    self.pm.en = v as u16;
                }
            }
            PM1A_CNT => {
                let v = mask(u64::from(v), size) as u16;
                self.pm.cnt = (v & !(1 << 13)) | 1;
                if v & 1 << 13 != 0 && ((v >> 10) & 7) as u8 == S5_TYP {
                    return Event::PowerOff;
                }
            }
            0xcf8 if size == 4 => self.pci_addr = v,
            0xcfc..=0xcff | 0x80 | 0x60 => {} // PCI data with no device, POST code, 8042 data
            RESET_PORT if v & 4 != 0 => return Event::Reset,
            RESET_PORT => {}
            KBC_CMD if v as u8 == 0xfe => return Event::Reset,
            KBC_CMD => {}
            _ => self.note(format!("io 0x{port:x}")),
        }
        Event::Continue
    }

    pub fn mmio_read(&mut self, gpa: u64, size: u8, now_ns: u64) -> u64 {
        match gpa {
            HPET_BASE..=0xfed0_03ff => self.hpet.read(gpa - HPET_BASE, size, now_ns),
            IOAPIC_BASE..=0xfec0_00ff => mask(u64::from(self.ioapic.read(gpa - IOAPIC_BASE)), size),
            _ => {
                self.note(format!("mmio 0x{gpa:x}"));
                ones(size)
            }
        }
    }

    pub fn mmio_write(&mut self, gpa: u64, size: u8, v: u64, now_ns: u64) {
        match gpa {
            HPET_BASE..=0xfed0_03ff => self.hpet.write(gpa - HPET_BASE, size, v, now_ns),
            IOAPIC_BASE..=0xfec0_00ff => self.ioapic.write(gpa - IOAPIC_BASE, v as u32),
            _ => self.note(format!("mmio 0x{gpa:x}")),
        }
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use std::collections::VecDeque;

    #[derive(Default)]
    pub(crate) struct Fake {
        pub out: Vec<u8>,
        pub keys: VecDeque<u8>,
    }

    impl Serial for Fake {
        fn tx(&mut self, b: u8) {
            self.out.push(b);
        }
        fn rx_ready(&mut self) -> bool {
            !self.keys.is_empty()
        }
        fn rx(&mut self) -> Option<u8> {
            self.keys.pop_front()
        }
    }

    #[test]
    fn com1_probe_transmit_receive() {
        let mut p = Platform::new(1);
        let mut s = Fake::default();
        // native/kernel/x86/console_x86.c ck_x86_com1_probe: scratch register round trip
        for v in [0x5a, 0xa5] {
            p.io_out(COM1 + 7, 1, v, &mut s);
            assert_eq!(p.io_in(COM1 + 7, 1, &mut s), v);
        }
        // divisor latch does not leak into THR
        p.io_out(COM1 + 3, 1, 0x80, &mut s);
        p.io_out(COM1, 1, 1, &mut s);
        p.io_out(COM1 + 3, 1, 0x03, &mut s);
        assert!(s.out.is_empty());
        assert_eq!(p.io_in(COM1 + 5, 1, &mut s) & 0x21, 0x20, "THRE, no data");
        for b in b"ok\n" {
            p.io_out(COM1, 1, u32::from(*b), &mut s);
        }
        assert_eq!(s.out, b"ok\n");
        s.keys.extend(b"a");
        assert_eq!(p.io_in(COM1 + 5, 1, &mut s) & 1, 1, "data ready");
        assert_eq!(p.io_in(COM1, 1, &mut s), u32::from(b'a'));
        assert_eq!(p.io_in(COM1 + 5, 1, &mut s) & 1, 0);
    }

    #[test]
    fn hpet_counts_from_enable_like_tsc_measure() {
        let mut p = Platform::new(1);
        let gcap = p.mmio_read(HPET_BASE, 8, 0);
        assert_eq!(gcap >> 32, HPET_PERIOD_FS);
        assert_eq!(
            p.mmio_read(HPET_BASE + 4, 4, 0),
            HPET_PERIOD_FS,
            "high half by 32-bit read"
        );
        assert_eq!(
            p.mmio_read(HPET_BASE + 0xf0, 8, 5_000),
            0,
            "halted until enabled"
        );
        // hp[2] |= 1 at t = 1 ms
        let conf = p.mmio_read(HPET_BASE + 0x10, 8, 1_000_000);
        p.mmio_write(HPET_BASE + 0x10, 8, conf | 1, 1_000_000);
        // 50 ms later: 5,000,000 ticks of 10 ns
        assert_eq!(p.mmio_read(HPET_BASE + 0xf0, 8, 51_000_000), 5_000_000);
        assert_eq!(p.mmio_read(HPET_BASE + 0xf0, 4, 51_000_000), 5_000_000);
        // disable freezes, re-enable continues
        p.mmio_write(HPET_BASE + 0x10, 4, 0, 61_000_000);
        assert_eq!(p.mmio_read(HPET_BASE + 0xf0, 8, 99_000_000), 6_000_000);
        p.mmio_write(HPET_BASE + 0x10, 4, 1, 100_000_000);
        assert_eq!(p.mmio_read(HPET_BASE + 0xf0, 8, 100_000_100), 6_000_010);
    }

    #[test]
    fn ioapic_mask_all_like_the_kernel() {
        let mut p = Platform::new(1);
        // ioapic_mask_all: io[0] = 1; n = ((io[4] >> 16) & 0xff) + 1
        p.mmio_write(IOAPIC_BASE, 4, 1, 0);
        let n = ((p.mmio_read(IOAPIC_BASE + 0x10, 4, 0) >> 16) & 0xff) + 1;
        assert_eq!(n, 24);
        for i in 0..n {
            p.mmio_write(IOAPIC_BASE, 4, 0x10 + 2 * i, 0);
            let v = p.mmio_read(IOAPIC_BASE + 0x10, 4, 0);
            p.mmio_write(IOAPIC_BASE + 0x10, 4, v | 1 << 16, 0);
        }
        assert!((0..24).all(|i| p.ioapic.masked(i)));
        p.mmio_write(IOAPIC_BASE, 4, 0x10, 0);
        p.mmio_write(IOAPIC_BASE + 0x10, 4, 0x40, 0);
        assert!(!p.ioapic.masked(0));
        p.mmio_write(IOAPIC_BASE, 4, 0, 0);
        assert_eq!(
            p.mmio_read(IOAPIC_BASE + 0x10, 4, 0) >> 24,
            1,
            "id after the CPUs"
        );
        assert!(p.unknown.is_empty());
    }

    #[test]
    fn s5_power_off_and_resets() {
        let mut p = Platform::new(1);
        let mut s = Fake::default();
        assert_eq!(p.io_in(PM1A_CNT, 2, &mut s) & 1, 1, "SCI_EN set");
        // ck_outw(pm1a, ta << 10 | 1 << 13)
        let s3 = u32::from(3u8) << 10 | 1 << 13;
        assert_eq!(
            p.io_out(PM1A_CNT, 2, s3, &mut s),
            Event::Continue,
            "only S5 is offered"
        );
        let s5 = u32::from(S5_TYP) << 10 | 1 << 13;
        assert_eq!(p.io_out(PM1A_CNT, 2, s5, &mut s), Event::PowerOff);
        assert_eq!(p.io_out(RESET_PORT, 1, 0x02, &mut s), Event::Continue);
        assert_eq!(
            p.io_out(RESET_PORT, 1, u32::from(RESET_VALUE), &mut s),
            Event::Reset
        );
        assert_eq!(p.io_out(KBC_CMD, 1, 0xfe, &mut s), Event::Reset);
    }

    #[test]
    fn unknown_reads_all_ones_and_are_noted() {
        let mut p = Platform::new(1);
        let mut s = Fake::default();
        assert_eq!(p.io_in(0x1234, 2, &mut s), 0xffff);
        assert_eq!(p.io_in(0xcfc, 4, &mut s), 0xffff_ffff);
        assert_eq!(p.mmio_read(0xe000_0000, 4, 0), 0xffff_ffff);
        assert_eq!(
            p.unknown.iter().cloned().collect::<Vec<_>>(),
            ["io 0x1234", "mmio 0xe0000000"]
        );
        for port in 0..100u16 {
            p.io_in(0x2000 + port, 1, &mut s);
        }
        assert_eq!(p.unknown.len(), UNKNOWN_KEEP);
    }
}
