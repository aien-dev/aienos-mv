//! ARM PrimeCell UART (PL011) model: the guest's serial console.
//! Registers per the ARM PL011 Technical Reference Manual (DDI 0183), section 3.2:
//! DR 0x000, RSR 0x004, FR 0x018, IBRD 0x024, FBRD 0x028, LCR_H 0x02c, CR 0x030,
//! IFLS 0x034, IMSC 0x038, RIS 0x03c, MIS 0x040, ICR 0x044, DMACR 0x048, ID 0xfe0-0xffc.
//! Polled only (the AienOS console polls); no interrupt line is raised.

use crate::console::Console;

const FR_RXFE: u32 = 1 << 4;
const FR_TXFE: u32 = 1 << 7;
/// CR as firmware leaves it: UARTEN | TXE | RXE.
const CR_ENABLED: u32 = 0x301;
const ID: [u32; 8] = [0x11, 0x10, 0x14, 0x00, 0x0d, 0xf0, 0x05, 0xb1];

pub struct Pl011 {
    regs: [u32; 0x50 / 4],
    tx: Vec<u8>,
}

impl Default for Pl011 {
    fn default() -> Self {
        let mut regs = [0u32; 0x50 / 4];
        regs[0x30 / 4] = CR_ENABLED;
        regs[0x34 / 4] = 0x12;
        Pl011 {
            regs,
            tx: Vec::with_capacity(1024),
        }
    }
}

impl Pl011 {
    pub fn read(&mut self, off: u64, console: &Console) -> u32 {
        match off {
            0x000 => console.pop_input().map(u32::from).unwrap_or(0),
            0x018 => {
                if console.has_input() {
                    FR_TXFE
                } else {
                    FR_TXFE | FR_RXFE
                }
            }
            0xfe0..=0xffc => ID[((off - 0xfe0) / 4) as usize],
            o if (o as usize) < 0x50 => self.regs[o as usize / 4],
            _ => 0,
        }
    }

    /// Returns true when the buffered output should be flushed now (end of line).
    pub fn write(&mut self, off: u64, value: u32) -> bool {
        match off {
            0x000 => {
                let b = value as u8;
                self.tx.push(b);
                b == b'\n' || self.tx.len() >= 1024
            }
            0x018 | 0x03c | 0x040 => false, // read-only
            o if (o as usize) < 0x50 => {
                self.regs[o as usize / 4] = value;
                false
            }
            _ => false,
        }
    }

    pub fn flush(&mut self, console: &Console) {
        if !self.tx.is_empty() {
            console.guest_output(&self.tx);
            self.tx.clear();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Instant;

    #[test]
    fn tx_rx_and_flags() {
        let c = Console::new(None, &[], Instant::now()).keep_transcript();
        let mut u = Pl011::default();
        assert_eq!(u.read(0x30, &c), CR_ENABLED);
        assert_eq!(u.read(0x18, &c) & FR_RXFE, FR_RXFE);
        assert!(!u.write(0, b'h' as u32));
        assert!(u.write(0, b'\n' as u32));
        u.flush(&c);
        assert_eq!(c.transcript(), b"h\n");
        c.push_input(b"x");
        assert_eq!(u.read(0x18, &c) & FR_RXFE, 0);
        assert_eq!(u.read(0, &c), b'x' as u32);
        assert_eq!(u.read(0x18, &c) & FR_RXFE, FR_RXFE);
        assert_eq!(u.read(0xfe0, &c), 0x11);
        assert_eq!(u.read(0xffc, &c), 0xb1);
    }
}
