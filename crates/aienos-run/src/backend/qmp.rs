//! The QEMU Machine Protocol (QMP) client the QEMU backend uses for a clean stop, and the
//! stop sequence itself.
//!
//! QMP is line-delimited JSON over a socket: QEMU sends a greeting (`{"QMP": ...}`), the
//! client answers `qmp_capabilities`, then sends commands and reads `{"return": ...}` or
//! `{"error": ...}` lines, with asynchronous `{"event": ...}` lines in between. aienos-run
//! only needs three commands, so it writes them as fixed strings and recognises replies by
//! their top-level key; no JSON library.
//!
//! Clean stop (Ctrl+C, Ctrl+], closing the window, `--timeout`): press the ACPI power
//! button (`system_powerdown`; AienOS goes to ACPI S5 and QEMU exits by itself), then type
//! `poweroff` on the console (the L260 serial contract), then ask QEMU to quit, then kill.

use std::io::{self, BufRead, BufReader, Write};
use std::net::TcpStream;
use std::time::Duration;

/// One step of the stop sequence.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StopAction {
    /// QMP `system_powerdown`: the ACPI power button.
    PowerButton,
    /// Type `poweroff` + Enter on the guest console.
    TypePoweroff,
    /// QMP `quit`: QEMU stops at once (the guest gets no say).
    Quit,
    /// Terminate the QEMU process.
    Kill,
}

/// When each step fires, measured from the stop request.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StopPlan {
    pub type_poweroff_after: Duration,
    pub quit_after: Duration,
    pub kill_after: Duration,
}

impl Default for StopPlan {
    /// 10 s for the power button (the L260 contract: S5 within 10 s), 5 s more for the
    /// typed `poweroff`, 3 s for QMP `quit`, then kill.
    fn default() -> Self {
        StopPlan {
            type_poweroff_after: Duration::from_secs(10),
            quit_after: Duration::from_secs(15),
            kill_after: Duration::from_secs(18),
        }
    }
}

/// Hands out each [`StopAction`] once, in order, when its time has come.
#[derive(Debug, Clone)]
pub struct Stopper {
    plan: StopPlan,
    next: usize,
}

impl Stopper {
    pub fn new(plan: StopPlan) -> Stopper {
        Stopper { plan, next: 0 }
    }

    /// `since_stop` is the time since the stop was requested. Returns the next due action.
    pub fn poll(&mut self, since_stop: Duration) -> Option<StopAction> {
        let steps = [
            (Duration::ZERO, StopAction::PowerButton),
            (self.plan.type_poweroff_after, StopAction::TypePoweroff),
            (self.plan.quit_after, StopAction::Quit),
            (self.plan.kill_after, StopAction::Kill),
        ];
        let (at, action) = *steps.get(self.next)?;
        if since_stop >= at {
            self.next += 1;
            Some(action)
        } else {
            None
        }
    }
}

/// The JSON line for a QMP command without arguments.
pub fn command_line(name: &str) -> String {
    format!("{{\"execute\":\"{name}\"}}\n")
}

/// True for QEMU's greeting line.
pub fn is_greeting(line: &str) -> bool {
    line.trim_start().starts_with("{\"QMP\"")
}

/// `Some(Ok)` for a `return` reply, `Some(Err(text))` for an `error` reply, None for an
/// event or anything else.
pub fn reply(line: &str) -> Option<Result<(), String>> {
    let l = line.trim_start();
    if l.starts_with("{\"return\"") {
        Some(Ok(()))
    } else if l.starts_with("{\"error\"") {
        Some(Err(l.trim_end().to_string()))
    } else {
        None
    }
}

/// A connected QMP session.
pub struct Qmp {
    reader: BufReader<TcpStream>,
    writer: TcpStream,
}

impl Qmp {
    /// Reads the greeting and negotiates capabilities.
    pub fn handshake(stream: TcpStream, timeout: Duration) -> io::Result<Qmp> {
        stream.set_read_timeout(Some(timeout))?;
        let writer = stream.try_clone()?;
        let mut q = Qmp {
            reader: BufReader::new(stream),
            writer,
        };
        let mut line = String::new();
        q.reader.read_line(&mut line)?;
        if !is_greeting(&line) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("not a QMP greeting: {}", line.trim_end()),
            ));
        }
        q.execute("qmp_capabilities")?;
        Ok(q)
    }

    /// Sends a command and waits for its reply (events in between are skipped).
    pub fn execute(&mut self, name: &str) -> io::Result<()> {
        self.writer.write_all(command_line(name).as_bytes())?;
        self.writer.flush()?;
        loop {
            let mut line = String::new();
            if self.reader.read_line(&mut line)? == 0 {
                return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "QMP closed"));
            }
            match reply(&line) {
                Some(Ok(())) => return Ok(()),
                Some(Err(e)) => return Err(io::Error::other(e)),
                None => continue,
            }
        }
    }

    /// Sends a command without waiting (QEMU may exit before it answers `quit`).
    pub fn send(&mut self, name: &str) -> io::Result<()> {
        self.writer.write_all(command_line(name).as_bytes())?;
        self.writer.flush()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;
    use std::net::TcpListener;

    #[test]
    fn stop_sequence_in_order_once() {
        let mut s = Stopper::new(StopPlan::default());
        assert_eq!(s.poll(Duration::ZERO), Some(StopAction::PowerButton));
        assert_eq!(s.poll(Duration::from_secs(1)), None);
        assert_eq!(
            s.poll(Duration::from_secs(10)),
            Some(StopAction::TypePoweroff)
        );
        assert_eq!(s.poll(Duration::from_secs(10)), None);
        // A late poll still hands the remaining steps out one at a time.
        assert_eq!(s.poll(Duration::from_secs(60)), Some(StopAction::Quit));
        assert_eq!(s.poll(Duration::from_secs(60)), Some(StopAction::Kill));
        assert_eq!(s.poll(Duration::from_secs(99)), None);
    }

    #[test]
    fn reply_kinds() {
        assert!(is_greeting("{\"QMP\": {\"version\": {}}}\r\n"));
        assert!(!is_greeting("{\"return\": {}}"));
        assert_eq!(reply("{\"return\": {}}\r\n"), Some(Ok(())));
        assert!(matches!(
            reply("{\"error\": {\"class\": \"x\"}}"),
            Some(Err(_))
        ));
        assert_eq!(reply("{\"event\": \"POWERDOWN\"}"), None);
        assert_eq!(command_line("quit"), "{\"execute\":\"quit\"}\n");
    }

    /// A fake QEMU on a real loopback socket: greeting, capabilities, an event before the
    /// reply, then the power button.
    #[test]
    fn handshake_and_command_over_a_socket() {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = l.local_addr().unwrap();
        let fake = std::thread::spawn(move || {
            let (mut s, _) = l.accept().unwrap();
            s.write_all(b"{\"QMP\": {\"version\": {}, \"capabilities\": []}}\r\n")
                .unwrap();
            let mut got = Vec::new();
            let mut b = [0u8; 1];
            let mut lines = 0;
            while lines < 2 {
                s.read_exact(&mut b).unwrap();
                got.push(b[0]);
                if b[0] == b'\n' {
                    lines += 1;
                    if lines == 1 {
                        s.write_all(b"{\"return\": {}}\r\n").unwrap();
                    } else {
                        s.write_all(b"{\"event\": \"POWERDOWN\"}\r\n{\"return\": {}}\r\n")
                            .unwrap();
                    }
                }
            }
            String::from_utf8(got).unwrap()
        });
        let s = TcpStream::connect(addr).unwrap();
        let mut q = Qmp::handshake(s, Duration::from_secs(5)).unwrap();
        q.execute("system_powerdown").unwrap();
        let seen = fake.join().unwrap();
        assert_eq!(
            seen,
            "{\"execute\":\"qmp_capabilities\"}\n{\"execute\":\"system_powerdown\"}\n"
        );
    }
}
