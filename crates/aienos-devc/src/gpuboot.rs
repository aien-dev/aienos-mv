//! Class `gpu-boot` (L332 rung 1, L346): a GPU firmware start-up over BAR0 as a device contract.
//!
//! Same lexical format as devc 1 (`contract::words`), its own statements: `window`, `reg`, `bit`,
//! `field`, `const`, and two step blocks (`boot`, `notify`) from a closed list of verbs. The checker
//! refuses unsafe orders (an EMEM write while the channel still holds a message, the doorbell before
//! the tail, any write after the doorbell to the channel, a touch of the GSP before SEC2 releases its
//! target mask, FALCON_OS before the FMC is checked, a wait with no budget). The emitter writes C with
//! the API of native/kernel/gpu/gsp_mmio.h, using its own offsets and bits (never gsp_mmio.h's), so the
//! hand-written twin and the generated one are two independent statements that must agree.

use crate::contract::{err, is_ident, num, words, Error};

#[derive(Debug, Clone)]
pub struct Reg {
    pub win: String,
    pub name: String,
    pub off: u64,
    pub width: u64,
    pub access: String,
}

#[derive(Debug, Clone)]
pub struct Win {
    pub name: String,
    pub bar: u64,
    pub base: u64,
    pub size: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Step {
    RequirePacket { max: String, err: String },
    WaitEqual { a: String, b: String, budget: String, err: String },
    EmemSend { ctrl: String, data: String, ainc: String, offs: String, blk: String, per_blk: String, err: String },
    PostTail(String),
    Doorbell(String),
    WaitNot { reg: String, val: String, budget: String, into: String, tread: Option<(String, String)>, err: String },
    WaitReleased { reg: String, locked: String, mask: String, budget: String, into: String, err: String },
    WaitLockdown { mb0: String, mb1: String, bit: String, budget: String, err: String },
    RefuseNonzero { reg: String, into: String, err: String },
    Write { reg: String, val: String },
    RequireBit { bit: String, into: String, err: String },
}

#[derive(Debug, Default, Clone)]
pub struct Boot {
    pub source_name: String,
    pub device: String,
    pub prefix: String,
    pub class: String,
    pub transport: String,
    pub manuals: Vec<(String, String, String)>,
    pub proven: Vec<Vec<String>>,
    pub consts: Vec<(String, u64)>,
    pub windows: Vec<Win>,
    pub regs: Vec<Reg>,
    /// (name, reg, bit)
    pub bits: Vec<(String, String, u32)>,
    /// (name, reg, hi, lo)
    pub fields: Vec<(String, String, u32, u32)>,
    pub boot: Vec<Step>,
    pub notify: Vec<Step>,
    pub stmt_count: usize,
}

/// The stages of gsp_mmio.h (enum ck_gsp_mmio_err) a step may stop at.
const API_ERRORS: &[&str] = &["E_ARG", "E_BUSY", "E_EMEM", "E_SEC2", "E_MASK", "E_LOCKDOWN", "E_FMC", "E_RISCV"];
/// Poll budgets of struct ck_gsp_mmio_in, in polls[] slot order.
const BUDGETS: &[&str] = &["polls_can_send", "polls_cmd", "polls_mask", "polls_lockdown"];
/// Fields of struct ck_gsp_mmio_out a read may land in.
const OUTS: &[&str] = &["sec2_mb0", "sec2_mb1", "hwcfg2", "gsp_mb0", "gsp_mb1", "cpuctl"];
/// Values of struct ck_gsp_mmio_in a `write` may store.
const WRITE_ARGS: &[&str] = &["app_version"];

/// True when the source declares `class gpu-boot` (main.rs dispatches on it).
pub fn is_gpu_boot(src: &str) -> bool {
    src.lines().any(|l| {
        let w: Vec<&str> = l.split('#').next().unwrap_or("").split_whitespace().collect();
        w == ["class", "gpu-boot"]
    })
}

fn else_err(w: &[String], at: usize, ln: usize) -> Result<String, Error> {
    if w.len() != at + 2 || w[at] != "else" {
        return err(ln, format!("`{}` must end with `else ERROR`", w[0]));
    }
    Ok(w[at + 1].clone())
}

/// `KEY value` at w[i] / w[i+1], or an error naming the step.
fn kw(w: &[String], i: usize, key: &str, ln: usize) -> Result<String, Error> {
    if w.get(i).map(String::as_str) != Some(key) || w.get(i + 1).is_none() {
        return err(ln, format!("`{}`: expected `{key} VALUE` at word {}", w[0], i + 1));
    }
    Ok(w[i + 1].clone())
}

fn parse_step(w: &[String], ln: usize, block: &str) -> Result<Step, Error> {
    let n = w.len();
    let at = |i: usize| w.get(i).cloned().unwrap_or_default();
    let st = match (block, w[0].as_str()) {
        ("boot", "require-packet") if n == 4 => Step::RequirePacket { max: at(1), err: else_err(w, 2, ln)? },
        ("boot", "wait-equal") if n == 7 => {
            Step::WaitEqual { a: at(1), b: at(2), budget: kw(w, 3, "budget", ln)?, err: else_err(w, 5, ln)? }
        }
        ("boot", "emem-send") if n == 9 => Step::EmemSend {
            ctrl: at(1),
            data: at(2),
            ainc: at(3),
            offs: at(4),
            blk: at(5),
            per_blk: at(6),
            err: else_err(w, 7, ln)?,
        },
        ("boot", "post-tail") if n == 2 => Step::PostTail(at(1)),
        ("boot", "doorbell") if n == 2 => Step::Doorbell(at(1)),
        ("boot", "wait-not") if n == 9 || n == 12 => {
            let tread = if n == 12 { Some((kw(w, 7, "timeout-read", ln)?, at(9))) } else { None };
            Step::WaitNot {
                reg: at(1),
                val: at(2),
                budget: kw(w, 3, "budget", ln)?,
                into: kw(w, 5, "into", ln)?,
                tread,
                err: else_err(w, n - 2, ln)?,
            }
        }
        ("boot", "wait-released") if n == 10 => Step::WaitReleased {
            reg: at(1),
            locked: at(2),
            mask: at(3),
            budget: kw(w, 4, "budget", ln)?,
            into: kw(w, 6, "into", ln)?,
            err: else_err(w, 8, ln)?,
        },
        ("boot", "wait-lockdown") if n == 8 => Step::WaitLockdown {
            mb0: at(1),
            mb1: at(2),
            bit: at(3),
            budget: kw(w, 4, "budget", ln)?,
            err: else_err(w, 6, ln)?,
        },
        ("boot", "refuse-nonzero") if n == 6 => {
            Step::RefuseNonzero { reg: at(1), into: kw(w, 2, "into", ln)?, err: else_err(w, 4, ln)? }
        }
        ("boot" | "notify", "write") if n == 3 => Step::Write { reg: at(1), val: at(2) },
        ("boot", "require-bit") if n == 6 => {
            Step::RequireBit { bit: at(1), into: kw(w, 2, "into", ln)?, err: else_err(w, 4, ln)? }
        }
        (b, o) => return err(ln, format!("`{o}` is not a step of `{b}` (or has the wrong number of words)")),
    };
    Ok(st)
}

fn range(s: &str, ln: usize) -> Result<(u32, u32), Error> {
    let Some((h, l)) = s.split_once(':') else {
        return err(ln, format!("expected HI:LO, got `{s}`"));
    };
    let (h, l) = (num(h, ln)? as u32, num(l, ln)? as u32);
    if h < l {
        return err(ln, format!("`{s}`: HI below LO"));
    }
    Ok((h, l))
}

pub fn parse(src: &str, source_name: &str) -> Result<Boot, Error> {
    let mut g = Boot { source_name: source_name.to_string(), ..Default::default() };
    let mut block: Option<String> = None;
    let mut header = false;
    for (i, raw) in src.lines().enumerate() {
        let ln = i + 1;
        let w = words(raw, ln)?;
        if w.is_empty() {
            continue;
        }
        g.stmt_count += 1;
        let s = |k: usize| w.get(k).map(String::as_str).unwrap_or("");
        if let Some(b) = block.clone() {
            if s(0) == "end" {
                block = None;
                continue;
            }
            let st = parse_step(&w, ln, &b)?;
            if b == "boot" {
                g.boot.push(st);
            } else {
                g.notify.push(st);
            }
            continue;
        }
        if !header {
            if w != ["devc", "1"] {
                return err(ln, "the first statement must be `devc 1`");
            }
            header = true;
            continue;
        }
        match s(0) {
            "device" => g.device = s(1).into(),
            "prefix" => g.prefix = s(1).into(),
            "class" => g.class = s(1).into(),
            "transport" => g.transport = s(1).into(),
            "manual" if w.len() == 4 => g.manuals.push((s(1).into(), s(2).into(), s(3).into())),
            "proven" if w.len() == 6 => g.proven.push(w[1..].to_vec()),
            "const" if w.len() == 3 => g.consts.push((s(1).into(), num(s(2), ln)?)),
            "window" if w.len() == 8 && s(2) == "bar" && s(4) == "base" && s(6) == "size" => g.windows.push(Win {
                name: s(1).into(),
                bar: num(s(3), ln)?,
                base: num(s(5), ln)?,
                size: num(s(7), ln)?,
            }),
            "reg" if w.len() == 6 => g.regs.push(Reg {
                win: s(1).into(),
                name: s(2).into(),
                off: num(s(3), ln)?,
                width: num(s(4), ln)?,
                access: s(5).into(),
            }),
            "bit" if w.len() == 4 => g.bits.push((s(1).into(), s(2).into(), num(s(3), ln)? as u32)),
            "field" if w.len() == 4 => {
                let (h, l) = range(s(3), ln)?;
                g.fields.push((s(1).into(), s(2).into(), h, l))
            }
            "boot" | "notify" if w.len() == 1 => {
                if (s(0) == "boot" && !g.boot.is_empty()) || (s(0) == "notify" && !g.notify.is_empty()) {
                    return err(ln, format!("a second `{}` block", s(0)));
                }
                block = Some(s(0).into())
            }
            o => return err(ln, format!("unknown or malformed statement `{o}` for class gpu-boot")),
        }
    }
    if block.is_some() {
        return err(0, "a step block has no `end`");
    }
    check(&g)?;
    Ok(g)
}

impl Boot {
    fn konst(&self, n: &str) -> Option<u64> {
        self.consts.iter().find(|(k, _)| k == n).map(|(_, v)| *v)
    }
    fn reg(&self, n: &str) -> Option<&Reg> {
        self.regs.iter().find(|r| r.name == n)
    }
    fn bit(&self, n: &str) -> Option<&(String, String, u32)> {
        self.bits.iter().find(|b| b.0 == n)
    }
    fn field(&self, n: &str) -> Option<&(String, String, u32, u32)> {
        self.fields.iter().find(|f| f.0 == n)
    }
    fn addr(&self, reg: &str) -> u64 {
        let r = self.reg(reg).unwrap();
        self.windows.iter().find(|w| w.name == r.win).unwrap().base + r.off
    }
}

/// Registers a step reads (`r`) and writes (`w`), in that step's order.
fn touches(g: &Boot, s: &Step) -> (Vec<String>, Vec<String>) {
    let bit_reg = |b: &str| g.bit(b).map(|x| x.1.clone()).unwrap_or_default();
    match s {
        Step::RequirePacket { .. } => (vec![], vec![]),
        Step::WaitEqual { a, b, .. } => (vec![a.clone(), b.clone()], vec![]),
        Step::EmemSend { ctrl, data, .. } => (vec![ctrl.clone()], vec![ctrl.clone(), data.clone()]),
        Step::PostTail(r) | Step::Doorbell(r) => (vec![], vec![r.clone()]),
        Step::WaitNot { reg, tread, .. } => {
            let mut v = vec![reg.clone()];
            if let Some((t, _)) = tread {
                v.push(t.clone());
            }
            (v, vec![])
        }
        Step::WaitReleased { reg, .. } => (vec![reg.clone()], vec![]),
        Step::WaitLockdown { mb0, mb1, bit, .. } => (vec![mb0.clone(), mb1.clone(), bit_reg(bit)], vec![]),
        Step::RefuseNonzero { reg, .. } => (vec![reg.clone()], vec![]),
        Step::Write { reg, .. } => (vec![], vec![reg.clone()]),
        Step::RequireBit { bit, .. } => (vec![bit_reg(bit)], vec![]),
    }
}

fn step_err(s: &Step) -> Option<&String> {
    match s {
        Step::RequirePacket { err, .. }
        | Step::WaitEqual { err, .. }
        | Step::EmemSend { err, .. }
        | Step::WaitNot { err, .. }
        | Step::WaitReleased { err, .. }
        | Step::WaitLockdown { err, .. }
        | Step::RefuseNonzero { err, .. }
        | Step::RequireBit { err, .. } => Some(err),
        _ => None,
    }
}

fn budget_of(s: &Step) -> Option<&String> {
    match s {
        Step::WaitEqual { budget, .. }
        | Step::WaitNot { budget, .. }
        | Step::WaitReleased { budget, .. }
        | Step::WaitLockdown { budget, .. } => Some(budget),
        _ => None,
    }
}

/// A step-kind test, for the steps that must appear exactly once.
type IsStep = fn(&Step) -> bool;

fn pos<F: Fn(&Step) -> bool>(v: &[Step], p: F) -> Option<usize> {
    v.iter().position(p)
}

/// Every rule a contract must meet before any C is written.
pub fn check(g: &Boot) -> Result<(), Error> {
    let e = |m: String| err::<()>(0, m);
    if g.class != "gpu-boot" || g.transport != "pci-bar" {
        return e("class gpu-boot needs transport pci-bar".into());
    }
    if g.prefix != "ck_gsp" {
        return e("class gpu-boot emits the gsp_mmio.h API: prefix must be ck_gsp".into());
    }
    if g.manuals.is_empty() {
        return e("a contract needs at least one `manual` evidence line".into());
    }
    let mut names: Vec<&str> = g.consts.iter().map(|c| c.0.as_str()).collect();
    names.extend(g.regs.iter().map(|r| r.name.as_str()));
    names.extend(g.bits.iter().map(|b| b.0.as_str()));
    names.extend(g.fields.iter().map(|f| f.0.as_str()));
    for (i, n) in names.iter().enumerate() {
        if !is_ident(n) || names[..i].contains(n) {
            return e(format!("name `{n}` is not an identifier or is declared twice"));
        }
    }
    for (k, v) in &g.consts {
        if *v > u64::from(u32::MAX) {
            return e(format!("const {k}: registers are 32-bit, {v:#x} does not fit"));
        }
    }
    for w in &g.windows {
        if w.size == 0 || w.base.checked_add(w.size).is_none() || w.bar > 5 {
            return e(format!("window {}: bad bar/base/size", w.name));
        }
    }
    for (i, a) in g.windows.iter().enumerate() {
        for b in &g.windows[..i] {
            if a.name == b.name || (a.bar == b.bar && a.base < b.base + b.size && b.base < a.base + a.size) {
                return e(format!("windows {} and {} overlap or share a name", a.name, b.name));
            }
        }
    }
    for r in &g.regs {
        let Some(w) = g.windows.iter().find(|w| w.name == r.win) else {
            return e(format!("reg {}: no window {}", r.name, r.win));
        };
        if r.width != 4 || r.off % 4 != 0 || r.off + r.width > w.size {
            return e(format!("reg {}: 32-bit, aligned, inside window {}", r.name, w.name));
        }
        if !["ro", "wo", "rw"].contains(&r.access.as_str()) {
            return e(format!("reg {}: access must be ro, wo or rw", r.name));
        }
    }
    for (i, a) in g.regs.iter().enumerate() {
        if g.regs[..i].iter().any(|b| g.addr(&b.name) == g.addr(&a.name)) {
            return e(format!("reg {}: two names for one address", a.name));
        }
    }
    for (n, r, b) in &g.bits {
        if g.reg(r).is_none() || *b > 31 {
            return e(format!("bit {n}: no reg {r} or past bit 31"));
        }
    }
    for (n, r, h, _) in &g.fields {
        if g.reg(r).is_none() || *h > 31 {
            return e(format!("field {n}: no reg {r} or past bit 31"));
        }
    }
    for s in g.boot.iter().chain(&g.notify) {
        if let Some(x) = step_err(s) {
            if !API_ERRORS.contains(&x.as_str()) {
                return e(format!("error `{x}` is not one of the API's {API_ERRORS:?}"));
            }
        }
        let (rd, wr) = touches(g, s);
        for r in rd.iter().chain(&wr) {
            if g.reg(r).is_none() {
                return e(format!("register or bit `{r}` is not declared"));
            }
        }
        for r in &rd {
            if g.reg(r).unwrap().access == "wo" {
                return e(format!("reg {r} is write-only and is read"));
            }
        }
        for r in &wr {
            if g.reg(r).unwrap().access == "ro" {
                return e(format!("reg {r} is read-only and is written"));
            }
        }
        let into: Vec<&String> = match s {
            Step::WaitNot { into, tread, .. } => {
                let mut v = vec![into];
                if let Some((_, f)) = tread {
                    v.push(f);
                }
                v
            }
            Step::WaitReleased { into, .. } | Step::RefuseNonzero { into, .. } | Step::RequireBit { into, .. } => {
                vec![into]
            }
            _ => vec![],
        };
        for f in into {
            if !OUTS.contains(&f.as_str()) {
                return e(format!("`into {f}`: not a field of struct ck_gsp_mmio_out {OUTS:?}"));
            }
        }
    }
    check_boot(g)?;
    check_notify(g)
}

fn check_boot(g: &Boot) -> Result<(), Error> {
    let b = &g.boot;
    let e = |m: &str| err::<()>(0, format!("boot: {m}"));
    // verbs that appear exactly once
    let once: &[(&str, IsStep)] = &[
        ("require-packet", |s| matches!(s, Step::RequirePacket { .. })),
        ("wait-equal", |s| matches!(s, Step::WaitEqual { .. })),
        ("emem-send", |s| matches!(s, Step::EmemSend { .. })),
        ("post-tail", |s| matches!(s, Step::PostTail(_))),
        ("doorbell", |s| matches!(s, Step::Doorbell(_))),
        ("wait-not", |s| matches!(s, Step::WaitNot { .. })),
        ("wait-released", |s| matches!(s, Step::WaitReleased { .. })),
        ("wait-lockdown", |s| matches!(s, Step::WaitLockdown { .. })),
        ("refuse-nonzero", |s| matches!(s, Step::RefuseNonzero { .. })),
        ("require-bit", |s| matches!(s, Step::RequireBit { .. })),
    ];
    let mut at = Vec::new();
    for (name, f) in once {
        if b.iter().filter(|s| f(s)).count() != 1 {
            return e(&format!("`{name}` must appear exactly once"));
        }
        at.push(pos(b, f).unwrap());
    }
    let [packet, can_send, emem, tail, bell, cmd, mask, lockdown, fmc, active] = at[..] else { unreachable!() };
    if packet != 0 {
        return e("require-packet comes first (nothing touches the chip before the packet is checked)");
    }
    if active != b.len() - 1 {
        return e("require-bit comes last (the RISC-V core is checked once everything else is done)");
    }
    if !(can_send < emem) {
        return e("wait-equal before emem-send (EMEM must not be written while SEC2 still holds a message)");
    }
    if !(emem < tail && tail < bell) {
        return e("order is emem-send, post-tail, doorbell (HEAD interrupts SEC2: the packet and TAIL are first)");
    }
    if !(bell < cmd && cmd < mask && mask < lockdown && lockdown < fmc) {
        return e("order after the doorbell is wait-not, wait-released, wait-lockdown, refuse-nonzero");
    }
    // the budgets: one per wait, each poll budget of the API once, in polls[] slot order
    let budgets: Vec<&String> = b.iter().filter_map(budget_of).collect();
    if budgets.iter().map(|s| s.as_str()).collect::<Vec<_>>() != BUDGETS {
        return e(&format!("the waits use the budgets {BUDGETS:?}, once each, in that order"));
    }
    // the packet
    let Step::RequirePacket { max, .. } = &b[packet] else { unreachable!() };
    let Step::EmemSend { ctrl, data, ainc, offs, blk, per_blk, .. } = &b[emem] else { unreachable!() };
    let (Some(fo), Some(fb), Some(ai)) = (g.field(offs), g.field(blk), g.bit(ainc)) else {
        return e("emem-send CTRL DATA AINC_BIT OFFS_FIELD BLK_FIELD DW_PER_BLK: undeclared bit or field");
    };
    if fo.1 != *ctrl || fb.1 != *ctrl || ai.1 != *ctrl {
        return e("emem-send: the auto-increment bit and the OFFS/BLK fields belong to the control register");
    }
    let Some(pb) = g.konst(per_blk) else {
        return e("emem-send: DW_PER_BLK is not a constant");
    };
    let offs_n = 1u64 << (fo.2 - fo.3 + 1);
    if pb != offs_n {
        return e(&format!("emem-send: OFFS {}:{} addresses {offs_n} dwords per block, not {pb}", fo.2, fo.3));
    }
    if fo.3 != 2 {
        return e("emem-send: OFFS counts dwords from bit 2 (byte offset / 4)");
    }
    if fb.3 != fo.2 + 1 || (ai.2 >= fb.3 && ai.2 <= fb.2) || (ai.2 >= fo.3 && ai.2 <= fo.2) {
        return e("emem-send: BLK sits right above OFFS and the auto-increment bit overlaps neither");
    }
    match g.konst(max) {
        Some(m) if m > 0 && m % 4 == 0 && m / 4 <= pb << (fb.2 - fb.3 + 1) => {}
        _ => return e("require-packet MAX: a constant, a multiple of 4, inside what EMEMC can address"),
    }
    let (rc, rd_, rt, rh) = (g.reg(ctrl).unwrap(), g.reg(data).unwrap(), &b[tail], &b[bell]);
    let (Step::PostTail(t), Step::Doorbell(h)) = (rt, rh) else { unreachable!() };
    let Step::WaitEqual { a, b: bb, .. } = &g.boot[can_send] else { unreachable!() };
    if a != h || bb != t {
        return e("wait-equal HEAD TAIL compares the doorbell register (HEAD) with the post-tail register (TAIL)");
    }
    if t == h || ctrl == data {
        return e("TAIL and HEAD, EMEMC and EMEMD are different registers");
    }
    let chan = &rc.win;
    if &rd_.win != chan || &g.reg(t).unwrap().win != chan || &g.reg(h).unwrap().win != chan {
        return e("EMEMC, EMEMD, TAIL and HEAD sit in one window (the SEC2 channel)");
    }
    // nothing writes the SEC2 window after the doorbell
    for s in &b[bell + 1..] {
        if touches(g, s).1.iter().any(|r| &g.reg(r).unwrap().win == chan) {
            return e("a write to the SEC2 window after the doorbell (SEC2 owns the channel until it answers)");
        }
    }
    // the GSP window: untouched until SEC2 releases the target mask; wait-released polls a GSP register
    let Step::WaitReleased { reg: mreg, locked, mask: mmask, .. } = &b[mask] else { unreachable!() };
    let gsp = &g.reg(mreg).unwrap().win;
    if gsp == chan {
        return e("wait-released polls a register of the GSP, not of the SEC2 channel");
    }
    for s in &b[..mask] {
        let (r, w) = touches(g, s);
        if r.iter().chain(&w).any(|x| &g.reg(x).unwrap().win == gsp) {
            return e("the GSP is touched before wait-released (it is locked to SEC2 and reads 0xBADF41xx)");
        }
    }
    match (g.konst(locked), g.konst(mmask)) {
        (Some(l), Some(m)) if m != 0 && l & !m == 0 && l != 0 => {}
        _ => return e("wait-released LOCKED MASK: constants, LOCKED non-zero and inside MASK"),
    }
    let Step::WaitNot { reg: creg, val, .. } = &b[cmd] else { unreachable!() };
    if &g.reg(creg).unwrap().win != chan || g.konst(val).is_none() {
        return e("wait-not polls a SEC2 register against a constant");
    }
    let Step::WaitLockdown { mb0, mb1, bit, .. } = &b[lockdown] else { unreachable!() };
    let Some(lb) = g.bit(bit) else {
        return e("wait-lockdown: no such bit");
    };
    if &lb.1 != mreg {
        return e("wait-lockdown: the lockdown bit is in the register wait-released polls");
    }
    if mb0 == mb1 || &g.reg(mb0).unwrap().win != gsp || &g.reg(mb1).unwrap().win != gsp {
        return e("wait-lockdown MB0 MB1: two GSP mailboxes");
    }
    let Step::RefuseNonzero { reg: freg, .. } = &b[fmc] else { unreachable!() };
    if freg != mb0 {
        return e("refuse-nonzero reads the mailbox the FMC reports errors in (wait-lockdown's MB0)");
    }
    // writes: FALCON_OS-style writes only between refuse-nonzero and require-bit
    for (i, s) in b.iter().enumerate() {
        if let Step::Write { reg, val } = s {
            if i < fmc {
                return e("a write before refuse-nonzero (the FMC may still hold the core in lockdown or have failed)");
            }
            if &g.reg(reg).unwrap().win != gsp {
                return e("boot writes outside the GSP window after the doorbell");
            }
            if !WRITE_ARGS.contains(&val.as_str()) && g.konst(val).is_none() && num(val, 0).is_err() {
                return e(&format!("write {reg} {val}: a number, a constant or one of {WRITE_ARGS:?}"));
            }
        }
    }
    if !b.iter().any(|s| matches!(s, Step::Write { .. })) {
        return e("no write: GSP-RM never learns its app version (FALCON_OS)");
    }
    let Step::RequireBit { bit: ab, .. } = &b[active] else { unreachable!() };
    if g.bit(ab).map(|x| &g.reg(&x.1).unwrap().win) != Some(gsp) {
        return e("require-bit checks a bit of a GSP register");
    }
    Ok(())
}

fn check_notify(g: &Boot) -> Result<(), Error> {
    match g.notify.as_slice() {
        [Step::Write { reg, val }] if val == "0" => {
            let r = g.reg(reg).unwrap();
            let gsp = g.boot.iter().find_map(|s| match s {
                Step::WaitReleased { reg, .. } => g.reg(reg).map(|x| &x.win),
                _ => None,
            });
            if Some(&r.win) != gsp {
                return err(0, "notify: writes a GSP register (the command queue head)");
            }
            Ok(())
        }
        _ => err(0, "notify: exactly one `write REG 0`"),
    }
}

// ---------------------------------------------------------------- C emitter

fn reg_c(r: &str) -> String {
    format!("GSB_REG_{r}")
}

fn bit_c(b: &str) -> String {
    format!("GSB_BIT_{b}")
}

fn val_c(g: &Boot, v: &str) -> String {
    if g.konst(v).is_some() {
        format!("GSB_{v}")
    } else if WRITE_ARGS.contains(&v) {
        format!("in->{v}")
    } else {
        format!("{:#x}u", num(v, 0).unwrap_or(0))
    }
}

/// The loop shared by every wait: `body` reads and breaks when done; `timeout` runs before the error return.
fn wait_loop(slot: usize, budget: &str, body: &str, timeout: &str, er: &str) -> String {
    format!(
        "    for (n = 0;; n++) {{\n{body}        if (n >= in->{budget}) {{\n            o->polls[{slot}] = n;\n{timeout}            return o->stage = CK_GSP_{er};\n        }}\n        between_polls(in);\n    }}\n    o->polls[{slot}] = n;\n"
    )
}

/// The C file: gsp_mmio.h's functions, written from the contract.
pub fn source(g: &Boot, digest: &str) -> String {
    let mut o = String::new();
    o += &format!(
        "/* GENERATED by aienos-devc from {} (sha256 {digest}).\n * Do not edit: change the contract and run `aienos-devc gen gpu/contract/gb10_gsp.devc gpu/contract/gen`.\n * Device {} (class {}, transport {}). API: ../../gsp_mmio.h (the hand twin is ../../gsp_mmio.c).\n * Register offsets and bits below come from the contract only; gsp_mmio.h's CK_GSP_ values are not used.\n * Evidence: {} manual line(s) in the contract. */\n",
        g.source_name, g.device, g.class, g.transport, g.manuals.len()
    );
    o += "#include \"gsp_mmio.h\"\n\n";
    for (k, v) in &g.consts {
        o += &format!("#define GSB_{k} {v:#x}u\n");
    }
    for w in &g.windows {
        o += &format!("#define GSB_WIN_{} {:#x}u /* bar {} */\n", w.name.to_ascii_uppercase(), w.base, w.bar);
    }
    for r in &g.regs {
        o += &format!("#define {} (GSB_WIN_{} + {:#x}u)\n", reg_c(&r.name), r.win.to_ascii_uppercase(), r.off);
    }
    for (n, _, b) in &g.bits {
        o += &format!("#define {} (1u << {b})\n", bit_c(n));
    }
    for (n, _, h, l) in &g.fields {
        o += &format!("#define GSB_FLD_{n}(v) (((v) >> {l}) & {:#x}u)\n", (1u64 << (h - l + 1)) - 1);
    }
    o += "\nstatic uint32_t rd(const struct ck_gpu_regs *r, uint32_t off)\n{\n    return r->rd(r->ctx, off);\n}\n\n";
    o += "static void wr(const struct ck_gpu_regs *r, uint32_t off, uint32_t v)\n{\n    r->wr(r->ctx, off, v);\n}\n\n";
    o += "static void between_polls(const struct ck_gsp_mmio_in *in)\n{\n    if (in->pause)\n        in->pause(in->pause_ctx);\n}\n\n";
    o += "int ck_gsp_mmio_boot(const struct ck_gpu_regs *r, const struct ck_gsp_mmio_in *in, struct ck_gsp_mmio_out *o)\n{\n    uint32_t i, n;\n\n";
    o += "    for (i = 0; i < sizeof(*o); i++)\n        ((uint8_t *)o)[i] = 0;\n";
    let mut slot = 0;
    for s in &g.boot {
        match s {
            Step::RequirePacket { max, err } => {
                o += &format!("    if (!r || !in || !in->packet || in->len == 0 || in->len > GSB_{max})\n        return o->stage = CK_GSP_{err};\n")
            }
            Step::WaitEqual { a, b, budget, err } => {
                let body = format!("        uint32_t v = rd(r, {});\n        if (v == rd(r, {}))\n            break;\n", reg_c(a), reg_c(b));
                o += &wait_loop(slot, budget, &body, "", err);
                slot += 1;
            }
            Step::EmemSend { ctrl, data, ainc, offs, blk, per_blk, err } => {
                let pos = |v: &str| format!("(GSB_FLD_{blk}({v}) * GSB_{per_blk} + GSB_FLD_{offs}({v}))");
                o += &format!("    wr(r, {c}, {a});\n    uint32_t c0 = rd(r, {c});\n    uint32_t start = {p};\n", c = reg_c(ctrl), a = bit_c(ainc), p = pos("c0"));
                o += &format!("    for (i = 0; i + 4 <= in->len; i += 4)\n        wr(r, {d}, (uint32_t)in->packet[i] | (uint32_t)in->packet[i + 1] << 8 |\n                     (uint32_t)in->packet[i + 2] << 16 | (uint32_t)in->packet[i + 3] << 24);\n", d = reg_c(data));
                o += &format!("    if (i < in->len) {{\n        uint32_t v = 0;\n        for (uint32_t k = 0; i + k < in->len; k++)\n            v |= (uint32_t)in->packet[i + k] << (8 * k);\n        wr(r, {}, v);\n    }}\n", reg_c(data));
                o += &format!("    o->dwords = (in->len + 3) / 4;\n    o->ememc_after = rd(r, {});\n    if ({} - start != o->dwords)\n        return o->stage = CK_GSP_{err};\n", reg_c(ctrl), pos("o->ememc_after"));
            }
            Step::PostTail(t) => o += &format!("    wr(r, {}, o->dwords * 4 - 4);\n", reg_c(t)),
            Step::Doorbell(h) => o += &format!("    wr(r, {}, 0);\n", reg_c(h)),
            Step::WaitNot { reg, val, budget, into, tread, err } => {
                let body = format!("        o->{into} = rd(r, {});\n        if (o->{into} != GSB_{val})\n            break;\n", reg_c(reg));
                let to = tread.as_ref().map(|(t, f)| format!("            o->{f} = rd(r, {});\n", reg_c(t))).unwrap_or_default();
                o += &wait_loop(slot, budget, &body, &to, err);
                slot += 1;
            }
            Step::WaitReleased { reg, locked, mask, budget, into, err } => {
                let body = format!("        o->{into} = rd(r, {});\n        if (o->{into} != 0 && (o->{into} & GSB_{mask}) != GSB_{locked})\n            break;\n", reg_c(reg));
                o += &wait_loop(slot, budget, &body, "", err);
                slot += 1;
            }
            Step::WaitLockdown { mb0, mb1, bit, budget, err } => {
                let cfg = &g.bit(bit).unwrap().1;
                let body = format!(
                    "        int args_unread = 0;\n        o->gsp_mb0 = rd(r, {m0});\n        if (o->gsp_mb0 != 0 && o->gsp_mb0 == (uint32_t)in->fmc_args_pa) {{\n            o->gsp_mb1 = rd(r, {m1});\n            args_unread = o->gsp_mb1 == (uint32_t)(in->fmc_args_pa >> 32);\n        }}\n        if (!args_unread) {{\n            o->hwcfg2 = rd(r, {c});\n            if (!(o->hwcfg2 & {b}) || o->gsp_mb0 != 0)\n                break;\n        }}\n",
                    m0 = reg_c(mb0), m1 = reg_c(mb1), c = reg_c(cfg), b = bit_c(bit)
                );
                let to = format!("            o->gsp_mb1 = rd(r, {});\n", reg_c(mb1));
                o += &wait_loop(slot, budget, &body, &to, err);
                slot += 1;
            }
            Step::RefuseNonzero { reg, into, err } => {
                o += &format!("    o->{into} = rd(r, {});\n    if (o->{into} != 0)\n        return o->stage = CK_GSP_{err};\n", reg_c(reg))
            }
            Step::Write { reg, val } => o += &format!("    wr(r, {}, {});\n", reg_c(reg), val_c(g, val)),
            Step::RequireBit { bit, into, err } => {
                let reg = &g.bit(bit).unwrap().1;
                o += &format!("    o->{into} = rd(r, {});\n    if (!(o->{into} & {}))\n        return o->stage = CK_GSP_{err};\n", reg_c(reg), bit_c(bit))
            }
        }
    }
    o += "    return o->stage = CK_GSP_OK;\n}\n\n";
    o += "void ck_gsp_mmio_notify(const struct ck_gpu_regs *r)\n{\n";
    for s in &g.notify {
        if let Step::Write { reg, val } = s {
            o += &format!("    wr(r, {}, {});\n", reg_c(reg), val_c(g, val));
        }
    }
    o += "}\n";
    o
}

/// The documentation page.
pub fn markdown(g: &Boot, digest: &str, c_lines: usize) -> String {
    let mut o = format!(
        "# {} (generated from {})\n\nContract sha256 `{digest}`. Class `{}`, transport `{}`.\n{} statements compile to {c_lines} lines of C with the API of `native/kernel/gpu/gsp_mmio.h`.\n\n",
        g.device, g.source_name, g.class, g.transport, g.stmt_count
    );
    o += "## What the compiler refuses (class gpu-boot)\n\n";
    for r in [
        "any chip access before the packet length is checked against the EMEM channel",
        "writing EMEM before the SEC2 channel is seen empty (HEAD equal to TAIL)",
        "the doorbell (HEAD) before the packet and TAIL are written, or a SEC2 write after it",
        "an EMEM layout where the dword offset field and the dwords-per-block constant disagree",
        "touching any GSP register before SEC2 releases the GSP target mask",
        "a write to the GSP (FALCON_OS) before the FMC error mailbox is checked",
        "a wait with no poll budget, or budgets out of the API's slot order",
        "writing a read-only register or reading a write-only one",
    ] {
        o += &format!("- {r}\n");
    }
    o += "\n## Steps\n\n";
    for s in &g.boot {
        o += &format!("- {s:?}\n");
    }
    o += "\n## Evidence\n\n";
    for (d, w, q) in &g.manuals {
        o += &format!("- {d}, `{w}`: \"{q}\"\n");
    }
    o += "\n## Proof on machines\n\n";
    for p in &g.proven {
        o += &format!("- {}\n", p.join(" "));
    }
    o += "\n## Registers\n\n";
    for r in &g.regs {
        let w = g.windows.iter().find(|w| w.name == r.win).unwrap();
        o += &format!("- {} at bar {} + {:#x} ({} bytes, {})\n", r.name, w.bar, w.base + r.off, r.width, r.access);
    }
    o += "\n## Bits and fields\n\n";
    for (n, r, b) in &g.bits {
        o += &format!("- {n}: {r} bit {b}\n");
    }
    for (n, r, h, l) in &g.fields {
        o += &format!("- {n}: {r} bits {h}:{l}\n");
    }
    o
}

#[cfg(test)]
mod tests {
    use super::*;

    const GOOD: &str = include_str!("../tests/gb10_gsp.devc");

    #[test]
    fn good_boot_contract_parses_and_emits_the_api() {
        let g = parse(GOOD, "gb10_gsp.devc").unwrap();
        let c = source(&g, "x");
        for f in ["ck_gsp_mmio_boot(", "ck_gsp_mmio_notify("] {
            assert!(c.contains(f), "{f} missing");
        }
        assert!(!c.contains("CK_GSP_SEC2_") && !c.contains("CK_GSP_FLCN_"), "uses the hand header's offsets");
        assert_eq!(g.addr("CPUCTL"), 0x111388);
        assert_eq!(g.addr("QTAIL0"), 0x840c04);
    }

    /// Each edit breaks one safety rule; the checker must refuse every one.
    #[test]
    fn checker_refuses_unsafe_boot_contracts() {
        let cases: &[(&str, &str)] = &[
            ("  wait-equal QHEAD0 QTAIL0 budget polls_can_send else E_BUSY\n", ""),
            ("  post-tail QTAIL0\n  doorbell QHEAD0\n", "  doorbell QHEAD0\n  post-tail QTAIL0\n"),
            ("  doorbell QHEAD0\n", "  doorbell QHEAD0\n  doorbell QHEAD0\n"),
            ("  write FALCON_OS app_version\n  require-bit", "  require-bit"),
            ("  refuse-nonzero GSP_MAILBOX0 into gsp_mb0 else E_FMC\n  write FALCON_OS app_version\n", "  write FALCON_OS app_version\n  refuse-nonzero GSP_MAILBOX0 into gsp_mb0 else E_FMC\n"),
            ("  require-packet EMEM_CHANNEL_BYTES else E_ARG\n  wait-equal QHEAD0 QTAIL0 budget polls_can_send else E_BUSY\n", "  wait-equal QHEAD0 QTAIL0 budget polls_can_send else E_BUSY\n  require-packet EMEM_CHANNEL_BYTES else E_ARG\n"),
            ("const EMEM_DW_PER_BLK 64", "const EMEM_DW_PER_BLK 32"),
            ("const EMEM_CHANNEL_BYTES 1024", "const EMEM_CHANNEL_BYTES 1022"),
            ("field BLK EMEMC0 15:8", "field BLK EMEMC0 16:9"),
            ("bit AINCW EMEMC0 24", "bit AINCW EMEMC0 9"),
            ("reg gsp HWCFG2 0x0f4 4 ro", "reg sec2 HWCFG2 0x0f4 4 ro"),
            ("reg gsp HWCFG2 0x0f4 4 ro", "reg gsp HWCFG2 0x0f4 4 wo"),
            ("budget polls_cmd", "budget polls_mask"),
            ("  wait-lockdown GSP_MAILBOX0 GSP_MAILBOX1 LOCKDOWN", "  wait-lockdown GSP_MAILBOX1 GSP_MAILBOX0 LOCKDOWN"),
            ("  write GSP_QHEAD0 0\n", "  write QHEAD0 0\n"),
            ("const PRIV_LOCKED 0xBADF4100", "const PRIV_LOCKED 0xBADF4101"),
            ("reg gsp GSP_QHEAD0 0xc00 4 rw", "reg gsp GSP_QHEAD0 0x040 4 rw"),
            ("  wait-released HWCFG2", "  write CPUCTL 0\n  wait-released HWCFG2"),
            ("into cpuctl", "into stage"),
            ("else E_RISCV", "else E_OOPS"),
        ];
        assert!(parse(GOOD, "good").is_ok());
        for (from, to) in cases {
            assert!(GOOD.contains(from), "case text not in contract: {from}");
            let bad = GOOD.replacen(from, to, 1);
            assert!(parse(&bad, "bad").is_err(), "checker accepted: {from:?} -> {to:?}");
        }
    }
}
