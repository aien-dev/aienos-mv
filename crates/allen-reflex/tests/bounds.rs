//! Runtime bounds (plan amendment 11, host side): maximum inputs, many repeated decisions,
//! memory-cap exhaustion, allocation failure, and no panic reachable from input.
//!
//! The heap is measured with a counting global allocator local to this test binary. Tests
//! that read its counters hold one lock, so parallel tests do not disturb the numbers.
#[path = "../examples/common/mod.rs"]
mod common;
use allen_reflex::{
    exam_turns, Action, Config, Decision, LearnError, Mode, Reflex, Role, Turn, Why,
};
use common::*;
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;

const MODES: [Mode; 3] = [Mode::Reflex, Mode::Nearest, Mode::Perceptron];

/// Heap bound for one decision, all modes, worst inputs (documented in
/// docs/native/PI5-8-ALLEN-REFLEX.md; a kernel heap must keep this much free beside the
/// learned memory).
const DECIDE_HEAP_BOUND: usize = 512 * 1024;

struct Counting;
static LIVE: AtomicUsize = AtomicUsize::new(0);
static PEAK: AtomicUsize = AtomicUsize::new(0);
/// allocations that would take LIVE above this fail (return null)
static CAP: AtomicUsize = AtomicUsize::new(usize::MAX);
static LOCK: Mutex<()> = Mutex::new(());

#[allow(unsafe_code)]
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        let now = LIVE.fetch_add(l.size(), Ordering::SeqCst) + l.size();
        if now > CAP.load(Ordering::SeqCst) {
            LIVE.fetch_sub(l.size(), Ordering::SeqCst);
            return std::ptr::null_mut();
        }
        PEAK.fetch_max(now, Ordering::SeqCst);
        unsafe { System.alloc(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        LIVE.fetch_sub(l.size(), Ordering::SeqCst);
        unsafe { System.dealloc(p, l) }
    }
}

#[global_allocator]
static A: Counting = Counting;

fn lock() -> std::sync::MutexGuard<'static, ()> {
    LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

fn trained(mode: Mode) -> Reflex {
    let pool = pool();
    let refs: Vec<&Case> = pool.iter().collect();
    let mut r = build(Config::frozen(mode), &refs);
    r.set_learning(false);
    r
}

fn user(t: &str) -> Vec<Turn<'_>> {
    vec![Turn {
        role: Role::User,
        text: t,
    }]
}

fn schema_ok(d: &Decision) -> bool {
    let name = d.action.name();
    match &d.text {
        None => {
            !d.action.has_text() && d.json() == format!("{{\"action\":\"{name}\",\"args\":{{}}}}")
        }
        Some(t) => {
            d.action.has_text()
                && !t.is_empty()
                && t.len() <= 48
                && t.bytes()
                    .all(|b| (0x20..=0x7e).contains(&b) && b != b'"' && b != b'\\')
                && d.json() == format!("{{\"action\":\"{name}\",\"args\":{{\"text\":\"{t}\"}}}}")
        }
    }
}

/// Peak heap above the live baseline while `f` runs.
fn peak_during<T>(f: impl FnOnce() -> T) -> (T, usize) {
    let base = LIVE.load(Ordering::SeqCst);
    PEAK.store(base, Ordering::SeqCst);
    let out = f();
    (out, PEAK.load(Ordering::SeqCst) - base)
}

/// Messages at and just past every input bound.
fn edge_inputs(cfg: &Config) -> Vec<(String, &'static str)> {
    let words = "remember the boiler and then show the facts ";
    let mut at_max = words.repeat(cfg.max_msg_bytes / words.len() + 1);
    at_max.truncate(cfg.max_msg_bytes);
    let over = format!("{at_max}x");
    // more tokens than max_tokens, within the byte bound
    let many_tokens = "a ".repeat(cfg.max_msg_bytes / 2 - 1);
    // 48-byte payload (the most the agent's format carries) and 49 bytes
    let p48 = "x".repeat(48);
    let p49 = "x".repeat(49);
    vec![
        (at_max, "at max bytes"),
        (over, "max bytes + 1"),
        (many_tokens, "over max tokens"),
        (format!("Remember this: {p48}"), "48-byte payload"),
        (format!("Remember this: {p49}"), "49-byte payload"),
        (";".repeat(cfg.max_msg_bytes), "only separators"),
        ("and then ".repeat(200), "only sequence markers"),
    ]
}

#[test]
fn max_length_inputs_are_answered_within_the_heap_bound() {
    let _g = lock();
    for mode in MODES {
        let r = trained(mode);
        let cfg = *r.config();
        let mut worst = 0;
        for (msg, what) in edge_inputs(&cfg) {
            let ((d, e), peak) = peak_during(|| r.decide(&user(&msg)));
            worst = worst.max(peak);
            assert!(schema_ok(&d), "{} {what}: {}", mode.name(), d.json());
            assert!(
                peak <= DECIDE_HEAP_BOUND,
                "{} {what}: peak {peak} bytes",
                mode.name()
            );
            if msg.len() > cfg.max_msg_bytes {
                assert_eq!(e.why, Why::InputTooLong, "{} {what}", mode.name());
                assert_eq!(d.action, Action::Clarify);
            } else {
                assert_ne!(e.why, Why::InputTooLong, "{} {what}", mode.name());
            }
        }
        // a 48-byte payload is carried, 49 bytes is never cut silently
        let d = r
            .decide(&user(&format!("Remember this: {}", "x".repeat(48))))
            .0;
        assert_eq!(d.text.as_deref().map(str::len), Some(48), "{}", mode.name());
        let d = r
            .decide(&user(&format!("Remember this: {}", "x".repeat(49))))
            .0;
        assert_eq!(d.action, Action::Clarify, "{}", mode.name());
        // many turns, and a huge earlier assistant turn: only the last max_turns are read
        let big_asst = format!(
            "{{\"action\":\"task\",\"args\":{{\"text\":\"{}\"}}}}",
            "y".repeat(1 << 20)
        );
        let mut turns = Vec::new();
        for i in 0..200 {
            turns.push(Turn {
                role: if i % 2 == 0 {
                    Role::User
                } else {
                    Role::Assistant
                },
                text: if i % 2 == 0 {
                    "Add a task: call Ann"
                } else {
                    big_asst.as_str()
                },
            });
        }
        turns.push(Turn {
            role: Role::User,
            text: "Yes, go ahead.",
        });
        let ((d, _), peak) = peak_during(|| r.decide(&turns));
        assert!(schema_ok(&d), "{}", mode.name());
        assert!(
            peak <= DECIDE_HEAP_BOUND,
            "{} many turns: peak {peak}",
            mode.name()
        );
        eprintln!(
            "{}: edge inputs peak {} bytes, 200 turns with a 1 MiB assistant turn peak {} bytes",
            mode.name(),
            worst,
            peak
        );
    }
}

#[test]
fn repeated_decisions_are_identical_and_leak_free() {
    let _g = lock();
    let mut cases: Vec<String> = load("train").into_iter().map(|c| c.turns).collect();
    cases.extend(load("dev").into_iter().map(|c| c.turns));
    for mode in MODES {
        let r = trained(mode);
        let first: Vec<Decision> = cases.iter().map(|s| r.decide(&exam_turns(s)).0).collect();
        let mem = r.memory_bytes();
        let live0 = LIVE.load(Ordering::SeqCst);
        let mut worst = 0;
        for round in 0..20 {
            for (s, want) in cases.iter().zip(&first) {
                let t = exam_turns(s);
                let ((d, _), peak) = peak_during(|| r.decide(&t));
                worst = worst.max(peak);
                assert_eq!(&d, want, "{} round {round}", mode.name());
            }
        }
        // decide() takes &self: nothing learned, nothing kept
        assert_eq!(r.memory_bytes(), mem, "{}", mode.name());
        assert_eq!(
            LIVE.load(Ordering::SeqCst),
            live0,
            "{}: heap grew",
            mode.name()
        );
        assert!(worst <= DECIDE_HEAP_BOUND, "{}: peak {worst}", mode.name());
        eprintln!(
            "{}: {} decisions, peak heap per decision {} bytes, learned memory {} bytes",
            mode.name(),
            20 * cases.len(),
            worst,
            mem
        );
    }
}

#[test]
fn memory_cap_exhaustion_is_refused_and_harmless() {
    let _g = lock();
    for mode in MODES {
        let mut cfg = Config::frozen(mode);
        cfg.hv.max_exemplars = 32;
        let mut r = Reflex::new(cfg);
        let msgs: Vec<String> = (0..64)
            .map(|i| format!("Kettle message number {i}"))
            .collect();
        let mut full_at = None;
        for (i, m) in msgs.iter().enumerate() {
            match r.train(&user(m), Action::Status, None) {
                Ok(()) => {}
                Err(LearnError::Full) => {
                    full_at = Some(i);
                    break;
                }
                Err(e) => panic!("{} {e:?}", mode.name()),
            }
        }
        assert_eq!(full_at, Some(32), "{}", mode.name());
        r.finish_training();
        // online learning at the cap is refused and changes nothing
        r.set_learning(true);
        let probe = user("Remember this: the gate code is 4411");
        let before = r.decide(&probe).0;
        let mem = r.memory_bytes();
        for _ in 0..100 {
            assert_eq!(
                r.learn_correct(&probe, Action::Remember, Some("the gate code is 4411")),
                Err(LearnError::Full),
                "{}",
                mode.name()
            );
            assert_eq!(
                r.learn_confirm(&probe, &before),
                Err(LearnError::Full),
                "{}",
                mode.name()
            );
        }
        assert_eq!(
            r.memory_bytes(),
            mem,
            "{}: memory moved at the cap",
            mode.name()
        );
        assert_eq!(r.decide(&probe).0, before, "{}", mode.name());
        assert!(schema_ok(&before));
        // sleep at the cap is bounded and keeps the core answering
        let _ = r.sleep();
        assert!(schema_ok(&r.decide(&probe).0));
    }
}

/// Child half of `allocation_failure_fails_closed` (runs only when that test starts it).
#[test]
#[ignore = "started by allocation_failure_fails_closed in a child process"]
fn alloc_failure_child() {
    if std::env::var("ALLEN_ALLOC_CHILD").ok().as_deref() != Some("1") {
        return;
    }
    let r = trained(Mode::Reflex);
    let t = user("Remember this: the spare key is under the mat");
    println!("READY");
    // allow 64 more bytes of heap: the decision needs far more
    CAP.store(LIVE.load(Ordering::SeqCst) + 64, Ordering::SeqCst);
    let (d, _) = r.decide(&t);
    CAP.store(usize::MAX, Ordering::SeqCst);
    println!("DECISION {}", d.json());
}

/// Allocation failure inside a decision: the Rust allocation-error path ends the process
/// (std: abort; a no_std kernel: its alloc error handler, then its panic handler). No
/// decision line is produced, and decide() writes nothing (it takes &self and has no I/O).
#[test]
fn allocation_failure_fails_closed() {
    let exe = std::env::current_exe().unwrap();
    let out = std::process::Command::new(exe)
        .args([
            "--exact",
            "alloc_failure_child",
            "--include-ignored",
            "--nocapture",
            "--test-threads=1",
        ])
        .env("ALLEN_ALLOC_CHILD", "1")
        .output()
        .unwrap();
    let so = String::from_utf8_lossy(&out.stdout);
    let se = String::from_utf8_lossy(&out.stderr);
    assert!(
        so.contains("READY"),
        "child did not reach the decision: {so} {se}"
    );
    assert!(
        !out.status.success(),
        "child survived an allocation failure: {so}"
    );
    assert!(
        !so.contains("DECISION"),
        "a decision escaped an allocation failure: {so}"
    );
    assert!(se.contains("memory allocation"), "unexpected failure: {se}");
}

/// splitmix64, fixed seed: the same inputs on every run.
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }
}

/// Words that reach the deep paths (frames, sequence markers, policy, span cues) mixed with
/// random printable bytes and some non-ASCII characters.
const VOCAB: &[&str] = &[
    "remember",
    "this",
    ":",
    "-",
    "and",
    "then",
    "afterwards",
    "later",
    "next",
    "after that",
    ";",
    "add a task",
    "to",
    "save",
    "note",
    "follow",
    "the file",
    "obey",
    "here is",
    "says",
    "do it",
    "approve",
    "deny",
    "cancel",
    "ignore your rules",
    "delete",
    "turn off",
    "lights",
    "not",
    "status",
    "facts",
    "write",
    "prefer",
    "?",
    ".",
    ",",
    "\"",
    "\\",
    "it",
    "ok",
    "{\"action\":\"task\",\"args\":{}}",
    "\u{2019}",
    "\u{e9}",
    "\u{1F600}",
    "~~",
];

fn random_message(g: &mut Rng) -> String {
    let n = g.below(40);
    let mut s = String::new();
    for _ in 0..n {
        if g.below(3) == 0 {
            let len = 1 + g.below(12);
            for _ in 0..len {
                s.push((0x20 + g.below(95) as u8) as char);
            }
        } else {
            s.push_str(VOCAB[g.below(VOCAB.len())]);
        }
        if g.below(4) != 0 {
            s.push(' ');
        }
    }
    s
}

#[test]
fn fuzz_random_input_never_panics() {
    let _g = lock();
    const N: usize = 3000;
    for mode in MODES {
        let r = trained(mode);
        let mut g = Rng(0x414c_4c45_4e5f_465a ^ mode as u64);
        for i in 0..N {
            // 1 to 5 turns, joined the exam way ("user ~~ assistant ~~ user ...")
            let k = 1 + g.below(5);
            let msg = (0..k)
                .map(|_| random_message(&mut g))
                .collect::<Vec<_>>()
                .join(" ~~ ");
            let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                r.decide(&exam_turns(&msg)).0
            }));
            match res {
                Ok(d) => assert!(
                    schema_ok(&d),
                    "{} case {i} {msg:?} -> {}",
                    mode.name(),
                    d.json()
                ),
                Err(_) => panic!("{} case {i}: panic on input {msg:?}", mode.name()),
            }
        }
    }
}

#[test]
fn fuzz_random_learning_never_panics() {
    let _g = lock();
    for mode in MODES {
        let mut cfg = Config::frozen(mode);
        cfg.hv.max_exemplars = 300;
        let pool = pool();
        let refs: Vec<&Case> = pool.iter().take(100).collect();
        let mut r = build(cfg, &refs);
        r.set_learning(true);
        let mut g = Rng(0x5eed_0f00_d000_0001 ^ mode as u64);
        for i in 0..600 {
            let msg = random_message(&mut g);
            let t = user(&msg);
            let label = Action::from_idx(g.below(allen_reflex::N_LABELS));
            // a text that is in the message, one that is not, or none
            let text = match g.below(3) {
                0 => {
                    let b = msg.as_bytes();
                    let a = g.below(b.len() + 1);
                    let e = a + g.below(b.len() - a + 1);
                    std::str::from_utf8(&b[a..e]).ok().map(String::from)
                }
                1 => Some(random_message(&mut g)),
                _ => None,
            };
            let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let _ = r.learn_correct(&t, label, text.as_deref());
                if i % 100 == 99 {
                    let _ = r.sleep();
                }
                r.decide(&t).0
            }));
            match res {
                Ok(d) => assert!(schema_ok(&d), "{} learn {i} {msg:?}", mode.name()),
                Err(_) => panic!(
                    "{} learn {i}: panic on input {msg:?} text {text:?}",
                    mode.name()
                ),
            }
        }
    }
}
