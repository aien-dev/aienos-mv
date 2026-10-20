//! Console pump: copies guest console bytes to the terminal and a log file,
//! and timestamps the first byte and the ready marker for boot receipts.

use std::io::{Read, Write};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

#[derive(Default, Clone, Debug)]
pub struct Timing {
    pub first_byte: Option<Duration>,
    pub marker: Option<Duration>,
    pub bytes: u64,
    pub eof: bool,
}

/// Finds `marker` across chunk boundaries with a bounded carry-over.
pub struct MarkerScan {
    marker: Vec<u8>,
    tail: Vec<u8>,
}

impl MarkerScan {
    pub fn new(marker: &str) -> Self {
        MarkerScan { marker: marker.as_bytes().to_vec(), tail: Vec::new() }
    }

    /// True when the marker completes inside (tail + chunk).
    pub fn feed(&mut self, chunk: &[u8]) -> bool {
        if self.marker.is_empty() {
            return false;
        }
        self.tail.extend_from_slice(chunk);
        let found = self.tail.windows(self.marker.len()).any(|w| w == self.marker.as_slice());
        let keep = self.marker.len().saturating_sub(1);
        if self.tail.len() > keep {
            let cut = self.tail.len() - keep;
            self.tail.drain(..cut);
        }
        found
    }
}

pub struct Pump {
    pub timing: Arc<Mutex<Timing>>,
    pub handle: std::thread::JoinHandle<()>,
}

pub fn spawn(
    mut from: Box<dyn Read + Send>,
    t0: Instant,
    marker: String,
    mut echo: Option<Box<dyn Write + Send>>,
    mut log: Option<std::fs::File>,
) -> Pump {
    let timing = Arc::new(Mutex::new(Timing::default()));
    let t = timing.clone();
    let handle = std::thread::spawn(move || {
        let mut scan = MarkerScan::new(&marker);
        let mut buf = [0u8; 4096];
        loop {
            let n = match from.read(&mut buf) {
                Ok(0) | Err(_) => {
                    t.lock().unwrap().eof = true;
                    return;
                }
                Ok(n) => n,
            };
            let now = t0.elapsed();
            let hit = scan.feed(&buf[..n]);
            {
                let mut g = t.lock().unwrap();
                g.bytes += n as u64;
                if g.first_byte.is_none() {
                    g.first_byte = Some(now);
                }
                if hit && g.marker.is_none() {
                    g.marker = Some(now);
                }
            }
            if let Some(e) = echo.as_mut() {
                let _ = e.write_all(&buf[..n]);
                let _ = e.flush();
            }
            if let Some(l) = log.as_mut() {
                let _ = l.write_all(&buf[..n]);
            }
        }
    });
    Pump { timing, handle }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn marker_found_across_chunks() {
        let mut s = MarkerScan::new("console: ready");
        assert!(!s.feed(b"boot...cons"));
        assert!(!s.feed(b"ole: rea"));
        assert!(s.feed(b"dy\r\naienos> "));
    }

    #[test]
    fn marker_not_found_in_noise_and_tail_bounded() {
        let mut s = MarkerScan::new("kernel: alive");
        for _ in 0..1000 {
            assert!(!s.feed(b"kernel: aliv_ x"));
        }
        assert!(s.tail.len() < 13);
        assert!(!MarkerScan::new("").feed(b"anything"));
    }
}
