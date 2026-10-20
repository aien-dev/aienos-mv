//! The peer of the kernel's network self-test, answered by aienos-run itself.
//!
//! The kernel sends `AIENOS-CK-NET ping nonce=<8 hex>` to 10.0.2.2:47029 and
//! passes when `AIENOS-CK-NET pong token=<token> echo=<the ping>` comes back
//! (native/kernel/dev/net_bind.c; the lab helper native/kernel/tools/ck_udp_echo.c
//! gives the same answer). Nothing outside the runner takes part:
//! - vz: the user-mode network (crate::usernet) answers the frame in-process;
//!   the ping never reaches a host socket;
//! - qemu: QEMU's user network delivers the ping to 127.0.0.1:47029, where
//!   [`LoopbackEcho`] answers it (loopback only, for as long as the VM runs).
//!
//! The token is fresh per run, so a pass cannot come from a stale answer.

use std::io::{self, Read};
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;

/// The kernel's echo port (native/kernel/dev/net_udp.h CK_NET_ECHO_PORT).
pub const SELFTEST_PORT: u16 = 47029;
const PING: &[u8] = b"AIENOS-CK-NET ping ";

/// 16 hex digits from the system's random source (a fixed fallback is never used).
pub fn new_token() -> io::Result<String> {
    let mut b = [0u8; 8];
    std::fs::File::open("/dev/urandom")?.read_exact(&mut b)?;
    Ok(b.iter().map(|x| format!("{x:02x}")).collect())
}

/// The answer to `ping`, or None when it is not the kernel's self-test ping.
pub fn pong(token: &str, ping: &[u8]) -> Option<Vec<u8>> {
    if !ping.starts_with(PING) {
        return None;
    }
    let mut r = format!("AIENOS-CK-NET pong token={token} echo=").into_bytes();
    r.extend_from_slice(ping);
    Some(r)
}

/// Answers the self-test on 127.0.0.1:47029 (the qemu backend). The thread
/// blocks in recv; dropping this wakes it with one datagram and joins it.
pub struct LoopbackEcho {
    stop: Arc<AtomicBool>,
    addr: SocketAddr,
    thread: Option<JoinHandle<()>>,
}

impl LoopbackEcho {
    /// Err when the port is taken (a lab helper or another runner answers it then).
    pub fn start(token: String) -> io::Result<LoopbackEcho> {
        let s = UdpSocket::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, SELFTEST_PORT))?;
        let addr = s.local_addr()?;
        let stop = Arc::new(AtomicBool::new(false));
        let st = stop.clone();
        let thread = std::thread::spawn(move || {
            let mut buf = [0u8; 2048];
            while let Ok((n, from)) = s.recv_from(&mut buf) {
                if st.load(Ordering::SeqCst) {
                    return;
                }
                if let Some(r) = pong(&token, &buf[..n]) {
                    let _ = s.send_to(&r, from);
                }
            }
        });
        Ok(LoopbackEcho {
            stop,
            addr,
            thread: Some(thread),
        })
    }
}

impl Drop for LoopbackEcho {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Ok(s) = UdpSocket::bind("127.0.0.1:0") {
            let _ = s.send_to(b"", self.addr);
        }
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn pong_answers_only_the_selftest_ping() {
        let r = pong("feedf00d", b"AIENOS-CK-NET ping nonce=0123abcd").unwrap();
        assert_eq!(
            r,
            b"AIENOS-CK-NET pong token=feedf00d echo=AIENOS-CK-NET ping nonce=0123abcd"
        );
        assert!(pong("feedf00d", b"hello").is_none());
    }

    #[test]
    fn tokens_are_fresh_hex() {
        let (a, b) = (new_token().unwrap(), new_token().unwrap());
        assert_eq!(a.len(), 16);
        assert!(a.bytes().all(|c| c.is_ascii_hexdigit()));
        assert_ne!(a, b);
    }

    /// Runs only when 47029 is free on this host (a lab helper may hold it).
    #[test]
    fn loopback_echo_answers_and_stops() {
        let Ok(e) = LoopbackEcho::start("ab".into()) else {
            return;
        };
        let c = UdpSocket::bind("127.0.0.1:0").unwrap();
        c.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        c.send_to(b"AIENOS-CK-NET ping nonce=1", ("127.0.0.1", SELFTEST_PORT))
            .unwrap();
        let mut b = [0u8; 128];
        let n = c.recv(&mut b).unwrap();
        assert_eq!(
            &b[..n],
            b"AIENOS-CK-NET pong token=ab echo=AIENOS-CK-NET ping nonce=1"
        );
        drop(e);
        assert!(UdpSocket::bind(("127.0.0.1", SELFTEST_PORT)).is_ok());
    }
}
