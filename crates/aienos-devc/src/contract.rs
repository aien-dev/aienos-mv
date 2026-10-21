//! The `.devc` format (devc 1): parse and check a device contract.
//!
//! A contract is data: identity, evidence, constants, register windows,
//! registers, bits, features, errors, resources, rings, the bring-up steps and
//! the protocol rules the chip model enforces. `parse` reads it line by line;
//! `check` refuses anything a generated driver could use to reach outside what
//! the contract declares (an undeclared register, a write to a read-only one,
//! a register outside its window's checked length, an unknown name).

use std::collections::BTreeMap;
use std::fmt;

#[derive(Debug)]
pub struct Error {
    pub line: usize,
    pub msg: String,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.line == 0 {
            write!(f, "contract: {}", self.msg)
        } else {
            write!(f, "contract line {}: {}", self.line, self.msg)
        }
    }
}

pub(crate) fn err<T>(line: usize, msg: impl Into<String>) -> Result<T, Error> {
    Err(Error {
        line,
        msg: msg.into(),
    })
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Access {
    Ro,
    Rw,
    Wo,
}

#[derive(Debug, Clone)]
pub struct Match {
    pub vendor: u32,
    pub device: u32,
    pub subsys: Option<u32>,
}

#[derive(Debug, Clone)]
pub struct Manual {
    pub doc: String,
    pub section: String,
    pub quote: String,
}

#[derive(Debug, Clone)]
pub struct Proven {
    pub platform: String,
    pub machine: String,
    pub date: String,
    pub gates: Vec<String>,
    pub commit: String,
}

#[derive(Debug, Clone)]
pub struct Const {
    pub name: String,
    /// C expression over earlier constants (`VNET_A + VNET_B`) or a number.
    pub c_expr: String,
    pub value: u64,
}

#[derive(Debug, Clone)]
pub struct Window {
    pub name: String,
    pub index: u32,
    pub cap_type: u32,
    pub min: u64,
    pub tag: Option<String>,
    pub multiplier: bool,
}

#[derive(Debug, Clone)]
pub struct Reg {
    pub window: String,
    pub name: String,
    pub offset: u64,
    pub width: u32,
    pub access: Access,
    pub reset: Option<u64>,
    pub bank: Option<String>,
    pub count: u32,
    pub queue_notify: bool,
    pub role: Option<String>,
}

#[derive(Debug, Clone)]
pub struct Bits {
    pub reg: String,
    pub group: String,
    pub bits: Vec<(String, u32)>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Policy {
    Require,
    Accept,
}

#[derive(Debug, Clone)]
pub struct Feature {
    pub name: String,
    pub bit: u32,
    pub policy: Policy,
    pub needs: Option<(String, u64)>,
    pub required_by: Option<String>,
    pub else_err: Option<String>,
}

#[derive(Debug, Clone)]
pub struct FeatureBanks {
    pub banks: u32,
    pub offer_sel: String,
    pub offer: String,
    pub accept_sel: String,
    pub accept: String,
}

#[derive(Debug, Clone)]
pub struct ErrorDef {
    pub name: String,
    pub code: u32,
    pub text: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Dir {
    DeviceWrites,
    DeviceReads,
}

#[derive(Debug, Clone)]
pub struct Ring {
    pub name: String,
    pub index: u32,
    pub dir: Dir,
    pub buf: String,
    pub stride: String,
    pub hdr: String,
}

/// A value written by a `write` step.
#[derive(Debug, Clone)]
pub enum Val {
    Num(u64),
    /// bit names of the register's `bits` group, OR-ed
    Bits(Vec<String>),
    /// `ring` (the queue index) or `size` (the fitted queue size), qsetup only
    Var(String),
}

#[derive(Debug, Clone)]
pub enum Cond {
    Has(Vec<String>, Vec<String>),
    Ge(u64),
    Ne(u64),
    Eq(u64),
}

#[derive(Debug, Clone)]
pub enum Step {
    Write {
        reg: String,
        val: Val,
    },
    Poll {
        reg: String,
        eq: u64,
        tries: String,
        err: String,
        nofail: bool,
    },
    ArmFail,
    Negotiate,
    Expect {
        lhs: String,
        cond: Cond,
        err: String,
    },
    CommitFeatures,
    Queues(Vec<String>),
    StableRead {
        reg: String,
        into: String,
        guard: String,
        tries: String,
        err: String,
        when: Option<String>,
    },
    Post(String),
    Live,
    Kick(String),
    Read {
        reg: String,
        var: String,
    },
    Fit {
        var: String,
        min: String,
        err: String,
    },
    NotifyOffset {
        reg: String,
        err: String,
    },
    Write64 {
        lo: String,
        hi: String,
        what: String,
    },
    AvailFlags(String),
}

#[derive(Debug, Clone)]
pub struct Rule {
    pub reg: String,
    pub write: bool,
    pub requires: Vec<String>,
    pub lacks: Vec<String>,
}

#[derive(Debug, Default, Clone)]
pub struct Contract {
    pub source_name: String,
    pub device: String,
    pub prefix: String,
    pub class: String,
    pub transport: String,
    pub ring_kind: String,
    pub matches: Vec<Match>,
    pub manuals: Vec<Manual>,
    pub proven: Vec<Proven>,
    pub consts: Vec<Const>,
    pub windows: Vec<Window>,
    pub regs: Vec<Reg>,
    pub bits: Vec<Bits>,
    pub features: Vec<Feature>,
    pub feature_banks: Option<FeatureBanks>,
    pub flags: Vec<(String, u64)>,
    pub errors: Vec<ErrorDef>,
    pub dma_align: u64,
    pub irq: String,
    pub qsize_min: String,
    pub qsize_max: String,
    pub rings: Vec<Ring>,
    pub failmark: Option<(String, String)>,
    pub init: Vec<Step>,
    pub qsetup: Vec<Step>,
    pub rules: Vec<Rule>,
    /// statement count by kind, for the declarative/hand-logic ratio in the doc
    pub stmt_count: usize,
}

/// Split a line into words; a double-quoted string is one word (quotes removed).
pub(crate) fn words(line: &str, ln: usize) -> Result<Vec<String>, Error> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut in_str = false;
    let mut had = false;
    for ch in line.chars() {
        if in_str {
            if ch == '"' {
                in_str = false;
                out.push(std::mem::take(&mut cur));
                had = false;
            } else {
                cur.push(ch);
            }
        } else if ch == '#' {
            break;
        } else if ch == '"' {
            if had {
                return err(ln, "a quote inside a word");
            }
            in_str = true;
        } else if ch.is_whitespace() {
            if had {
                out.push(std::mem::take(&mut cur));
                had = false;
            }
        } else {
            cur.push(ch);
            had = true;
        }
    }
    if in_str {
        return err(ln, "unterminated string");
    }
    if had {
        out.push(cur);
    }
    Ok(out)
}

pub fn num(s: &str, ln: usize) -> Result<u64, Error> {
    let r = if let Some(h) = s.strip_prefix("0x") {
        u64::from_str_radix(h, 16)
    } else {
        s.parse::<u64>()
    };
    r.or_else(|_| err(ln, format!("not a number: {s}")))
}

pub(crate) fn is_ident(s: &str) -> bool {
    let mut c = s.chars();
    matches!(c.next(), Some(ch) if ch.is_ascii_alphabetic() || ch == '_')
        && s.chars().all(|ch| ch.is_ascii_alphanumeric() || ch == '_')
}

/// `key value` pairs after a fixed head, e.g. `reset 0 bank X count 6`.
fn opts<'a>(
    w: &'a [String],
    ln: usize,
    flags: &[&str],
    keyed: &[&str],
) -> Result<BTreeMap<&'a str, &'a str>, Error> {
    let mut m = BTreeMap::new();
    let mut i = 0;
    while i < w.len() {
        let k = w[i].as_str();
        if flags.contains(&k) {
            m.insert(k, "");
            i += 1;
        } else if keyed.contains(&k) {
            let Some(v) = w.get(i + 1) else {
                return err(ln, format!("`{k}` needs a value"));
            };
            m.insert(k, v.as_str());
            i += 2;
        } else {
            return err(ln, format!("unknown word `{k}`"));
        }
    }
    Ok(m)
}

fn need(w: &[String], n: usize, ln: usize, what: &str) -> Result<(), Error> {
    if w.len() < n {
        return err(ln, format!("`{what}` needs {} words, got {}", n, w.len()));
    }
    Ok(())
}

fn bitlist(s: &str) -> Vec<String> {
    s.split('|').map(str::to_string).collect()
}

fn parse_step(w: &[String], ln: usize, in_qsetup: bool) -> Result<Step, Error> {
    let s = |i: usize| w.get(i).map(String::as_str).unwrap_or("");
    let expect_word = |i: usize, want: &str| -> Result<(), Error> {
        if s(i) != want {
            return err(
                ln,
                format!("expected `{want}` at word {}, got `{}`", i + 1, s(i)),
            );
        }
        Ok(())
    };
    let step = match s(0) {
        "write" => {
            need(w, 3, ln, "write")?;
            let v = s(2);
            let val = if v == "ring" || v == "size" {
                if !in_qsetup {
                    return err(ln, format!("`{v}` is only known inside qsetup"));
                }
                Val::Var(v.to_string())
            } else if v.starts_with(|c: char| c.is_ascii_digit()) {
                Val::Num(num(v, ln)?)
            } else {
                Val::Bits(bitlist(v))
            };
            Step::Write {
                reg: s(1).into(),
                val,
            }
        }
        "poll" => {
            need(w, 8, ln, "poll")?;
            expect_word(2, "==")?;
            expect_word(4, "tries")?;
            expect_word(6, "else")?;
            let nofail = match s(8) {
                "" => false,
                "nofail" => true,
                o => return err(ln, format!("unknown word `{o}`")),
            };
            Step::Poll {
                reg: s(1).into(),
                eq: num(s(3), ln)?,
                tries: s(5).into(),
                err: s(7).into(),
                nofail,
            }
        }
        "armfail" => Step::ArmFail,
        "negotiate" => Step::Negotiate,
        "commit-features" => Step::CommitFeatures,
        "live" => Step::Live,
        "expect" => {
            need(w, 5, ln, "expect")?;
            let lhs = s(1).to_string();
            let (cond, rest) = match s(2) {
                "has" => {
                    let has = bitlist(s(3));
                    if s(4) == "lacks" {
                        (Cond::Has(has, bitlist(s(5))), 6)
                    } else {
                        (Cond::Has(has, vec![]), 4)
                    }
                }
                ">=" => (Cond::Ge(num(s(3), ln)?), 4),
                "!=" => (Cond::Ne(num(s(3), ln)?), 4),
                "==" => (Cond::Eq(num(s(3), ln)?), 4),
                o => return err(ln, format!("unknown condition `{o}`")),
            };
            expect_word(rest, "else")?;
            if w.len() != rest + 2 {
                return err(ln, "expect: words after the error name");
            }
            Step::Expect {
                lhs,
                cond,
                err: s(rest + 1).into(),
            }
        }
        "queues" => Step::Queues(w[1..].to_vec()),
        "stable-read" => {
            need(w, 10, ln, "stable-read")?;
            expect_word(2, "into")?;
            expect_word(4, "guard")?;
            expect_word(6, "tries")?;
            expect_word(8, "else")?;
            let when = match (s(10), s(11)) {
                ("", _) => None,
                ("when", f) if !f.is_empty() => Some(f.to_string()),
                (o, _) => return err(ln, format!("unknown word `{o}`")),
            };
            Step::StableRead {
                reg: s(1).into(),
                into: s(3).into(),
                guard: s(5).into(),
                tries: s(7).into(),
                err: s(9).into(),
                when,
            }
        }
        "post" => {
            need(w, 2, ln, "post")?;
            Step::Post(s(1).into())
        }
        "kick" => {
            need(w, 2, ln, "kick")?;
            Step::Kick(s(1).into())
        }
        "read" if in_qsetup => {
            need(w, 3, ln, "read")?;
            Step::Read {
                reg: s(1).into(),
                var: s(2).into(),
            }
        }
        "fit" if in_qsetup => {
            need(w, 6, ln, "fit")?;
            expect_word(2, "min")?;
            expect_word(4, "else")?;
            Step::Fit {
                var: s(1).into(),
                min: s(3).into(),
                err: s(5).into(),
            }
        }
        "notify-offset" if in_qsetup => {
            need(w, 4, ln, "notify-offset")?;
            expect_word(2, "else")?;
            Step::NotifyOffset {
                reg: s(1).into(),
                err: s(3).into(),
            }
        }
        "write64" if in_qsetup => {
            need(w, 4, ln, "write64")?;
            Step::Write64 {
                lo: s(1).into(),
                hi: s(2).into(),
                what: s(3).into(),
            }
        }
        "avail-flags" if in_qsetup => {
            need(w, 2, ln, "avail-flags")?;
            Step::AvailFlags(s(1).into())
        }
        o => return err(ln, format!("unknown step `{o}`")),
    };
    Ok(step)
}

pub fn parse(src: &str, source_name: &str) -> Result<Contract, Error> {
    let mut c = Contract {
        source_name: source_name.to_string(),
        ..Default::default()
    };
    let mut block: Option<&str> = None;
    let mut saw_header = false;
    for (i, raw) in src.lines().enumerate() {
        let ln = i + 1;
        let w = words(raw, ln)?;
        if w.is_empty() {
            continue;
        }
        c.stmt_count += 1;
        let s = |k: usize| w.get(k).map(String::as_str).unwrap_or("");
        if let Some(b) = block {
            if s(0) == "end" {
                block = None;
                continue;
            }
            let step = parse_step(&w, ln, b == "qsetup")?;
            if b == "init" {
                c.init.push(step);
            } else {
                c.qsetup.push(step);
            }
            continue;
        }
        if !saw_header {
            if s(0) != "devc" || s(1) != "1" || w.len() != 2 {
                return err(ln, "the first statement must be `devc 1`");
            }
            saw_header = true;
            continue;
        }
        match s(0) {
            "device" => c.device = s(1).into(),
            "prefix" => c.prefix = s(1).into(),
            "class" => c.class = s(1).into(),
            "transport" => c.transport = s(1).into(),
            "ring-kind" => c.ring_kind = s(1).into(),
            "match" => {
                need(&w, 4, ln, "match")?;
                if s(1) != "pci" {
                    return err(ln, "only `match pci` is known");
                }
                let o = opts(&w[4..], ln, &[], &["subsys"])?;
                c.matches.push(Match {
                    vendor: num(s(2), ln)? as u32,
                    device: num(s(3), ln)? as u32,
                    subsys: o
                        .get("subsys")
                        .map(|v| num(v, ln))
                        .transpose()?
                        .map(|v| v as u32),
                });
            }
            "manual" => {
                need(&w, 4, ln, "manual")?;
                c.manuals.push(Manual {
                    doc: s(1).into(),
                    section: s(2).into(),
                    quote: s(3).into(),
                });
            }
            "proven" => {
                need(&w, 6, ln, "proven")?;
                c.proven.push(Proven {
                    platform: s(1).into(),
                    machine: s(2).into(),
                    date: s(3).into(),
                    gates: s(4).split(',').map(str::to_string).collect(),
                    commit: s(5).into(),
                });
            }
            "const" => {
                need(&w, 3, ln, "const")?;
                let name = s(1).to_string();
                if !is_ident(&name) {
                    return err(ln, format!("bad name `{name}`"));
                }
                // a number, a constant, or terms joined by `+`
                let mut value: u64 = 0;
                let mut parts = Vec::new();
                for (k, t) in w[2..].iter().enumerate() {
                    if k % 2 == 1 {
                        if t != "+" {
                            return err(ln, "const: only `+` joins terms");
                        }
                        continue;
                    }
                    let (v, cx) = if t.starts_with(|ch: char| ch.is_ascii_digit()) {
                        (num(t, ln)?, t.clone())
                    } else if let Some(k) = c.consts.iter().find(|k| &k.name == t) {
                        (k.value, format!("{}_{}", c.prefix.to_uppercase(), k.name))
                    } else {
                        return err(ln, format!("unknown constant `{t}`"));
                    };
                    value = value.checked_add(v).ok_or(Error {
                        line: ln,
                        msg: "const overflows".into(),
                    })?;
                    parts.push(cx);
                }
                if w.len() % 2 == 0 {
                    return err(ln, "const: dangling `+`");
                }
                let c_expr = if parts.len() == 1 && w[2].starts_with(|ch: char| ch.is_ascii_digit())
                {
                    format!("{}u", value)
                } else {
                    format!("({})", parts.join(" + "))
                };
                c.consts.push(Const {
                    name,
                    c_expr,
                    value,
                });
            }
            "window" => {
                need(&w, 7, ln, "window")?;
                if s(3) != "virtio-cap" || s(5) != "min" {
                    return err(
                        ln,
                        "window NAME INDEX virtio-cap TYPE min LEN [tag T] [multiplier]",
                    );
                }
                let o = opts(&w[7..], ln, &["multiplier"], &["tag"])?;
                c.windows.push(Window {
                    name: s(1).into(),
                    index: num(s(2), ln)? as u32,
                    cap_type: num(s(4), ln)? as u32,
                    min: num(s(6), ln)?,
                    tag: o.get("tag").map(|v| v.to_string()),
                    multiplier: o.contains_key("multiplier"),
                });
            }
            "reg" => {
                need(&w, 6, ln, "reg")?;
                let access = match s(5) {
                    "ro" => Access::Ro,
                    "rw" => Access::Rw,
                    "wo" => Access::Wo,
                    o => return err(ln, format!("unknown access `{o}`")),
                };
                let o = opts(
                    &w[6..],
                    ln,
                    &["queue-notify"],
                    &["reset", "bank", "count", "role"],
                )?;
                c.regs.push(Reg {
                    window: s(1).into(),
                    name: s(2).into(),
                    offset: num(s(3), ln)?,
                    width: num(s(4), ln)? as u32,
                    access,
                    reset: o.get("reset").map(|v| num(v, ln)).transpose()?,
                    bank: o.get("bank").map(|v| v.to_string()),
                    count: o.get("count").map(|v| num(v, ln)).transpose()?.unwrap_or(1) as u32,
                    queue_notify: o.contains_key("queue-notify"),
                    role: o.get("role").map(|v| v.to_string()),
                });
            }
            "bits" => {
                need(&w, 4, ln, "bits")?;
                let mut bits = Vec::new();
                for b in &w[3..] {
                    let Some((n, v)) = b.split_once('=') else {
                        return err(ln, format!("bits: `{b}` is not NAME=bit"));
                    };
                    bits.push((n.to_string(), num(v, ln)? as u32));
                }
                c.bits.push(Bits {
                    reg: s(1).into(),
                    group: s(2).into(),
                    bits,
                });
            }
            "feature" => {
                need(&w, 4, ln, "feature")?;
                let policy = match s(3) {
                    "require" => Policy::Require,
                    "accept" => Policy::Accept,
                    o => return err(ln, format!("unknown feature policy `{o}`")),
                };
                let mut needs = None;
                let mut required_by = None;
                let mut else_err = None;
                let mut k = 4;
                while k < w.len() {
                    match s(k) {
                        "needs" => {
                            needs = Some((s(k + 1).to_string(), num(s(k + 2), ln)?));
                            k += 3;
                        }
                        "required-by" => {
                            required_by = Some(s(k + 1).to_string());
                            k += 2;
                        }
                        "else" => {
                            else_err = Some(s(k + 1).to_string());
                            k += 2;
                        }
                        o => return err(ln, format!("unknown word `{o}`")),
                    }
                }
                c.features.push(Feature {
                    name: s(1).into(),
                    bit: num(s(2), ln)? as u32,
                    policy,
                    needs,
                    required_by,
                    else_err,
                });
            }
            "features" => {
                need(&w, 9, ln, "features")?;
                if s(1) != "banks" || s(3) != "offer" || s(6) != "accept" {
                    return err(ln, "features banks N offer SEL REG accept SEL REG");
                }
                c.feature_banks = Some(FeatureBanks {
                    banks: num(s(2), ln)? as u32,
                    offer_sel: s(4).into(),
                    offer: s(5).into(),
                    accept_sel: s(7).into(),
                    accept: s(8).into(),
                });
            }
            "flag" => {
                need(&w, 3, ln, "flag")?;
                c.flags.push((s(1).into(), num(s(2), ln)?));
            }
            "error" => {
                need(&w, 4, ln, "error")?;
                c.errors.push(ErrorDef {
                    name: s(1).into(),
                    code: num(s(2), ln)? as u32,
                    text: s(3).into(),
                });
            }
            "dma" => {
                if s(1) != "region" || s(2) != "align" {
                    return err(ln, "dma region align N");
                }
                c.dma_align = num(s(3), ln)?;
            }
            "irq" => c.irq = s(1).into(),
            "queue-size" => {
                if s(1) != "min" || s(3) != "max" || s(5) != "pow2" {
                    return err(ln, "queue-size min C max C pow2");
                }
                c.qsize_min = s(2).into();
                c.qsize_max = s(4).into();
            }
            "ring" => {
                need(&w, 10, ln, "ring")?;
                if s(5) != "buf" || s(7) != "stride" || s(9) != "hdr" {
                    return err(ln, "ring NAME INDEX KIND DIR buf C stride C hdr C");
                }
                let dir = match s(4) {
                    "device-writes" => Dir::DeviceWrites,
                    "device-reads" => Dir::DeviceReads,
                    o => return err(ln, format!("unknown ring direction `{o}`")),
                };
                if s(3) != c.ring_kind {
                    return err(
                        ln,
                        format!("ring kind `{}` is not the contract's ring-kind", s(3)),
                    );
                }
                c.rings.push(Ring {
                    name: s(1).into(),
                    index: num(s(2), ln)? as u32,
                    dir,
                    buf: s(6).into(),
                    stride: s(8).into(),
                    hdr: s(10).into(),
                });
            }
            "failmark" => {
                need(&w, 3, ln, "failmark")?;
                c.failmark = Some((s(1).into(), s(2).into()));
            }
            "init" | "qsetup" => {
                if w.len() != 1 {
                    return err(ln, format!("`{}` stands alone on its line", s(0)));
                }
                block = Some(if s(0) == "init" { "init" } else { "qsetup" });
            }
            "rule" => {
                need(&w, 5, ln, "rule")?;
                let write = match s(2) {
                    "write" => true,
                    "read" => false,
                    o => return err(ln, format!("rule: `{o}` is not read or write")),
                };
                if s(3) != "requires-status" {
                    return err(ln, "rule REG write|read requires-status BITS [lacks BITS]");
                }
                let lacks = if s(5) == "lacks" {
                    bitlist(s(6))
                } else {
                    vec![]
                };
                c.rules.push(Rule {
                    reg: s(1).into(),
                    write,
                    requires: bitlist(s(4)),
                    lacks,
                });
            }
            o => return err(ln, format!("unknown statement `{o}`")),
        }
    }
    if block.is_some() {
        return err(0, "a block is missing its `end`");
    }
    if !saw_header {
        return err(0, "empty contract");
    }
    check(&c)?;
    Ok(c)
}

impl Contract {
    pub fn reg(&self, n: &str) -> Option<&Reg> {
        self.regs.iter().find(|r| r.name == n)
    }
    pub fn window(&self, n: &str) -> Option<&Window> {
        self.windows.iter().find(|w| w.name == n)
    }
    pub fn konst(&self, n: &str) -> Option<&Const> {
        self.consts.iter().find(|k| k.name == n)
    }
    pub fn feature(&self, n: &str) -> Option<&Feature> {
        self.features.iter().find(|f| f.name == n)
    }
    pub fn ring(&self, n: &str) -> Option<&Ring> {
        self.rings.iter().find(|r| r.name == n)
    }
    pub fn bits_of(&self, reg: &str) -> Option<&Bits> {
        self.bits.iter().find(|b| b.reg == reg)
    }
    pub fn up(&self) -> String {
        self.prefix.to_uppercase()
    }
    /// C name of a constant
    pub fn cname(&self, n: &str) -> String {
        format!("{}_{}", self.up(), n)
    }
    /// C name of a register offset: the window tag if any, else the window name
    pub fn reg_cname(&self, r: &Reg) -> String {
        let w = self.window(&r.window).expect("checked");
        match &w.tag {
            Some(t) => format!("{}_{}_{}", self.up(), t, r.name),
            None => format!("{}_{}_{}", self.up(), w.name.to_uppercase(), r.name),
        }
    }
    pub fn bit_cname(&self, reg: &str, bit: &str) -> String {
        let b = self.bits_of(reg).expect("checked");
        format!("{}_{}_{}", self.up(), b.group, bit)
    }
    pub fn tx_ring(&self) -> &Ring {
        self.rings
            .iter()
            .find(|r| r.dir == Dir::DeviceReads)
            .expect("checked")
    }
    pub fn rx_ring(&self) -> &Ring {
        self.rings
            .iter()
            .find(|r| r.dir == Dir::DeviceWrites)
            .expect("checked")
    }
    /// The register whose writes carry a queue doorbell.
    pub fn doorbell(&self) -> &Reg {
        self.regs.iter().find(|r| r.queue_notify).expect("checked")
    }
    /// The proof status for the doc and the generated admission constant.
    pub fn hardware_receipts(&self) -> usize {
        self.proven
            .iter()
            .filter(|p| p.commit != "pending" && !p.platform.starts_with("qemu"))
            .count()
    }
}

fn check(c: &Contract) -> Result<(), Error> {
    let e = |m: String| err::<()>(0, m);
    for (what, v) in [
        ("device", &c.device),
        ("prefix", &c.prefix),
        ("class", &c.class),
    ] {
        if v.is_empty() {
            return e(format!("`{what}` is missing"));
        }
    }
    if c.class != "net-poll" || c.transport != "virtio-pci" || c.ring_kind != "virtq-split" {
        return e(format!(
            "devc 1 knows class net-poll, transport virtio-pci, ring-kind virtq-split (got {} / {} / {})",
            c.class, c.transport, c.ring_kind
        ));
    }
    if !is_ident(&c.prefix) {
        return e("prefix must be a C identifier".into());
    }
    if c.manuals.is_empty() {
        return e("a contract needs at least one `manual` line (ADR 0021: evidence travels with the contract)".into());
    }
    // windows
    let mut idx: Vec<u32> = c.windows.iter().map(|w| w.index).collect();
    idx.sort();
    if idx != (0..c.windows.len() as u32).collect::<Vec<_>>() {
        return e("window indexes must be 0..n-1, each once".into());
    }
    for w in &c.windows {
        if !(1..=4).contains(&w.cap_type) {
            return e(format!(
                "window {}: virtio-cap type {} is not 1..4",
                w.name, w.cap_type
            ));
        }
    }
    // registers: window known, width 1/2/4, unique names, inside the checked window length
    let mut seen = BTreeMap::new();
    for r in &c.regs {
        let Some(w) = c.window(&r.window) else {
            return e(format!("reg {}: unknown window {}", r.name, r.window));
        };
        if ![1, 2, 4].contains(&r.width) {
            return e(format!(
                "reg {}: width {} is not 1, 2 or 4",
                r.name, r.width
            ));
        }
        if seen.insert(r.name.clone(), ()).is_some() {
            return e(format!("reg {} declared twice", r.name));
        }
        if r.offset % r.width as u64 != 0 {
            return e(format!("reg {}: offset not aligned to its width", r.name));
        }
        let end = r.offset + r.width as u64 * r.count as u64;
        let covered_by_need = c
            .features
            .iter()
            .any(|f| matches!(&f.needs, Some((wn, len)) if *wn == w.name && end <= *len));
        if !(end <= w.min || covered_by_need || (r.queue_notify && w.multiplier)) {
            return e(format!(
                "reg {}: bytes {:#x}..{:#x} lie outside window {}'s checked length {:#x} and no feature `needs` them",
                r.name, r.offset, end, w.name, w.min
            ));
        }
        if let Some(b) = &r.bank {
            match c.reg(b) {
                Some(s) if s.access == Access::Rw => {}
                _ => {
                    return e(format!(
                        "reg {}: bank selector {b} must be an rw register declared above",
                        r.name
                    ))
                }
            }
        }
        if let Some(v) = r.reset {
            if r.width < 8 && v >> (8 * r.width) != 0 {
                return e(format!(
                    "reg {}: reset value does not fit {} bytes",
                    r.name, r.width
                ));
            }
        }
    }
    for role in VIRTIO_PCI_ROLES {
        if c.regs
            .iter()
            .filter(|r| r.role.as_deref() == Some(role))
            .count()
            != 1
        {
            return e(format!(
                "transport virtio-pci needs exactly one register with role {role}"
            ));
        }
    }
    for r in &c.regs {
        if let Some(x) = &r.role {
            if !VIRTIO_PCI_ROLES.contains(&x.as_str()) {
                return e(format!("reg {}: unknown role {x}", r.name));
            }
        }
    }
    if c.rings.iter().any(|r| {
        r.buf != c.rings[0].buf || r.stride != c.rings[0].stride || r.hdr != c.rings[0].hdr
    }) {
        return e("devc 1: every ring uses the same buf, stride and hdr constants".into());
    }
    if c.regs.iter().filter(|r| r.queue_notify).count() != 1 {
        return e("exactly one register is the queue doorbell (`queue-notify`)".into());
    }
    let db = c.doorbell();
    if db.access != Access::Wo || !c.window(&db.window).is_some_and(|w| w.multiplier) {
        return e(
            "the doorbell is write-only and lives in the window with the notify multiplier".into(),
        );
    }
    for b in &c.bits {
        let Some(r) = c.reg(&b.reg) else {
            return e(format!("bits: unknown register {}", b.reg));
        };
        for (n, v) in &b.bits {
            if *v >= 8 * r.width {
                return e(format!("bit {n}={v} does not fit register {}", r.name));
            }
        }
    }
    // features
    let Some(fb) = &c.feature_banks else {
        return e("`features banks ...` is missing".into());
    };
    if fb.banks != 2 {
        return e("devc 1 negotiates exactly 2 feature banks (64 bits)".into());
    }
    for (n, want_ro) in [
        (&fb.offer_sel, false),
        (&fb.offer, true),
        (&fb.accept_sel, false),
        (&fb.accept, false),
    ] {
        match c.reg(n) {
            Some(r) if (r.access == Access::Ro) == want_ro && r.width == 4 => {}
            _ => {
                return e(format!(
                    "features: {n} must be a 4-byte {} register",
                    if want_ro { "ro" } else { "rw" }
                ))
            }
        }
    }
    let errname = |n: &str| c.errors.iter().any(|x| x.name == n);
    for f in &c.features {
        if f.bit >= 64 {
            return e(format!("feature {}: bit {} >= 64", f.name, f.bit));
        }
        if f.policy == Policy::Require && f.else_err.is_none() {
            return e(format!("feature {}: `require` needs `else ERROR`", f.name));
        }
        if let Some(fl) = &f.required_by {
            if !c.flags.iter().any(|(n, _)| n == fl) {
                return e(format!("feature {}: unknown flag {fl}", f.name));
            }
            if f.else_err.is_none() {
                return e(format!(
                    "feature {}: `required-by` needs `else ERROR`",
                    f.name
                ));
            }
        }
        if let Some(x) = &f.else_err {
            if !errname(x) {
                return e(format!("feature {}: unknown error {x}", f.name));
            }
        }
        if let Some((wn, _)) = &f.needs {
            if c.window(wn).is_none() {
                return e(format!("feature {}: unknown window {wn}", f.name));
            }
        }
    }
    // errors: codes unique and > 0; the class needs these
    let mut codes: Vec<u32> = c.errors.iter().map(|x| x.code).collect();
    codes.sort();
    codes.dedup();
    if codes.len() != c.errors.len() || codes.first() == Some(&0) {
        return e("error codes must be unique and non-zero (0 is Ok)".into());
    }
    for n in ["ARG", "CAPS", "MEM", "FULL", "DEVICE", "BROKEN"] {
        if !errname(n) {
            return e(format!("class net-poll needs error {n}"));
        }
    }
    // resources
    if c.dma_align == 0 || !c.dma_align.is_power_of_two() {
        return e("dma region align must be a power of two".into());
    }
    if c.irq != "none" {
        return e("devc 1 drivers are polled: `irq none`".into());
    }
    for k in [&c.qsize_min, &c.qsize_max] {
        if c.konst(k).is_none() {
            return e(format!("queue-size: unknown constant {k}"));
        }
    }
    // rings: one of each direction, indexes 0..n-1, constants known
    if c.rings.len() != 2 || c.rings[0].dir == c.rings[1].dir {
        return e(
            "class net-poll has one device-writes (RX) and one device-reads (TX) ring".into(),
        );
    }
    for (k, r) in c.rings.iter().enumerate() {
        if r.index != k as u32 {
            return e(format!("ring {} must have index {k}", r.name));
        }
        for n in [&r.buf, &r.stride, &r.hdr] {
            if c.konst(n).is_none() {
                return e(format!("ring {}: unknown constant {n}", r.name));
            }
        }
        let (buf, stride) = (
            c.konst(&r.buf).unwrap().value,
            c.konst(&r.stride).unwrap().value,
        );
        if buf > stride || stride % c.dma_align != 0 {
            return e(format!(
                "ring {}: buffer longer than its stride, or stride not DMA-aligned",
                r.name
            ));
        }
    }
    // steps
    let Some((fr, fbit)) = &c.failmark else {
        return e("`failmark REG BIT` is missing".into());
    };
    check_bits(c, fr, std::slice::from_ref(fbit))?;
    let mut seen_neg = 0;
    let mut seen_live = 0;
    let mut seen_queues = 0;
    for s in &c.init {
        check_step(c, s, false)?;
        match s {
            Step::Negotiate => seen_neg += 1,
            Step::Live => seen_live += 1,
            Step::Queues(rs) => {
                seen_queues += 1;
                let mut names: Vec<&str> = rs.iter().map(String::as_str).collect();
                names.sort();
                let mut all: Vec<&str> = c.rings.iter().map(|r| r.name.as_str()).collect();
                all.sort();
                if names != all {
                    return e("`queues` must name every ring once".into());
                }
            }
            _ => {}
        }
    }
    if seen_neg != 1 || seen_live != 1 || seen_queues != 1 {
        return e("init needs exactly one negotiate, one queues and one live".into());
    }
    if c.qsetup.is_empty() {
        return e("qsetup is missing".into());
    }
    for s in &c.qsetup {
        check_step(c, s, true)?;
    }
    for r in &c.rules {
        let Some(reg) = c.reg(&r.reg) else {
            return e(format!("rule: unknown register {}", r.reg));
        };
        if r.write && reg.access == Access::Ro {
            return e(format!("rule: {} is read-only", r.reg));
        }
        check_bits(c, fr, &r.requires)?;
        check_bits(c, fr, &r.lacks)?;
    }
    Ok(())
}

fn check_bits(c: &Contract, reg: &str, names: &[String]) -> Result<(), Error> {
    let Some(b) = c.bits_of(reg) else {
        return err(0, format!("register {reg} has no `bits`"));
    };
    for n in names {
        if !b.bits.iter().any(|(x, _)| x == n) {
            return err(0, format!("register {reg} has no bit {n}"));
        }
    }
    Ok(())
}

fn check_step(c: &Contract, s: &Step, q: bool) -> Result<(), Error> {
    let e = |m: String| err::<()>(0, m);
    let reg = |n: &str, write: bool| -> Result<&Reg, Error> {
        let Some(r) = c.reg(n) else {
            return err(0, format!("step uses undeclared register {n}"));
        };
        if write && r.access == Access::Ro {
            return err(0, format!("step writes read-only register {n}"));
        }
        if !write && r.access == Access::Wo {
            return err(0, format!("step reads write-only register {n}"));
        }
        if r.queue_notify {
            return err(
                0,
                format!("the doorbell {n} is rung by `kick`, never by a step"),
            );
        }
        let banked_q = r.bank.as_deref() == c.qsetup_selector();
        if banked_q && !q {
            return err(
                0,
                format!("{n} is banked by the queue selector: use it inside qsetup"),
            );
        }
        Ok(r)
    };
    let errn = |n: &str| {
        if c.errors.iter().any(|x| x.name == n) {
            Ok(())
        } else {
            err(0, format!("unknown error {n}"))
        }
    };
    let konst = |n: &str| {
        if c.konst(n).is_some() {
            Ok(())
        } else {
            err(0, format!("unknown constant {n}"))
        }
    };
    match s {
        Step::Write { reg: n, val } => {
            let r = reg(n, true)?;
            match val {
                Val::Num(v) => {
                    if r.width < 8 && v >> (8 * r.width) != 0 {
                        return e(format!("write {n}: {v:#x} does not fit"));
                    }
                }
                Val::Bits(b) => check_bits(c, n, b)?,
                Val::Var(_) => {}
            }
        }
        Step::Poll {
            reg: n,
            tries,
            err: x,
            ..
        } => {
            reg(n, false)?;
            konst(tries)?;
            errn(x)?;
        }
        Step::Expect { lhs, cond, err: x } => {
            errn(x)?;
            if c.reg(lhs).is_some() {
                reg(lhs, false)?;
                if let Cond::Has(h, l) = cond {
                    check_bits(c, lhs, h)?;
                    check_bits(c, lhs, l)?;
                }
            } else if !q {
                return e(format!("expect: unknown register {lhs}"));
            }
        }
        Step::StableRead {
            reg: n,
            into,
            guard,
            tries,
            err: x,
            when,
        } => {
            reg(n, false)?;
            reg(guard, false)?;
            konst(tries)?;
            errn(x)?;
            if into != "mac" || c.reg(n).unwrap().count != 6 {
                return e("class net-poll: stable-read fills `mac` from a 6-byte register".into());
            }
            if let Some(f) = when {
                if c.feature(f).is_none() {
                    return e(format!("stable-read: unknown feature {f}"));
                }
            } else if c.window(&c.reg(n).unwrap().window).unwrap().min < 6 {
                return e(
                    "stable-read of a register no window length covers needs `when FEATURE`".into(),
                );
            }
        }
        Step::Post(r) | Step::Kick(r) => {
            if c.ring(r).is_none() {
                return e(format!("unknown ring {r}"));
            }
        }
        Step::Read { reg: n, .. } => {
            reg(n, false)?;
        }
        Step::Fit { min, err: x, .. } => {
            konst(min)?;
            errn(x)?;
        }
        Step::NotifyOffset { reg: n, err: x } => {
            reg(n, false)?;
            errn(x)?;
        }
        Step::Write64 { lo, hi, what } => {
            let (l, h) = (reg(lo, true)?, reg(hi, true)?);
            if l.width != 4 || h.width != 4 || h.offset != l.offset + 4 {
                return e(format!("write64 {lo} {hi}: two adjacent 4-byte registers"));
            }
            if !["desc", "avail", "used"].contains(&what.as_str()) {
                return e(format!("write64: `{what}` is not desc, avail or used"));
            }
        }
        Step::AvailFlags(f) => {
            if f != "NO_INTERRUPT" {
                return e("avail-flags knows NO_INTERRUPT".into());
            }
        }
        Step::ArmFail | Step::Negotiate | Step::CommitFeatures | Step::Live | Step::Queues(_) => {}
    }
    Ok(())
}

impl Contract {
    /// The register qsetup selects queues with (the first qsetup write of `ring`).
    pub fn qsetup_selector(&self) -> Option<&str> {
        self.qsetup.iter().find_map(|s| match s {
            Step::Write {
                reg,
                val: Val::Var(v),
            } if v == "ring" => Some(reg.as_str()),
            _ => None,
        })
    }
}

/// Register roles the virtio-pci transport model gives behaviour to.
pub const VIRTIO_PCI_ROLES: [&str; 12] = [
    "status", "cfggen", "numq", "offer", "accept", "qsize", "qenable", "qnoff", "qdesc", "qavail",
    "qused", "config",
];

impl Contract {
    pub fn role(&self, role: &str) -> &Reg {
        self.regs
            .iter()
            .find(|r| r.role.as_deref() == Some(role))
            .expect("checked")
    }
}
