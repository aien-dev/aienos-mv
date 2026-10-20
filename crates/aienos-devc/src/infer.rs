//! Inference from a register trace (ADR 0021 "Chips with no manual").
//!
//! Trace format `devc-trace 1`, one register access per line, anything else ignored:
//!
//! ```text
//! R common+0x14/1 0x0        read  window+offset/width value
//! W common+0x14/1 0x1        write window+offset/width value
//! ```
//!
//! It is what the generated chip model prints (`vnet_model.c`), and what a
//! recording of a vendor driver under QEMU or a hypervisor is converted to.
//! `infer` turns one trace into a draft contract: every register touched with
//! its width and access (read-only, write-only, read-write), the value read
//! before any write (a reset-value candidate), every value written, the
//! repeated reads that look like polls, and the write order as a draft `init`.
//! The draft has no `manual` and no `proven` line, so `parse` refuses it until
//! someone writes the evidence: inference proposes, the checker and the gates
//! decide.

use crate::contract::{Access, Contract};
use std::collections::BTreeMap;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Access1 {
    pub write: bool,
    pub window: usize,
    pub offset: u64,
    pub width: u32,
    pub value: u64,
}

/// Parse one trace line; `None` for anything that is not a register access.
pub fn parse_line(line: &str, windows: &mut Vec<String>) -> Option<Access1> {
    let mut w = line.split_whitespace();
    let write = match w.next()? {
        "R" => false,
        "W" => true,
        _ => return None,
    };
    let (win, rest) = w.next()?.split_once("+0x")?;
    let (off, width) = rest.split_once('/')?;
    let value = w.next()?.strip_prefix("0x")?;
    if w.next().is_some() || win.is_empty() {
        return None;
    }
    let offset = u64::from_str_radix(off, 16).ok()?;
    let width: u32 = width.parse().ok()?;
    let value = u64::from_str_radix(value, 16).ok()?;
    if !matches!(width, 1 | 2 | 4 | 8) {
        return None;
    }
    let window = match windows.iter().position(|n| n == win) {
        Some(i) => i,
        None => {
            windows.push(win.to_string());
            windows.len() - 1
        }
    };
    Some(Access1 {
        write,
        window,
        offset,
        width,
        value,
    })
}

#[derive(Debug, Default)]
pub struct Seen {
    pub reads: usize,
    pub writes: usize,
    pub first_read_before_write: Option<u64>,
    pub written: Vec<u64>,
    pub longest_poll: usize,
}

impl Seen {
    pub fn access(&self) -> Access {
        match (self.reads > 0, self.writes > 0) {
            (true, true) => Access::Rw,
            (false, true) => Access::Wo,
            _ => Access::Ro,
        }
    }
}

pub struct Inferred {
    pub windows: Vec<String>,
    /// (window, offset, width) -> what was seen
    pub regs: BTreeMap<(usize, u64, u32), Seen>,
    /// the write order, first occurrence of each (register, value)
    pub writes: Vec<(usize, u64, u32, u64)>,
    pub accesses: usize,
}

pub fn infer(trace: &str) -> Inferred {
    let mut windows = Vec::new();
    let mut regs: BTreeMap<(usize, u64, u32), Seen> = BTreeMap::new();
    let mut writes = Vec::new();
    let mut accesses = 0;
    let mut run: Option<((usize, u64, u32), u64, usize)> = None;
    for line in trace.lines() {
        let Some(a) = parse_line(line, &mut windows) else {
            continue;
        };
        accesses += 1;
        let key = (a.window, a.offset, a.width);
        let s = regs.entry(key).or_default();
        if a.write {
            s.writes += 1;
            if !s.written.contains(&a.value) {
                s.written.push(a.value);
            }
            if !writes.contains(&(a.window, a.offset, a.width, a.value)) {
                writes.push((a.window, a.offset, a.width, a.value));
            }
            run = None;
        } else {
            s.reads += 1;
            if s.writes == 0 && s.first_read_before_write.is_none() {
                s.first_read_before_write = Some(a.value);
            }
            run = match run {
                Some((k, v, n)) if k == key && v == a.value => Some((k, v, n + 1)),
                _ => Some((key, a.value, 1)),
            };
            if let Some((_, _, n)) = run {
                if n > s.longest_poll {
                    s.longest_poll = n;
                }
            }
        }
    }
    Inferred {
        windows,
        regs,
        writes,
        accesses,
    }
}

fn access_word(a: Access) -> &'static str {
    match a {
        Access::Ro => "ro",
        Access::Rw => "rw",
        Access::Wo => "wo",
    }
}

fn reg_name(inf: &Inferred, k: &(usize, u64, u32)) -> String {
    format!("{}_{:02X}", inf.windows[k.0].to_uppercase(), k.1)
}

/// The draft contract, in `.devc` words. Not accepted by `parse` as written.
pub fn draft(inf: &Inferred, device: &str, source: &str) -> String {
    let mut o = String::new();
    o += &format!(
        "# DRAFT inferred by `aienos-devc infer` from {source}: {} register accesses, {} registers.\n",
        inf.accesses,
        inf.regs.len()
    );
    o += "# Status UNPROVEN and UNCHECKED: names, bit meanings, banks and error paths are\n";
    o +=
        "# not in a trace. Add `manual` (datasheet or DOCS SILENT + trace) and the rest by hand;\n";
    o += "# the checker refuses this file until then.\n";
    o += &format!("devc 1\ndevice {device}\n");
    o += "# evidence: trace ";
    o += source;
    o += "\n\n";
    for w in &inf.windows {
        o += &format!("window {w} ? min ?\n");
    }
    o += "\n";
    for (k, s) in &inf.regs {
        let mut l = format!(
            "reg {} {} 0x{:02x} {} {}",
            inf.windows[k.0],
            reg_name(inf, k),
            k.1,
            k.2,
            access_word(s.access())
        );
        if let Some(v) = s.first_read_before_write {
            l += &format!(" reset 0x{v:x}");
        }
        let mut notes = vec![format!("reads {} writes {}", s.reads, s.writes)];
        if !s.written.is_empty() {
            let vals: Vec<String> = s.written.iter().map(|v| format!("0x{v:x}")).collect();
            notes.push(format!("written {}", vals.join(" ")));
        }
        if s.longest_poll > 2 {
            notes.push(format!("polled: {} equal reads in a row", s.longest_poll));
        }
        o += &format!("{l}   # {}\n", notes.join("; "));
    }
    o += "\n# write order (first time each register takes each value): the draft bring-up\ninit\n";
    for (w, off, width, v) in &inf.writes {
        o += &format!("  write {} 0x{v:x}\n", reg_name(inf, &(*w, *off, *width)));
    }
    o += "end\n";
    o
}

/// One disagreement between a trace and a contract.
#[derive(Debug, PartialEq, Eq)]
pub struct Mismatch(pub String);

/// Every access in the trace must land on a register the contract declares,
/// with an access the contract allows. Returns the disagreements (empty: the
/// trace stays inside the contract).
pub fn covered_by(inf: &Inferred, c: &Contract) -> Vec<Mismatch> {
    let mut out = Vec::new();
    for (k, s) in &inf.regs {
        let win = &inf.windows[k.0];
        let hit = c.regs.iter().find(|r| {
            &r.window == win
                && k.1 >= r.offset
                && k.1 + u64::from(k.2) <= r.offset + u64::from(r.width) * u64::from(r.count)
                && (k.1 - r.offset) % u64::from(r.width) == 0
                && r.width == k.2
        });
        match hit {
            None => out.push(Mismatch(format!(
                "{win}+0x{:x}/{} is not a declared register",
                k.1, k.2
            ))),
            Some(r) => {
                let ok = match r.access {
                    Access::Rw => true,
                    Access::Ro => s.writes == 0,
                    Access::Wo => s.reads == 0,
                };
                if !ok {
                    out.push(Mismatch(format!(
                        "{} is {} in the contract but the trace {}",
                        r.name,
                        access_word(r.access),
                        if s.writes > 0 {
                            "writes it"
                        } else {
                            "reads it"
                        }
                    )));
                }
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_model_trace_format() {
        let t = "== init\n  R common+0x14/1 0x0\n  W common+0x14/1 0x1\n  W common+0x14/1 0x3\n  R common+0x14/1 0x3\n  R common+0x14/1 0x3\n  R common+0x14/1 0x3\nvnet_init -> Ok\n  R device+0x00/1 0x52\n";
        let inf = infer(t);
        assert_eq!(inf.accesses, 7);
        assert_eq!(inf.windows, vec!["common", "device"]);
        let st = &inf.regs[&(0, 0x14, 1)];
        assert_eq!((st.reads, st.writes), (4, 2));
        assert_eq!(st.first_read_before_write, Some(0));
        assert_eq!(st.written, vec![1, 3]);
        assert_eq!(st.longest_poll, 3);
        assert!(matches!(inf.regs[&(1, 0, 1)].access(), Access::Ro));
        let d = draft(&inf, "x", "t");
        assert!(d.contains("reg common COMMON_14 0x14 1 rw reset 0x0"));
        assert!(d.contains("  write COMMON_14 0x3\n"));
        assert!(
            crate::contract::parse(&d, "draft").is_err(),
            "a draft must not pass the checker"
        );
    }

    #[test]
    fn ignores_lines_that_are_not_accesses() {
        let mut w = Vec::new();
        for l in [
            "R common+0x14/3 0x0",
            "R common+0x14/1",
            "X common+0x14/1 0x0",
            "R +0x14/1 0x0",
            "R common+0x14/1 0x0 extra",
        ] {
            assert!(parse_line(l, &mut w).is_none(), "{l}");
        }
    }

    /// The committed recording of the hand-written driver (contract/traces/)
    /// must stay inside the contract: every register it touches is declared,
    /// with the access the contract gives it.
    #[test]
    fn hand_driver_trace_stays_inside_the_contract() {
        let src = include_str!("../../../native/net/contract/virtio_net.devc");
        let trace = include_str!("../../../native/net/contract/traces/virtio_net_hand.trace");
        let c = crate::contract::parse(src, "virtio_net.devc").expect("contract");
        let inf = infer(trace);
        assert!(inf.accesses > 100, "trace too short: {}", inf.accesses);
        assert_eq!(covered_by(&inf, &c), vec![]);
        let mut bad = c.clone();
        bad.regs.retain(|r| r.name != "Q_NOFF");
        assert_eq!(
            covered_by(&inf, &bad),
            vec![Mismatch("common+0x1e/2 is not a declared register".into())]
        );
    }
}
