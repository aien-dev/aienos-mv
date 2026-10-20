//! The console hub between the user's terminal and the guest's serial port.
//!
//! Backends call [`Console::guest_output`] with every byte the guest writes to its
//! serial port and [`Console::pop_input`] to take bytes typed by the user. The hub
//! timestamps the first guest byte and each "ready" marker (the boot-to-console
//! receipt), mirrors output to an optional log file, and carries the stop request
//! (Ctrl+] on a terminal, SIGINT/SIGTERM otherwise) that backends turn into a clean
//! VM stop.

use std::collections::VecDeque;
use std::fs::File;
use std::io::Write;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

/// Byte that ends the session from a raw terminal (Ctrl+]).
pub const ESCAPE_BYTE: u8 = 0x1d;

/// Lines that mean "AienOS is at its console". `console> ` is the interactive prompt;
/// `report_kind: final` is the end-of-boot report of the core image (which then resets);
/// `console: ready` is the PC edition's ready line (L260 serial contract, prompt `aienos> `).
pub const DEFAULT_READY_MARKERS: &[&str] = &["console> ", "report_kind: final", "console: ready"];

static SIGNAL_STOP: AtomicBool = AtomicBool::new(false);
static SESSION_DONE: AtomicBool = AtomicBool::new(false);
static PUMP_ALIVE: AtomicBool = AtomicBool::new(false);

/// Marks the VM session finished (the Windows close-window handler waits for this).
pub fn mark_session_done() {
    SESSION_DONE.store(true, Ordering::SeqCst);
}

/// Installs handlers that request a clean stop: SIGINT/SIGTERM on Unix; Ctrl+C,
/// Ctrl+Break and closing the console window on Windows. Windows ends a process about
/// 5 s after its window is closed, so that handler waits up to 4.5 s for the stop.
pub fn install_signal_stop() {
    #[cfg(windows)]
    unsafe {
        unsafe extern "system" fn on_ctrl(ev: crate::win::DWORD) -> crate::win::BOOL {
            SIGNAL_STOP.store(true, Ordering::SeqCst);
            if ev == crate::win::CTRL_CLOSE_EVENT {
                let t0 = std::time::Instant::now();
                while !SESSION_DONE.load(Ordering::SeqCst)
                    && t0.elapsed() < Duration::from_millis(4500)
                {
                    std::thread::sleep(Duration::from_millis(20));
                }
            }
            1
        }
        crate::win::SetConsoleCtrlHandler(Some(on_ctrl), 1);
    }
    #[cfg(unix)]
    unsafe {
        extern "C" fn on_signal(_: libc::c_int) {
            SIGNAL_STOP.store(true, Ordering::SeqCst);
        }
        let h = on_signal as extern "C" fn(libc::c_int) as libc::sighandler_t;
        libc::signal(libc::SIGINT, h);
        libc::signal(libc::SIGTERM, h);
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BootTimes {
    /// When the backend reported the VM started (process spawned and connected, or
    /// partition and vCPUs created): the startup cost before any guest code runs.
    pub vmm_started: Option<Duration>,
    pub first_byte: Option<Duration>,
    /// (marker, time it was first seen)
    pub markers: Vec<(String, Option<Duration>)>,
}

impl BootTimes {
    /// The earliest ready marker seen.
    pub fn ready(&self) -> Option<(String, Duration)> {
        self.markers
            .iter()
            .filter_map(|(m, t)| t.map(|t| (m.clone(), t)))
            .min_by_key(|(_, t)| *t)
    }
}

pub struct Console {
    t0: Instant,
    input: Mutex<VecDeque<u8>>,
    stop: AtomicBool,
    out: Mutex<Option<Box<dyn Write + Send>>>,
    log: Mutex<Option<File>>,
    vmm_started: OnceLock<Duration>,
    first_byte: OnceLock<Duration>,
    markers: Vec<(String, OnceLock<Duration>)>,
    tail: Mutex<Vec<u8>>,
    stop_on_ready: bool,
    transcript: Mutex<Vec<u8>>,
    keep_transcript: bool,
}

impl Console {
    /// `out` receives guest output (None: discard). `t0` is the launch instant.
    pub fn new(out: Option<Box<dyn Write + Send>>, markers: &[String], t0: Instant) -> Console {
        Console {
            t0,
            input: Mutex::new(VecDeque::new()),
            stop: AtomicBool::new(false),
            out: Mutex::new(out),
            log: Mutex::new(None),
            vmm_started: OnceLock::new(),
            first_byte: OnceLock::new(),
            markers: markers
                .iter()
                .map(|m| (m.clone(), OnceLock::new()))
                .collect(),
            tail: Mutex::new(Vec::new()),
            stop_on_ready: false,
            transcript: Mutex::new(Vec::new()),
            keep_transcript: false,
        }
    }

    pub fn with_log(self, f: File) -> Console {
        *self.log.lock().unwrap() = Some(f);
        self
    }

    /// Requests a stop as soon as a ready marker is seen (measurement runs).
    pub fn stop_when_ready(mut self) -> Console {
        self.stop_on_ready = true;
        self
    }

    /// Keeps every guest byte in memory (tests).
    pub fn keep_transcript(mut self) -> Console {
        self.keep_transcript = true;
        self
    }

    pub fn elapsed(&self) -> Duration {
        self.t0.elapsed()
    }

    pub fn guest_output(&self, bytes: &[u8]) {
        if bytes.is_empty() {
            return;
        }
        let now = self.t0.elapsed();
        let _ = self.first_byte.set(now);
        if let Some(o) = self.out.lock().unwrap().as_mut() {
            let _ = o.write_all(bytes);
            let _ = o.flush();
        }
        if let Some(l) = self.log.lock().unwrap().as_mut() {
            let _ = l.write_all(bytes);
        }
        if self.keep_transcript {
            self.transcript.lock().unwrap().extend_from_slice(bytes);
        }
        let longest = self.markers.iter().map(|(m, _)| m.len()).max().unwrap_or(0);
        if longest == 0 {
            return;
        }
        let mut tail = self.tail.lock().unwrap();
        tail.extend_from_slice(bytes);
        let mut hit = false;
        for (m, t) in &self.markers {
            if t.get().is_none() && find(&tail, m.as_bytes()) {
                let _ = t.set(now);
                hit = true;
            }
        }
        let keep = longest.saturating_sub(1);
        if tail.len() > keep {
            let cut = tail.len() - keep;
            tail.drain(..cut);
        }
        if hit && self.stop_on_ready {
            self.request_stop();
        }
    }

    /// Backends call this once the VM is created and about to run guest code.
    pub fn vmm_started(&self) {
        let _ = self.vmm_started.set(self.t0.elapsed());
    }

    pub fn push_input(&self, bytes: &[u8]) {
        self.input.lock().unwrap().extend(bytes.iter().copied());
    }

    pub fn pop_input(&self) -> Option<u8> {
        self.input.lock().unwrap().pop_front()
    }

    pub fn has_input(&self) -> bool {
        !self.input.lock().unwrap().is_empty()
    }

    pub fn request_stop(&self) {
        self.stop.store(true, Ordering::SeqCst);
    }

    pub fn stop_requested(&self) -> bool {
        self.stop.load(Ordering::SeqCst) || SIGNAL_STOP.load(Ordering::SeqCst)
    }

    pub fn times(&self) -> BootTimes {
        BootTimes {
            vmm_started: self.vmm_started.get().copied(),
            first_byte: self.first_byte.get().copied(),
            markers: self
                .markers
                .iter()
                .map(|(m, t)| (m.clone(), t.get().copied()))
                .collect(),
        }
    }

    pub fn transcript(&self) -> Vec<u8> {
        self.transcript.lock().unwrap().clone()
    }
}

fn find(hay: &[u8], needle: &[u8]) -> bool {
    !needle.is_empty() && hay.windows(needle.len()).any(|w| w == needle)
}

/// Raw terminal mode for the session (Unix termios; Windows console modes). Restores the
/// old mode on drop.
pub struct RawTerminal {
    #[cfg(unix)]
    saved: Option<libc::termios>,
    /// (input handle, old input mode, output handle, old output mode)
    #[cfg(windows)]
    saved: Option<(crate::win::HANDLE, u32, crate::win::HANDLE, u32)>,
}

impl RawTerminal {
    pub fn enter() -> RawTerminal {
        #[cfg(unix)]
        unsafe {
            if libc::isatty(0) != 1 {
                return RawTerminal { saved: None };
            }
            let mut t: libc::termios = std::mem::zeroed();
            if libc::tcgetattr(0, &mut t) != 0 {
                return RawTerminal { saved: None };
            }
            let saved = t;
            libc::cfmakeraw(&mut t);
            // Keep output post-processing so the host terminal still turns \n into \r\n.
            t.c_oflag |= libc::OPOST;
            libc::tcsetattr(0, libc::TCSANOW, &t);
            RawTerminal { saved: Some(saved) }
        }
        #[cfg(windows)]
        unsafe {
            use crate::win::*;
            SetConsoleTitleW(wide("AienOS").as_ptr());
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);
            let (Some((hi, mi)), Some((ho, mo))) = (
                console_mode(STD_INPUT_HANDLE),
                console_mode(STD_OUTPUT_HANDLE),
            ) else {
                return RawTerminal { saved: None };
            };
            // Keys go to AienOS one at a time as VT sequences; Ctrl+C stays a console
            // event (processed input) so it becomes the clean stop, not a byte.
            let raw_in = (mi & !(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT))
                | ENABLE_VIRTUAL_TERMINAL_INPUT
                | ENABLE_PROCESSED_INPUT;
            SetConsoleMode(hi, raw_in);
            SetConsoleMode(
                ho,
                mo | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING,
            );
            RawTerminal {
                saved: Some((hi, mi, ho, mo)),
            }
        }
        #[cfg(not(any(unix, windows)))]
        RawTerminal {}
    }

    pub fn is_raw(&self) -> bool {
        #[cfg(any(unix, windows))]
        return self.saved.is_some();
        #[cfg(not(any(unix, windows)))]
        false
    }
}

impl Drop for RawTerminal {
    fn drop(&mut self) {
        #[cfg(windows)]
        if let Some((hi, mi, ho, mo)) = self.saved {
            unsafe {
                crate::win::SetConsoleMode(hi, mi);
                crate::win::SetConsoleMode(ho, mo);
            }
        }
        #[cfg(unix)]
        if let Some(t) = self.saved {
            unsafe {
                libc::tcsetattr(0, libc::TCSANOW, &t);
            }
        }
    }
}

/// Starts a thread that copies stdin into the console. Ctrl+] (raw terminal) or end of
/// input requests a stop; end of input only stops when `eof_stops` is set.
pub fn spawn_stdin_pump(console: std::sync::Arc<Console>, eof_stops: bool) {
    PUMP_ALIVE.store(true, Ordering::SeqCst);
    std::thread::spawn(move || {
        use std::io::Read;
        let mut stdin = std::io::stdin();
        let mut buf = [0u8; 256];
        let _alive = PumpAlive;
        loop {
            match stdin.read(&mut buf) {
                _ if SESSION_DONE.load(Ordering::SeqCst) => return,
                Ok(0) | Err(_) => {
                    if eof_stops {
                        console.request_stop();
                    }
                    return;
                }
                Ok(n) => {
                    if let Some(i) = buf[..n].iter().position(|b| *b == ESCAPE_BYTE) {
                        console.push_input(&buf[..i]);
                        console.request_stop();
                        return;
                    }
                    console.push_input(&buf[..n]);
                }
            }
        }
    });
}

/// Clears PUMP_ALIVE however the pump thread ends.
struct PumpAlive;

impl Drop for PumpAlive {
    fn drop(&mut self) {
        PUMP_ALIVE.store(false, Ordering::SeqCst);
    }
}

/// A writer that passes UTF-8 through and turns every invalid byte sequence into U+FFFD.
/// A sequence split across two writes is held back until it completes. The Windows
/// console rejects a whole write that is not valid UTF-8, so guest output (which may hold
/// any bytes) goes through this on Windows.
pub struct Utf8Lossy<W: Write> {
    inner: W,
    pending: Vec<u8>,
}

impl<W: Write> Utf8Lossy<W> {
    pub fn new(inner: W) -> Utf8Lossy<W> {
        Utf8Lossy {
            inner,
            pending: Vec::new(),
        }
    }

    pub fn into_inner(self) -> W {
        self.inner
    }
}

impl<W: Write> Write for Utf8Lossy<W> {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.pending.extend_from_slice(buf);
        let mut out = Vec::with_capacity(self.pending.len());
        let mut rest: &[u8] = &self.pending;
        loop {
            match std::str::from_utf8(rest) {
                Ok(s) => {
                    out.extend_from_slice(s.as_bytes());
                    rest = &[];
                    break;
                }
                Err(e) => {
                    let (good, bad) = rest.split_at(e.valid_up_to());
                    out.extend_from_slice(good);
                    match e.error_len() {
                        Some(n) => {
                            out.extend_from_slice("\u{FFFD}".as_bytes());
                            rest = &bad[n..];
                        }
                        // An unfinished sequence at the end: keep it for the next write.
                        None => {
                            rest = bad;
                            break;
                        }
                    }
                }
            }
        }
        let keep = rest.to_vec();
        self.pending = keep;
        self.inner.write_all(&out)?;
        Ok(buf.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.inner.flush()
    }
}

/// When aienos-run was started by a double click (Windows: it is the only process on its
/// console), the window would vanish with the last message; this asks for Enter first.
/// Returns at once everywhere else.
pub fn hold_window_on_exit() {
    #[cfg(windows)]
    if crate::win::owns_console_window() {
        eprint!("\r\nPress Enter to close this window.");
        if PUMP_ALIVE.load(Ordering::SeqCst) {
            // The session's input thread owns stdin; its next read (this Enter) ends it.
            while PUMP_ALIVE.load(Ordering::SeqCst) {
                std::thread::sleep(Duration::from_millis(20));
            }
        } else {
            let mut line = String::new();
            let _ = std::io::stdin().read_line(&mut line);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lossy(chunks: &[&[u8]]) -> Vec<u8> {
        let mut w = Utf8Lossy::new(Vec::new());
        for c in chunks {
            assert_eq!(w.write(c).unwrap(), c.len());
        }
        w.into_inner()
    }

    #[test]
    fn utf8_lossy_passes_text_and_replaces_garbage() {
        assert_eq!(lossy(&[b"console: ready\r\n"]), b"console: ready\r\n");
        // 0xff is never valid; a lone continuation byte neither.
        assert_eq!(lossy(&[b"a\xffb\x80c"]), "a\u{FFFD}b\u{FFFD}c".as_bytes());
        // "é" (c3 a9) split across writes stays one character.
        assert_eq!(lossy(&[b"caf\xc3", b"\xa9!"]), "café!".as_bytes());
        // A three-byte sequence cut by an ASCII byte is one replacement, then the byte.
        assert_eq!(lossy(&[b"\xe2\x82", b"x"]), "\u{FFFD}x".as_bytes());
    }

    #[test]
    fn pc_edition_ready_line_is_a_marker() {
        let m: Vec<String> = DEFAULT_READY_MARKERS
            .iter()
            .map(|s| s.to_string())
            .collect();
        let c = Console::new(None, &m, Instant::now());
        c.guest_output(b"kernel: alive\r\nconsole: ready\r\naienos> ");
        assert_eq!(c.times().ready().unwrap().0, "console: ready");
    }

    #[test]
    fn markers_across_chunks_and_first_byte() {
        let c = Console::new(None, &["report_kind: final".to_string()], Instant::now())
            .keep_transcript();
        assert!(c.times().first_byte.is_none());
        c.guest_output(b"boot...\nreport_kind: fi");
        assert!(c.times().first_byte.is_some());
        assert!(c.times().ready().is_none());
        c.guest_output(b"nal\n");
        let (m, _) = c.times().ready().unwrap();
        assert_eq!(m, "report_kind: final");
        assert_eq!(c.transcript(), b"boot...\nreport_kind: final\n");
    }

    #[test]
    fn stop_when_ready_and_input_queue() {
        let c = Console::new(None, &["console> ".to_string()], Instant::now()).stop_when_ready();
        assert!(!c.stop_requested());
        c.push_input(b"hi");
        assert_eq!(c.pop_input(), Some(b'h'));
        assert!(c.has_input());
        c.guest_output(b"console> ");
        assert!(c.stop_requested());
    }
}
