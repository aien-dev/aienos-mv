//! Class `gpu-chan` (L332 rung 1): a GPU command channel as a device contract.
//!
//! Same lexical format as devc 1 (`contract::words`), its own statements:
//! `window NAME bar N base B size S`, `reg`, `format`, `buffer`, and three step
//! blocks (`create`, `submit`, `teardown`) from a closed list of verbs. The checker
//! refuses unsafe orders (doorbell before the put pointer is published, no fence
//! between them, a ring with no room check, CPU-written GPU-read memory that is
//! cached, a bound that does not fit its hardware field). The emitter writes C
//! with the API of native/kernel/gpu/gpu_chan.h (L193), so the hand-written twin
//! and the generated one run the same tests.

use crate::contract::{err, is_ident, num, words, Error};

#[derive(Debug, Clone)]
pub struct Field {
    pub name: String,
    pub hi: u32,
    pub lo: u32,
    pub place: bool,
}

#[derive(Debug, Clone)]
pub struct Format {
    pub name: String,
    pub bits: u32,
    pub fields: Vec<Field>,
}

#[derive(Debug, Clone)]
pub struct Buffer {
    pub name: String,
    /// `Ok(const name)` or `Err(bytes per ring entry)`
    pub size: Result<String, u64>,
    pub cache: String,
    pub flags: String,
}

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
    Refuse { arg: String, op: String, rhs: String, err: String },
    RefuseEntries { min: String, max: String, err: String },
    Alloc { bufs: Vec<String>, err: String },
    Zero(Vec<String>),
    StorePdb { buf: String, word: String, fmt: String, sets: Vec<(String, String)> },
    Token { fmt: String, sets: Vec<(String, String)> },
    RunlistUserd { fmt: String, buf: String, err: String },
    Fence,
    Bind { err: String },
    RequireOpen(String),
    RequireBuffer(String),
    RequireSpan { max: String, err: String },
    RequireVa { bits: String, err: String },
    RequireRoom(String),
    RingWrite { fmt: String, sets: Vec<(String, String)> },
    Publish { buf: String, off: String },
    Doorbell { reg: String, fmt: String },
    Disable,
    PollIdle,
    Release(Vec<String>),
}

#[derive(Debug, Default, Clone)]
pub struct Gpu {
    pub source_name: String,
    pub device: String,
    pub prefix: String,
    pub class: String,
    pub transport: String,
    pub ring_kind: String,
    pub manuals: Vec<(String, String, String)>,
    pub proven: Vec<Vec<String>>,
    pub consts: Vec<(String, u64)>,
    pub windows: Vec<Win>,
    pub regs: Vec<Reg>,
    pub formats: Vec<Format>,
    pub buffers: Vec<Buffer>,
    pub create: Vec<Step>,
    pub submit: Vec<Step>,
    pub teardown: Vec<Step>,
    pub stmt_count: usize,
}

/// The error names the gpu_chan.h API has (enum ck_gpu_chan_status).
const API_ERRORS: &[&str] = &["EINVAL", "ENOMEM", "ESTATE", "EFULL", "EBIND", "ETIMEDOUT"];
/// Arguments of ck_gpu_chan_create a `refuse` may test.
const CREATE_ARGS: &[&str] = &["chid", "runlist_id"];

/// True when the source declares `class gpu-chan` (main.rs dispatches on it).
pub fn is_gpu(src: &str) -> bool {
    src.lines().any(|l| {
        let w: Vec<&str> = l.split('#').next().unwrap_or("").split_whitespace().collect();
        w == ["class", "gpu-chan"]
    })
}

fn sets(w: &[String], ln: usize) -> Result<Vec<(String, String)>, Error> {
    let mut v = Vec::new();
    for x in w {
        let Some((k, val)) = x.split_once('=') else {
            return err(ln, format!("expected FIELD=value, got `{x}`"));
        };
        if !is_ident(k) || !(is_ident(val) || num(val, ln).is_ok()) {
            return err(ln, format!("bad assignment `{x}`"));
        }
        v.push((k.to_string(), val.to_string()));
    }
    Ok(v)
}

fn else_err(w: &[String], at: usize, ln: usize) -> Result<String, Error> {
    if w.len() != at + 2 || w[at] != "else" {
        return err(ln, format!("`{}` must end with `else ERROR`", w[0]));
    }
    Ok(w[at + 1].clone())
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

fn parse_step(w: &[String], ln: usize, block: &str) -> Result<Step, Error> {
    let n = w.len();
    let at = |i: usize| w.get(i).cloned().unwrap_or_default();
    let list = |from: usize, to: usize| w[from..to].to_vec();
    let st = match (block, w[0].as_str()) {
        ("create", "refuse") if n == 6 => Step::Refuse { arg: at(1), op: at(2), rhs: at(3), err: else_err(w, 4, ln)? },
        ("create", "refuse-entries") if n == 6 && w[1] == "pow2" => {
            Step::RefuseEntries { min: at(2), max: at(3), err: else_err(w, 4, ln)? }
        }
        ("create", "alloc") if n >= 4 => Step::Alloc { bufs: list(1, n - 2), err: else_err(w, n - 2, ln)? },
        ("create", "zero") if n >= 2 => Step::Zero(list(1, n)),
        ("create", "store-pdb") if n >= 5 => {
            Step::StorePdb { buf: at(1), word: at(2), fmt: at(3), sets: sets(&w[4..], ln)? }
        }
        ("create", "token") if n >= 3 => Step::Token { fmt: at(1), sets: sets(&w[2..], ln)? },
        ("create", "runlist-userd") if n == 5 => {
            Step::RunlistUserd { fmt: at(1), buf: at(2), err: else_err(w, 3, ln)? }
        }
        ("create" | "submit", "fence") if n == 1 => Step::Fence,
        ("create", "bind") if n == 3 => Step::Bind { err: else_err(w, 1, ln)? },
        ("submit" | "teardown", "require-open") if n == 3 => Step::RequireOpen(else_err(w, 1, ln)?),
        ("submit", "require-buffer") if n == 3 => Step::RequireBuffer(else_err(w, 1, ln)?),
        ("submit", "require-span") if n == 4 => Step::RequireSpan { max: at(1), err: else_err(w, 2, ln)? },
        ("submit", "require-va") if n == 4 => Step::RequireVa { bits: at(1), err: else_err(w, 2, ln)? },
        ("submit", "require-room") if n == 3 => Step::RequireRoom(else_err(w, 1, ln)?),
        ("submit", "ring-write") if n >= 3 => Step::RingWrite { fmt: at(1), sets: sets(&w[2..], ln)? },
        ("submit", "publish") if n == 3 => Step::Publish { buf: at(1), off: at(2) },
        ("submit", "doorbell") if n == 3 => Step::Doorbell { reg: at(1), fmt: at(2) },
        ("teardown", "disable") if n == 1 => Step::Disable,
        ("teardown", "poll-idle") if n == 1 => Step::PollIdle,
        ("teardown", "release") if n >= 2 => Step::Release(list(1, n)),
        (b, o) => return err(ln, format!("`{o}` is not a step of `{b}` (or has the wrong number of words)")),
    };
    Ok(st)
}

pub fn parse(src: &str, source_name: &str) -> Result<Gpu, Error> {
    let mut g = Gpu { source_name: source_name.to_string(), ..Default::default() };
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
            match b.as_str() {
                "create" => g.create.push(st),
                "submit" => g.submit.push(st),
                _ => g.teardown.push(st),
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
            "ring-kind" => g.ring_kind = s(1).into(),
            "manual" if w.len() == 4 => g.manuals.push((s(1).into(), s(2).into(), s(3).into())),
            "proven" if w.len() == 6 => g.proven.push(w[1..].to_vec()),
            "const" if w.len() == 3 => {
                if !is_ident(s(1)) {
                    return err(ln, format!("bad name `{}`", s(1)));
                }
                g.consts.push((s(1).into(), num(s(2), ln)?));
            }
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
            "format" if w.len() >= 5 => {
                let mut f = Format { name: s(1).into(), bits: num(s(2), ln)? as u32, fields: Vec::new() };
                let mut k = 3;
                while k < w.len() {
                    let (hi, lo) = range(s(k + 1), ln)?;
                    let place = s(k + 2) == "place";
                    f.fields.push(Field { name: s(k).into(), hi, lo, place });
                    k += if place { 3 } else { 2 };
                }
                g.formats.push(f);
            }
            "buffer" if w.len() == 5 || w.len() == 6 => {
                let (size, rest) = if s(2) == "entries" {
                    let Some(x) = s(3).strip_prefix('x') else {
                        return err(ln, "`entries` needs xBYTES");
                    };
                    (Err(num(x, ln)?), 4)
                } else {
                    (Ok(s(2).to_string()), 3)
                };
                if w.len() != rest + 2 {
                    return err(ln, "`buffer NAME size cache flags`");
                }
                g.buffers.push(Buffer { name: s(1).into(), size, cache: s(rest).into(), flags: s(rest + 1).into() });
            }
            "create" | "submit" | "teardown" if w.len() == 1 => block = Some(s(0).into()),
            o => return err(ln, format!("unknown or malformed statement `{o}` for class gpu-chan")),
        }
    }
    if block.is_some() {
        return err(0, "a step block has no `end`");
    }
    check(&g)?;
    Ok(g)
}

impl Gpu {
    fn konst(&self, n: &str) -> Option<u64> {
        self.consts.iter().find(|(k, _)| k == n).map(|(_, v)| *v)
    }
    fn format(&self, n: &str) -> Option<&Format> {
        self.formats.iter().find(|f| f.name == n)
    }
    fn buffer(&self, n: &str) -> Option<&Buffer> {
        self.buffers.iter().find(|b| b.name == n)
    }
    fn reg(&self, n: &str) -> Option<&Reg> {
        self.regs.iter().find(|r| r.name == n)
    }
    /// Value of a number or a declared constant.
    fn val(&self, v: &str) -> Option<u64> {
        self.konst(v).or_else(|| num(v, 0).ok())
    }
    fn allocated(&self) -> Vec<String> {
        self.create
            .iter()
            .flat_map(|s| match s {
                Step::Alloc { bufs, .. } => bufs.clone(),
                _ => vec![],
            })
            .collect()
    }
}

fn mask(f: &Field) -> u64 {
    let w = f.hi - f.lo + 1;
    let m = if w >= 64 { u64::MAX } else { (1u64 << w) - 1 };
    m << f.lo
}
fn field_max(f: &Field) -> u64 {
    mask(f) >> f.lo
}

fn pos<F: Fn(&Step) -> bool>(v: &[Step], p: F) -> Option<usize> {
    v.iter().position(p)
}

/// Every rule a contract must meet before any C is written.
pub fn check(g: &Gpu) -> Result<(), Error> {
    let e = |m: String| err::<()>(0, m);
    if g.class != "gpu-chan" || g.transport != "pci-bar" || g.ring_kind != "gpfifo" {
        return e("class gpu-chan needs transport pci-bar and ring-kind gpfifo".into());
    }
    if g.prefix != "ck_gpu" {
        return e("class gpu-chan emits the gpu_chan.h API: prefix must be ck_gpu".into());
    }
    if g.manuals.is_empty() {
        return e("a contract needs at least one `manual` evidence line".into());
    }
    // names unique across consts, formats, buffers, regs
    let mut names: Vec<&str> = g.consts.iter().map(|c| c.0.as_str()).collect();
    names.extend(g.formats.iter().map(|f| f.name.as_str()));
    names.extend(g.buffers.iter().map(|b| b.name.as_str()));
    names.extend(g.regs.iter().map(|r| r.name.as_str()));
    for (i, n) in names.iter().enumerate() {
        if !is_ident(n) || names[..i].contains(n) {
            return e(format!("name `{n}` is not an identifier or is declared twice"));
        }
    }
    for w in &g.windows {
        if w.size == 0 || w.base.checked_add(w.size).is_none() || w.bar > 5 {
            return e(format!("window {}: bad bar/base/size", w.name));
        }
    }
    for r in &g.regs {
        let Some(w) = g.windows.iter().find(|w| w.name == r.win) else {
            return e(format!("reg {}: no window {}", r.name, r.win));
        };
        if ![1, 2, 4, 8].contains(&r.width) || r.off % r.width != 0 || r.off + r.width > w.size {
            return e(format!("reg {}: offset/width outside window {} or unaligned", r.name, w.name));
        }
        if !["ro", "wo", "rw"].contains(&r.access.as_str()) {
            return e(format!("reg {}: access must be ro, wo or rw", r.name));
        }
    }
    for f in &g.formats {
        if f.bits != 32 && f.bits != 64 {
            return e(format!("format {}: 32 or 64 bits", f.name));
        }
        let mut used = 0u64;
        for x in &f.fields {
            if x.hi >= f.bits {
                return e(format!("format {}: field {} past bit {}", f.name, x.name, f.bits - 1));
            }
            if used & mask(x) != 0 {
                return e(format!("format {}: field {} overlaps another", f.name, x.name));
            }
            used |= mask(x);
        }
    }
    for b in &g.buffers {
        if let Ok(c) = &b.size {
            if g.konst(c).is_none() {
                return e(format!("buffer {}: size `{c}` is not a constant", b.name));
            }
        }
        if !["cached", "uncached"].contains(&b.cache.as_str())
            || !["none", "gpu-ro", "host-polled"].contains(&b.flags.as_str())
        {
            return e(format!("buffer {}: cache cached|uncached, flags none|gpu-ro|host-polled", b.name));
        }
        // spark-hardware-manual 4.2 / gb10-completion-flag lesson: memory the CPU writes while the GPU
        // reads it, or that the CPU polls, must not sit in the GPU's cache.
        if b.flags != "none" && b.cache != "uncached" {
            return e(format!("buffer {}: {} memory must be uncached (the GPU would read a stale copy)", b.name, b.flags));
        }
    }
    for st in g.create.iter().chain(&g.submit).chain(&g.teardown) {
        let er = match st {
            Step::Refuse { err, .. }
            | Step::RefuseEntries { err, .. }
            | Step::Alloc { err, .. }
            | Step::RunlistUserd { err, .. }
            | Step::Bind { err }
            | Step::RequireSpan { err, .. }
            | Step::RequireVa { err, .. } => Some(err),
            Step::RequireOpen(x) | Step::RequireBuffer(x) | Step::RequireRoom(x) => Some(x),
            _ => None,
        };
        if let Some(x) = er {
            if !API_ERRORS.contains(&x.as_str()) {
                return e(format!("error `{x}` is not one of the API's {API_ERRORS:?}"));
            }
        }
    }
    // verbs that must appear exactly once in their block (a second bind or doorbell would run before its fence)
    for (blk, v) in [("create", &g.create), ("submit", &g.submit), ("teardown", &g.teardown)] {
        let once: &[fn(&Step) -> bool] = &[
            |s| matches!(s, Step::Bind { .. }),
            |s| matches!(s, Step::Token { .. }),
            |s| matches!(s, Step::RunlistUserd { .. }),
            |s| matches!(s, Step::RingWrite { .. }),
            |s| matches!(s, Step::Publish { .. }),
            |s| matches!(s, Step::Doorbell { .. }),
            |s| matches!(s, Step::RequireOpen(_)),
            |s| matches!(s, Step::Disable),
            |s| matches!(s, Step::PollIdle),
            |s| matches!(s, Step::Release(_)),
            |s| matches!(s, Step::StorePdb { .. }),
        ];
        for f in once {
            if v.iter().filter(|s| f(s)).count() > 1 {
                return e(format!("{blk}: a step that must appear once appears twice"));
            }
        }
    }
    check_create(g)?;
    check_submit(g)?;
    check_teardown(g)
}

fn check_format_sets(g: &Gpu, fmt: &str, sets: &[(String, String)], args: &[&str]) -> Result<(), Error> {
    let Some(f) = g.format(fmt) else {
        return err(0, format!("no format {fmt}"));
    };
    for (k, v) in sets {
        let Some(x) = f.fields.iter().find(|x| &x.name == k) else {
            return err(0, format!("format {fmt} has no field {k}"));
        };
        match g.val(v) {
            Some(n) if !x.place && n > field_max(x) => {
                return err(0, format!("{fmt}.{k}={v} does not fit {}:{}", x.hi, x.lo))
            }
            Some(_) => {}
            None if args.contains(&v.as_str()) => {}
            None => return err(0, format!("{fmt}.{k}: `{v}` is not a constant or an argument {args:?}")),
        }
    }
    Ok(())
}

fn check_create(g: &Gpu) -> Result<(), Error> {
    let c = &g.create;
    let e = |m: &str| err::<()>(0, format!("create: {m}"));
    let first_alloc = pos(c, |s| matches!(s, Step::Alloc { .. })).ok_or(Error { line: 0, msg: "create: no alloc".into() })?;
    // every refusal happens before memory is taken
    for (i, s) in c.iter().enumerate() {
        if matches!(s, Step::Refuse { .. } | Step::RefuseEntries { .. }) && i > first_alloc {
            return e("refusals must come before alloc");
        }
        if let Step::Refuse { arg, op, rhs, .. } = s {
            if !CREATE_ARGS.contains(&arg.as_str()) || !(op == ">" || op == ">=") || g.konst(rhs).is_none() {
                return e("refuse ARG >|>= CONST, ARG one of chid runlist_id");
            }
        }
        if let Step::RefuseEntries { min, max, .. } = s {
            match (g.konst(min), g.konst(max)) {
                (Some(a), Some(b)) if a >= 2 && a <= b && b.is_power_of_two() && a.is_power_of_two() => {}
                _ => return e("refuse-entries pow2 MIN MAX: powers of two, 2 <= MIN <= MAX"),
            }
        }
    }
    if pos(c, |s| matches!(s, Step::RefuseEntries { .. })).is_none() {
        return e("the ring size needs refuse-entries (a ring that is not a power of two wraps wrong)");
    }
    let alloc = g.allocated();
    for (i, b) in alloc.iter().enumerate() {
        if g.buffer(b).is_none() || alloc[..i].contains(b) {
            return e(&format!("alloc: `{b}` undeclared or allocated twice"));
        }
    }
    if alloc.len() != g.buffers.len() {
        return e("every declared buffer is allocated exactly once");
    }
    if g.buffers.iter().filter(|b| b.size.is_err()).count() != 1 {
        return e("exactly one buffer is sized by the ring entries (the GPFIFO)");
    }
    // zero before any store; everything allocated is zeroed
    let zeroed: Vec<String> = c.iter().flat_map(|s| if let Step::Zero(v) = s { v.clone() } else { vec![] }).collect();
    for b in &alloc {
        if !zeroed.contains(b) {
            return e(&format!("buffer {b} is never zeroed (the GPU would read old memory)"));
        }
    }
    let last_zero = c.iter().rposition(|s| matches!(s, Step::Zero(_))).unwrap_or(0);
    let mut token_fmt = None;
    for (i, s) in c.iter().enumerate() {
        match s {
            Step::StorePdb { buf, word, fmt, sets } => {
                if i < last_zero {
                    return e("store-pdb before zero: the zero would erase it");
                }
                let (Some(b), Some(f), Some(wd)) = (g.buffer(buf), g.format(fmt), g.val(word)) else {
                    return e("store-pdb BUF WORD FORMAT: undeclared name");
                };
                let size = b.size.as_ref().ok().and_then(|k| g.konst(k)).unwrap_or(0);
                if (wd + 1) * 4 + 4 > size || f.bits != 64 {
                    return e("store-pdb writes two 32-bit words (a 64-bit format) inside the buffer");
                }
                if f.fields.iter().filter(|x| x.place).count() != 1 {
                    return e("store-pdb: the format has exactly one `place` field (the page directory address)");
                }
                check_format_sets(g, fmt, sets, &[])?;
            }
            Step::Token { fmt, sets } => {
                check_format_sets(g, fmt, sets, CREATE_ARGS)?;
                // each argument's refusal bound must fit the field it lands in
                for (k, v) in sets {
                    let f = g.format(fmt).unwrap();
                    let x = f.fields.iter().find(|x| &x.name == k).unwrap();
                    let lim = c.iter().find_map(|s| match s {
                        Step::Refuse { arg, op, rhs, .. } if arg == v => {
                            g.konst(rhs).map(|r| if op == ">" { r } else { r.saturating_sub(1) })
                        }
                        _ => None,
                    });
                    match lim {
                        Some(m) if m <= field_max(x) => {}
                        Some(m) => {
                            return e(&format!("{v} may reach {m:#x} but {fmt}.{k} holds only {:#x}", field_max(x)))
                        }
                        None if CREATE_ARGS.contains(&v.as_str()) => {
                            return e(&format!("{v} lands in {fmt}.{k} but has no refuse bound"))
                        }
                        None => {}
                    }
                }
                if sets.len() != 2 || sets[0].1 != "runlist_id" && sets[1].1 != "runlist_id" {
                    return e("token sets exactly the runlist id and the channel id fields");
                }
                token_fmt = Some(fmt.clone());
            }
            Step::RunlistUserd { fmt, buf, .. } => {
                let (Some(f), Some(_)) = (g.format(fmt), g.buffer(buf)) else {
                    return e("runlist-userd FORMAT BUF: undeclared");
                };
                let mut pl: Vec<&Field> = f.fields.iter().filter(|x| x.place).collect();
                pl.sort_by_key(|x| x.lo);
                if pl.is_empty() || f.fields.len() != pl.len() || pl.windows(2).any(|p| p[0].hi + 1 != p[1].lo) {
                    return e("runlist-userd: the format is address bits only, contiguous (no address bit dropped)");
                }
            }
            _ => {}
        }
    }
    if token_fmt.is_none() {
        return e("no token: the doorbell would have nothing to ring");
    }
    let n = c.len();
    if n < 2 || c[n - 1] != (Step::Bind { err: match &c[n - 1] { Step::Bind { err } => err.clone(), _ => String::new() } })
        || c[n - 2] != Step::Fence
    {
        return e("must end with `fence` then `bind`: the device may read everything written before it");
    }
    Ok(())
}

fn check_submit(g: &Gpu) -> Result<(), Error> {
    let s = &g.submit;
    let e = |m: &str| err::<()>(0, format!("submit: {m}"));
    if !matches!(s.first(), Some(Step::RequireOpen(_))) {
        return e("starts with require-open");
    }
    let ring = pos(s, |x| matches!(x, Step::RingWrite { .. })).ok_or(Error { line: 0, msg: "submit: no ring-write".into() })?;
    let publish = pos(s, |x| matches!(x, Step::Publish { .. })).ok_or(Error { line: 0, msg: "submit: no publish".into() })?;
    let bell = pos(s, |x| matches!(x, Step::Doorbell { .. })).ok_or(Error { line: 0, msg: "submit: no doorbell".into() })?;
    for (name, p) in [
        ("require-buffer", pos(s, |x| matches!(x, Step::RequireBuffer(_)))),
        ("require-span", pos(s, |x| matches!(x, Step::RequireSpan { .. }))),
        ("require-va", pos(s, |x| matches!(x, Step::RequireVa { .. }))),
        ("require-room", pos(s, |x| matches!(x, Step::RequireRoom(_)))),
    ] {
        match p {
            Some(i) if i < ring => {}
            _ => return e(&format!("{name} must come before ring-write (else the ring takes a bad or overflowing entry)")),
        }
    }
    if !(ring < publish && publish < bell && bell == s.len() - 1) {
        return e("order is ring-write, publish, doorbell, and the doorbell is last");
    }
    if !s[ring..publish].contains(&Step::Fence) || !s[publish..bell].contains(&Step::Fence) {
        return e("a fence must separate ring-write from publish and publish from the doorbell (the GPU may fetch early)");
    }
    let Step::RingWrite { fmt, sets } = &s[ring] else { unreachable!() };
    check_format_sets(g, fmt, sets, &["va", "nwords"])?;
    let f = g.format(fmt).unwrap();
    let field_of = |arg: &str| sets.iter().find(|(_, v)| v == arg).and_then(|(k, _)| f.fields.iter().find(|x| &x.name == k));
    let (Some(fva), Some(fn_)) = (field_of("va"), field_of("nwords")) else {
        return e("ring-write sets one field to va and one to nwords");
    };
    if !fva.place || fn_.place {
        return e("ring-write: the va field is `place`, the nwords field is shifted");
    }
    for x in s {
        match x {
            Step::RequireVa { bits, .. } if g.val(bits) != Some(u64::from(fva.hi) + 1) => {
                return e(&format!("require-va {bits} must equal the address field width ({} bits)", fva.hi + 1))
            }
            Step::RequireSpan { max, .. } if g.val(max) != Some(field_max(fn_)) => {
                return e(&format!("require-span {max} must equal the length field's max {:#x}", field_max(fn_)))
            }
            _ => {}
        }
    }
    if f.bits != 64 {
        return e("ring-write: GPFIFO entries are 64-bit");
    }
    let Step::Publish { buf, off } = &s[publish] else { unreachable!() };
    let ok = g.buffer(buf).map(|b| b.flags == "host-polled").unwrap_or(false);
    let size = g.buffer(buf).and_then(|b| b.size.as_ref().ok()).and_then(|k| g.konst(k)).unwrap_or(0);
    match g.val(off) {
        Some(o) if ok && o % 4 == 0 && o + 4 <= size => {}
        _ => return e("publish BUF OFF: a host-polled buffer, aligned 32-bit word inside it"),
    }
    let Step::Doorbell { reg, fmt: tf } = &s[bell] else { unreachable!() };
    match g.reg(reg) {
        Some(r) if r.access != "ro" && r.width == 4 => {}
        _ => return e("doorbell REG: a writable 32-bit register"),
    }
    let tok = g.create.iter().find_map(|x| if let Step::Token { fmt, .. } = x { Some(fmt) } else { None });
    if tok != Some(tf) {
        return e("doorbell rings the token format built in create");
    }
    Ok(())
}

fn check_teardown(g: &Gpu) -> Result<(), Error> {
    let t = &g.teardown;
    let e = |m: &str| err::<()>(0, format!("teardown: {m}"));
    if !matches!(t.first(), Some(Step::RequireOpen(_))) {
        return e("starts with require-open");
    }
    let d = pos(t, |x| *x == Step::Disable);
    let p = pos(t, |x| *x == Step::PollIdle);
    let r = pos(t, |x| matches!(x, Step::Release(_)));
    match (d, p, r) {
        (Some(d), Some(p), Some(r)) if d < p && p < r && r == t.len() - 1 => {}
        _ => return e("order is disable, poll-idle, release (memory is freed only once the GPU is idle)"),
    }
    let Some(Step::Release(v)) = t.last() else { unreachable!() };
    let mut a = g.allocated();
    let mut b = v.clone();
    a.sort();
    b.sort();
    if a != b {
        return e("release names every buffer create allocated, once");
    }
    Ok(())
}

// ---------------------------------------------------------------- C emitter

fn up(s: &str) -> String {
    s.to_ascii_uppercase()
}

fn enc_name(f: &str) -> String {
    format!("gch_enc_{}", f.to_ascii_lowercase())
}

/// Static encoder for a format: one argument per field, in declaration order.
fn encoder(f: &Format) -> String {
    let ty = if f.bits == 64 { "uint64_t" } else { "uint32_t" };
    let args: Vec<String> = f.fields.iter().map(|x| format!("uint64_t {}", x.name.to_ascii_lowercase())).collect();
    let mut o = format!("static inline {ty} {}({})\n{{\n    return ({ty})(", enc_name(&f.name), args.join(", "));
    let terms: Vec<String> = f
        .fields
        .iter()
        .map(|x| {
            let a = x.name.to_ascii_lowercase();
            if x.place {
                format!("({a} & {:#x}ull)", mask(x))
            } else {
                format!("(({a} & {:#x}ull) << {})", field_max(x), x.lo)
            }
        })
        .collect();
    o += &terms.join(" | ");
    o += ");\n}\n";
    o
}

/// Arguments for a format encoder call: fields set by `sets`, the rest from `rest`.
fn call(f: &Format, sets: &[(String, String)], rest: &dyn Fn(&Field) -> String) -> String {
    let a: Vec<String> = f
        .fields
        .iter()
        .map(|x| match sets.iter().find(|(k, _)| *k == x.name) {
            Some((_, v)) if is_ident(v) && v.chars().next().unwrap().is_ascii_uppercase() => format!("GCH_{v}"),
            Some((_, v)) => v.clone(),
            None => rest(x),
        })
        .collect();
    format!("{}({})", enc_name(&f.name), a.join(", "))
}

fn buf_field(b: &str) -> String {
    b.to_ascii_lowercase()
}

fn size_expr(b: &Buffer) -> String {
    match &b.size {
        Ok(k) => format!("GCH_{k}"),
        Err(per) => format!("(uint64_t)entries * {per}u"),
    }
}

fn cache_c(b: &Buffer) -> &'static str {
    if b.cache == "uncached" { "CK_GMEM_UNCACHED" } else { "CK_GMEM_CACHED" }
}

fn flags_c(b: &Buffer) -> &'static str {
    match b.flags.as_str() {
        "gpu-ro" => "CK_GMEM_GPU_RO",
        "host-polled" => "CK_GMEM_HOST_POLLED",
        _ => "0",
    }
}

/// The C file: gpu_chan.h's functions, written from the contract.
pub fn source(g: &Gpu, digest: &str) -> String {
    let mut o = String::new();
    o += &format!(
        "/* GENERATED by aienos-devc from {} (sha256 {digest}).\n * Do not edit: change the contract and run `make -C native/kernel gpu-contract-gen`.\n * Device {} (class {}, transport {}, ring {}). API: ../../gpu_chan.h (L193 twin).\n * Evidence: {} manual line(s) in the contract. */\n",
        g.source_name, g.device, g.class, g.transport, g.ring_kind, g.manuals.len()
    );
    o += "#include \"gpu_chan.h\"\n\n";
    for (k, v) in &g.consts {
        o += &format!("#define GCH_{k} {v:#x}ull\n");
    }
    for w in &g.windows {
        o += &format!("#define GCH_WIN_{} {:#x}u /* bar {} */\n", up(&w.name), w.base, w.bar);
    }
    for r in &g.regs {
        o += &format!("#define GCH_REG_{}_OFF (GCH_WIN_{} + {:#x}u)\n", r.name, up(&r.win), r.off);
    }
    o += "\n";
    for f in &g.formats {
        o += &encoder(f);
        o += "\n";
    }
    let tok = g.create.iter().find_map(|s| if let Step::Token { fmt, sets } = s { Some((fmt, sets)) } else { None }).unwrap();
    let tf = g.format(tok.0).unwrap();
    o += &format!(
        "uint32_t ck_gpu_submit_token(uint32_t runlist_id, uint32_t chid)\n{{\n    return {};\n}}\n\n",
        call(tf, tok.1, &|_| "0".into())
    );
    let rl = g.create.iter().find_map(|s| if let Step::RunlistUserd { fmt, .. } = s { Some(fmt) } else { None }).unwrap();
    let rf = g.format(rl).unwrap();
    let lo = rf.fields.iter().map(|x| x.lo).min().unwrap();
    let hi = rf.fields.iter().map(|x| x.hi).max().unwrap();
    let rl_call = call(rf, &[], &|_| "userd_pa".into());
    o += &format!(
        "int ck_gpu_rl_userd(uint32_t out[2], uint64_t userd_pa)\n{{\n    if ((userd_pa & {:#x}ull) || userd_pa >= (1ull << {}))\n        return CK_GCHAN_EINVAL;\n    uint64_t v = {rl_call};\n    out[0] = (uint32_t)v;\n    out[1] = (uint32_t)(v >> 32);\n    return CK_GCHAN_OK;\n}}\n\n",
        (1u64 << lo) - 1,
        hi + 1
    );
    o += "static void fence(void)\n{\n    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n}\n\n";
    o += "static void zero(uint8_t *p, uint64_t n)\n{\n    for (uint64_t i = 0; i < n; i++)\n        ((volatile uint8_t *)p)[i] = 0;\n}\n\n";
    o += "static int buf_of(const struct ck_gmem *m, const struct ck_gmem_buf *b)\n{\n    for (uint32_t i = 0; i < CK_GMEM_MAX_BUF; i++)\n        if (&m->buf[i] == b)\n            return b->state == CK_GMEM_LIVE;\n    return 0;\n}\n\n";

    // ---- create
    let alloc = g.allocated();
    let mut free_all = String::new();
    for b in alloc.iter().rev() {
        free_all += &format!("    if (c->{0})\n        (void)ck_gmem_free(mem, c->{0});\n", buf_field(b));
    }
    o += "int ck_gpu_chan_create(struct ck_gpu_chan *c, struct ck_gmem *mem, const struct ck_gpu_regs *regs,\n                       const struct ck_gpu_chan_ops *ops, void *ops_ctx, uint32_t chid, uint32_t runlist_id,\n                       uint32_t entries)\n{\n";
    o += "    if (!c || !mem || !regs || !ops || !ops->bind || !ops->disable || !ops->is_idle)\n        return CK_GCHAN_EINVAL;\n";
    let mut inited = false;
    for s in &g.create {
        match s {
            Step::Refuse { arg, op, rhs, err } => {
                o += &format!("    if ({arg} {op} GCH_{rhs})\n        return CK_GCHAN_{err};\n")
            }
            Step::RefuseEntries { min, max, err } => {
                o += &format!("    if (entries < GCH_{min} || entries > GCH_{max} || (entries & (entries - 1u)))\n        return CK_GCHAN_{err};\n")
            }
            Step::Alloc { bufs, .. } => {
                if !inited {
                    o += "    c->open = 0;\n    c->mem = mem;\n    c->regs = regs;\n    c->ops = *ops;\n    c->ops_ctx = ops_ctx;\n    c->put = c->retired = 0;\n";
                    o += &format!("    {} = 0;\n", alloc.iter().map(|b| format!("c->{}", buf_field(b))).collect::<Vec<_>>().join(" = "));
                    inited = true;
                }
                for b in bufs {
                    let bb = g.buffer(b).unwrap();
                    o += &format!(
                        "    if (ck_gmem_alloc(mem, {}, {}, {}, &c->{}) != CK_GMEM_OK)\n        goto nomem;\n",
                        size_expr(bb), cache_c(bb), flags_c(bb), buf_field(b)
                    );
                }
            }
            Step::Zero(v) => {
                for b in v {
                    o += &format!("    zero(c->{0}->cpu, c->{0}->size);\n", buf_field(b));
                }
            }
            Step::StorePdb { buf, word, fmt, sets } => {
                let f = g.format(fmt).unwrap();
                let e = call(f, sets, &|x| if x.place { "mem->as->root_pa".into() } else { "0".into() });
                o += &format!(
                    "    {{\n        volatile uint32_t *w = (volatile uint32_t *)c->{}->cpu;\n        uint64_t v = {e};\n        w[GCH_{word}] = (uint32_t)v;\n        w[GCH_{word} + 1u] = (uint32_t)(v >> 32);\n    }}\n",
                    buf_field(buf)
                );
            }
            Step::Token { .. } => {
                let gp = g.buffers.iter().find(|b| b.size.is_err()).unwrap();
                o += "    c->d.chid = chid;\n    c->d.runlist_id = runlist_id;\n    c->d.token = ck_gpu_submit_token(runlist_id, chid);\n    c->d.entries = entries;\n    c->d.pdb = mem->as->root_pa;\n";
                o += &format!("    c->d.gpfifo_va = c->{}->gpu_va;\n", buf_field(&gp.name));
            }
            Step::RunlistUserd { buf, .. } => {
                let inst = alloc.iter().find(|b| b.as_str() != buf && g.buffer(b).map(|x| x.size.is_ok()).unwrap_or(false)).unwrap();
                o += &format!(
                    "    c->d.userd_pa = c->{0}->pa;\n    c->d.inst_pa = c->{1}->pa;\n    if (ck_gpu_rl_userd(c->d.rl_entry, c->{0}->pa) != CK_GCHAN_OK)\n        goto nomem;\n",
                    buf_field(buf),
                    buf_field(inst)
                );
            }
            Step::Fence => o += "    fence();\n",
            Step::Bind { err } => {
                o += "    if (c->ops.bind(c->ops_ctx, &c->d) != 0) {\n";
                for b in alloc.iter().rev() {
                    o += &format!("        (void)ck_gmem_free(mem, c->{});\n", buf_field(b));
                }
                o += &format!("        return CK_GCHAN_{err};\n    }}\n    c->open = 1;\n    return CK_GCHAN_OK;\n");
            }
            _ => {}
        }
    }
    let nomem = g.create.iter().find_map(|s| if let Step::Alloc { err, .. } = s { Some(err.clone()) } else { None }).unwrap();
    o += &format!("nomem:\n{free_all}    {} = 0;\n    return CK_GCHAN_{nomem};\n}}\n\n", alloc.iter().map(|b| format!("c->{}", buf_field(b))).collect::<Vec<_>>().join(" = "));

    // ---- submit
    let gp = buf_field(&g.buffers.iter().find(|b| b.size.is_err()).unwrap().name);
    o += "int ck_gpu_chan_submit(struct ck_gpu_chan *c, const struct ck_gmem_buf *pb, uint32_t off, uint32_t nwords)\n{\n";
    for s in &g.submit {
        match s {
            Step::RequireOpen(e) => o += &format!("    if (!c || !c->open)\n        return CK_GCHAN_{e};\n"),
            Step::RequireBuffer(e) => o += &format!("    if (!buf_of(c->mem, pb))\n        return CK_GCHAN_{e};\n"),
            Step::RequireSpan { max, err } => {
                o += &format!("    if ((off & 3u) || nwords == 0 || nwords > GCH_{max})\n        return CK_GCHAN_{err};\n    uint64_t span = (uint64_t)nwords * 4u;\n    if ((uint64_t)off > pb->size || span > pb->size - off)\n        return CK_GCHAN_{err};\n    uint64_t va = pb->gpu_va + off;\n")
            }
            Step::RequireVa { bits, err } => {
                o += &format!("    if (va >= (1ull << GCH_{bits}) || span > (1ull << GCH_{bits}) - va)\n        return CK_GCHAN_{err};\n")
            }
            Step::RequireRoom(e) => o += &format!("    if (c->put - c->retired >= c->d.entries - 1u)\n        return CK_GCHAN_{e};\n"),
            Step::RingWrite { fmt, sets } => {
                let f = g.format(fmt).unwrap();
                o += &format!(
                    "    volatile uint64_t *ring = (volatile uint64_t *)c->{gp}->cpu;\n    ring[c->put % c->d.entries] = {};\n    c->put++;\n",
                    call(f, sets, &|_| "0".into())
                );
            }
            Step::Fence => o += "    fence();\n",
            Step::Publish { buf, off } => {
                o += &format!("    *(volatile uint32_t *)(c->{}->cpu + GCH_{off}) = c->put % c->d.entries;\n", buf_field(buf))
            }
            Step::Doorbell { reg, .. } => {
                o += &format!("    c->regs->wr(c->regs->ctx, GCH_REG_{reg}_OFF, c->d.token);\n    return CK_GCHAN_OK;\n}}\n\n")
            }
            _ => {}
        }
    }
    o += "void ck_gpu_chan_retire(struct ck_gpu_chan *c, uint32_t upto)\n{\n    if (c && upto - c->retired <= c->put - c->retired)\n        c->retired = upto;\n}\n\n";

    // ---- teardown
    o += "int ck_gpu_chan_teardown(struct ck_gpu_chan *c, uint32_t idle_budget, struct ck_gmem_buf **extra, uint32_t nextra)\n{\n";
    for s in &g.teardown {
        match s {
            Step::RequireOpen(e) => o += &format!("    if (!c || !c->open)\n        return CK_GCHAN_{e};\n    c->open = 0;\n"),
            Step::Disable => o += "    (void)c->ops.disable(c->ops_ctx, c->d.chid);\n",
            Step::PollIdle => {
                o += "    int idle = 0;\n    for (uint32_t i = 0; i < idle_budget && !idle; i++)\n        idle = c->ops.is_idle(c->ops_ctx, c->d.chid) != 0;\n"
            }
            Step::Release(v) => {
                let own: Vec<String> = v.iter().map(|b| format!("c->{}", buf_field(b))).collect();
                o += &format!(
                    "    struct ck_gmem_buf *own[{n}] = {{{l}}};\n    int rc = CK_GCHAN_OK;\n    for (uint32_t i = 0; i < {n}; i++) {{\n        if (!idle)\n            (void)ck_gmem_quarantine(c->mem, own[i]);\n        else if (ck_gmem_free(c->mem, own[i]) != CK_GMEM_OK)\n            rc = CK_GCHAN_ETIMEDOUT;\n    }}\n    if (!idle) {{\n        for (uint32_t i = 0; extra && i < nextra; i++)\n            if (extra[i] && extra[i]->state == CK_GMEM_LIVE)\n                (void)ck_gmem_quarantine(c->mem, extra[i]);\n        rc = CK_GCHAN_ETIMEDOUT;\n    }}\n    {z} = 0;\n    return rc;\n}}\n",
                    n = v.len(),
                    l = own.join(", "),
                    z = own.join(" = ")
                );
            }
            _ => {}
        }
    }
    o
}

/// The documentation page.
pub fn markdown(g: &Gpu, digest: &str, c_lines: usize) -> String {
    let mut o = format!(
        "# {} (generated from {})\n\nContract sha256 `{digest}`. Class `{}`, transport `{}`, ring `{}`.\n{} statements compile to {c_lines} lines of C with the API of `native/kernel/gpu/gpu_chan.h`.\n\n",
        g.device, g.source_name, g.class, g.transport, g.ring_kind, g.stmt_count
    );
    o += "## What the compiler refuses (class gpu-chan)\n\n";
    for r in [
        "a doorbell before the put pointer is published, or anything after the doorbell",
        "no fence between the ring write and the publish, or between the publish and the doorbell",
        "a ring write with no room, buffer, span or address check before it",
        "a ring size that is not a power of two, or no ring-size check at all",
        "a channel or runlist bound that does not fit the doorbell token field it lands in",
        "an address check that differs from the width of the GPFIFO address field, a length check that differs from the length field",
        "memory the CPU writes while the GPU reads it (or that the CPU polls) that is GPU-cached",
        "a buffer never zeroed, a page-directory store before the zeroing",
        "binding before a fence, or anything after the bind",
        "teardown that frees before disable and idle, or does not release every buffer",
    ] {
        o += &format!("- {r}\n");
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
    o += "\n## Formats\n\n";
    for f in &g.formats {
        let x: Vec<String> = f.fields.iter().map(|x| format!("{} {}:{}{}", x.name, x.hi, x.lo, if x.place { " (address bits in place)" } else { "" })).collect();
        o += &format!("- {} ({} bits): {}\n", f.name, f.bits, x.join(", "));
    }
    o
}

#[cfg(test)]
mod tests {
    use super::*;

    const GOOD: &str = include_str!("../tests/gb10_chan.devc");

    #[test]
    fn good_contract_parses_and_emits_the_api() {
        let g = parse(GOOD, "gb10_chan.devc").unwrap();
        let c = source(&g, "x");
        for f in ["ck_gpu_submit_token", "ck_gpu_rl_userd", "ck_gpu_chan_create", "ck_gpu_chan_submit", "ck_gpu_chan_retire", "ck_gpu_chan_teardown"] {
            assert!(c.contains(&format!("{f}(")), "{f} missing");
        }
    }

    /// Each edit breaks one safety rule; the checker must refuse every one.
    #[test]
    fn checker_refuses_unsafe_contracts() {
        let cases: &[(&str, &str)] = &[
            ("  doorbell DOORBELL TOKEN\n", ""),
            ("  require-room else EFULL\n", ""),
            ("  publish USERD USERD_GPPUT\n  fence\n", "  publish USERD USERD_GPPUT\n"),
            ("  fence\n  bind else EBIND\n", "  bind else EBIND\n"),
            ("buffer USERD USERD_BYTES uncached host-polled", "buffer USERD USERD_BYTES cached host-polled"),
            ("const CHID_MAX 0xFFF", "const CHID_MAX 0x1FFF"),
            ("const GP_VA_BITS 40", "const GP_VA_BITS 41"),
            ("const GP_LEN_MAX 0x1FFFFF", "const GP_LEN_MAX 0x3FFFFF"),
            ("  zero GPFIFO USERD INST\n", "  zero GPFIFO USERD\n"),
            ("  release INST USERD GPFIFO\n", "  release INST USERD\n"),
            ("  disable\n  poll-idle\n", "  poll-idle\n  disable\n"),
            ("  refuse-entries pow2 ENTRIES_MIN ENTRIES_MAX else EINVAL\n", ""),
            ("  fence\n  bind else EBIND\n", "  bind else EBIND\n  fence\n  bind else EBIND\n"),
            ("format RL_USERD 64 PTR_LO 31:8 place PTR_HI 51:32 place", "format RL_USERD 64 PTR_LO 31:8 place PTR_HI 51:33 place"),
            ("format TOKEN 32 RUNLIST_ID 22:16 VECTOR 11:0", "format TOKEN 32 RUNLIST_ID 22:16 VECTOR 11:2"),
        ];
        for (from, to) in cases {
            assert!(GOOD.contains(from), "case text not in contract: {from}");
            let bad = GOOD.replacen(from, to, 1);
            assert!(parse(&bad, "bad").is_err(), "checker accepted: {from:?} -> {to:?}");
        }
    }
}
