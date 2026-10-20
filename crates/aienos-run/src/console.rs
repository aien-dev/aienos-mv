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
/// `report_kind: final` is the end-of-boot report of the core image (which then resets).
pub const DEFAULT_READY_MARKERS: &[&str] = &["console> ", "report_kind: final"];

static SIGNAL_STOP: AtomicBool = AtomicBool::new(false);

/// Installs SIGINT/SIGTERM handlers that request a clean stop (Unix).
pub fn install_signal_stop() {
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

/// Raw terminal mode for the session (Unix). Restores the old mode on drop.
pub struct RawTerminal {
    #[cfg(unix)]
    saved: Option<libc::termios>,
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
        #[cfg(not(unix))]
        RawTerminal {}
    }

    pub fn is_raw(&self) -> bool {
        #[cfg(unix)]
        return self.saved.is_some();
        #[cfg(not(unix))]
        false
    }
}

impl Drop for RawTerminal {
    fn drop(&mut self) {
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
    std::thread::spawn(move || {
        use std::io::Read;
        let mut stdin = std::io::stdin();
        let mut buf = [0u8; 256];
        loop {
            match stdin.read(&mut buf) {
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

#[cfg(test)]
mod tests {
    use super::*;

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
