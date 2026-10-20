//! User-mode network for backends that hand us raw Ethernet frames (VZ's
//! VZFileHandleNetworkDeviceAttachment over a datagram socket pair: one
//! datagram = one frame). Same addresses as QEMU user networking, because the
//! kernel's contract is written against them (native/kernel/dev/net_udp.h):
//! guest 10.0.2.15, gateway 10.0.2.2 = this host's loopback.
//!
//! What it does, and nothing more:
//! - answers ARP for 10.0.2.2;
//! - UDP from the guest to 10.0.2.2:P goes to 127.0.0.1:P, to any other
//!   address goes there unchanged (one host socket per guest source port;
//!   datagrams come back only from addresses that socket has sent to);
//! - port forwards: host 127.0.0.1:H -> guest 10.0.2.15:G, with replies from
//!   guest port G going back to the sender;
//! - no TCP, no ICMP, no DHCP, no DNS, no IP fragments (dropped, counted).
//!
//! The frame code (`Nat`) is pure and unit-tested; `UserNet` runs it on
//! threads.

use std::collections::{HashMap, HashSet};
use std::io;
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, UdpSocket};
use std::os::unix::net::UnixDatagram;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use crate::net::{Forward, Proto};

pub const GUEST_IP: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 15);
pub const GATEWAY_IP: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 2);
/// Locally administered, the same value QEMU slirp uses for its gateway.
pub const GATEWAY_MAC: [u8; 6] = [0x52, 0x55, 0x0a, 0x00, 0x02, 0x02];
/// Largest Ethernet frame (MTU 1500, no FCS, no VLAN).
pub const FRAME_MAX: usize = 1514;
/// Ethernet minimum without FCS. Shorter frames are padded: VZ stopped
/// passing frames after a 42-byte ARP reply (OBSERVED, macOS 26.6.2).
pub const FRAME_MIN: usize = 60;
const UDP_PAYLOAD_MAX: usize = 1500 - 20 - 8;

/// What the guest's frame asks the host to do.
#[derive(Debug, PartialEq, Eq)]
pub enum Action {
    /// Send this frame back to the guest.
    ToGuest(Vec<u8>),
    /// Send `payload` from the guest's UDP port `sport` to `dst` on the host.
    Udp {
        sport: u16,
        dst: SocketAddrV4,
        payload: Vec<u8>,
    },
    Drop(&'static str),
}

/// Frame parsing and building for one guest.
#[derive(Default)]
pub struct Nat {
    pub guest_mac: Option<[u8; 6]>,
    ip_id: u16,
}

fn csum_add(mut sum: u32, b: &[u8]) -> u32 {
    let mut i = 0;
    while i + 1 < b.len() {
        sum += u32::from(u16::from_be_bytes([b[i], b[i + 1]]));
        i += 2;
    }
    if i < b.len() {
        sum += u32::from(b[i]) << 8;
    }
    sum
}

fn csum_fold(mut sum: u32) -> u16 {
    while sum > 0xffff {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

fn udp_csum(src: Ipv4Addr, dst: Ipv4Addr, udp: &[u8]) -> u16 {
    let mut s = csum_add(0, &src.octets());
    s = csum_add(s, &dst.octets());
    s += 17 + udp.len() as u32;
    s = csum_add(s, udp);
    match csum_fold(s) {
        0 => 0xffff,
        c => c,
    }
}

impl Nat {
    /// Host address the guest means by `ip`: the gateway is our loopback.
    pub fn host_addr(ip: Ipv4Addr) -> Option<Ipv4Addr> {
        let o = ip.octets();
        if ip == GATEWAY_IP {
            Some(Ipv4Addr::LOCALHOST)
        } else if (o[0], o[1], o[2]) == (10, 0, 2)
            || ip.is_broadcast()
            || ip.is_multicast()
            || ip.is_unspecified()
        {
            None // the virtual subnet has nothing else on it
        } else {
            Some(ip)
        }
    }

    /// Guest-visible source for a datagram the host received from `from`.
    pub fn guest_view(from: SocketAddrV4) -> SocketAddrV4 {
        if from.ip().is_loopback() {
            SocketAddrV4::new(GATEWAY_IP, from.port())
        } else {
            from
        }
    }

    pub fn from_guest(&mut self, f: &[u8]) -> Action {
        if f.len() < 14 || f.len() > FRAME_MAX {
            return Action::Drop("frame length");
        }
        let src_mac: [u8; 6] = f[6..12].try_into().unwrap();
        if src_mac[0] & 1 != 0 {
            return Action::Drop("multicast source MAC");
        }
        match u16::from_be_bytes([f[12], f[13]]) {
            0x0806 => self.arp(f, src_mac),
            0x0800 => self.ipv4(&f[14..], src_mac),
            _ => Action::Drop("ethertype"),
        }
    }

    fn arp(&mut self, f: &[u8], src_mac: [u8; 6]) -> Action {
        let a = &f[14..];
        // htype 1, ptype 0x0800, hlen 6, plen 4, op 1 (request)
        if a.len() < 28 || a[0..8] != [0, 1, 8, 0, 6, 4, 0, 1] {
            return Action::Drop("arp: not an Ethernet/IPv4 request");
        }
        let (sha, spa, tpa) = (&a[8..14], &a[14..18], &a[24..28]);
        if tpa != GATEWAY_IP.octets() {
            return Action::Drop("arp: not for the gateway");
        }
        if sha != src_mac || spa != GUEST_IP.octets() {
            return Action::Drop("arp: sender is not the guest");
        }
        self.guest_mac = Some(src_mac);
        let mut r = Vec::with_capacity(42);
        r.extend_from_slice(&src_mac);
        r.extend_from_slice(&GATEWAY_MAC);
        r.extend_from_slice(&[0x08, 0x06, 0, 1, 8, 0, 6, 4, 0, 2]);
        r.extend_from_slice(&GATEWAY_MAC);
        r.extend_from_slice(&GATEWAY_IP.octets());
        r.extend_from_slice(&src_mac);
        r.extend_from_slice(spa);
        r.resize(FRAME_MIN, 0);
        Action::ToGuest(r)
    }

    fn ipv4(&mut self, p: &[u8], src_mac: [u8; 6]) -> Action {
        if p.len() < 20 || p[0] >> 4 != 4 {
            return Action::Drop("ipv4: header");
        }
        let ihl = usize::from(p[0] & 0xf) * 4;
        let total = usize::from(u16::from_be_bytes([p[2], p[3]]));
        if ihl < 20 || total < ihl || total > p.len() {
            return Action::Drop("ipv4: lengths");
        }
        if csum_fold(csum_add(0, &p[..ihl])) != 0 {
            return Action::Drop("ipv4: header checksum");
        }
        if u16::from_be_bytes([p[6], p[7]]) & 0x3fff != 0 {
            return Action::Drop("ipv4: fragment");
        }
        let src = Ipv4Addr::new(p[12], p[13], p[14], p[15]);
        let dst = Ipv4Addr::new(p[16], p[17], p[18], p[19]);
        if src != GUEST_IP {
            return Action::Drop("ipv4: source is not the guest");
        }
        if p[9] != 17 {
            return Action::Drop("ipv4: not UDP");
        }
        let u = &p[ihl..total];
        if u.len() < 8 || usize::from(u16::from_be_bytes([u[4], u[5]])) != u.len() {
            return Action::Drop("udp: length");
        }
        if u16::from_be_bytes([u[6], u[7]]) != 0
            && csum_fold(csum_add(
                csum_add(csum_add(0, &src.octets()), &dst.octets()) + 17 + u.len() as u32,
                u,
            )) != 0
        {
            return Action::Drop("udp: checksum");
        }
        let Some(host) = Self::host_addr(dst) else {
            return Action::Drop("udp: destination not routable");
        };
        self.guest_mac = Some(src_mac);
        Action::Udp {
            sport: u16::from_be_bytes([u[0], u[1]]),
            dst: SocketAddrV4::new(host, u16::from_be_bytes([u[2], u[3]])),
            payload: u[8..].to_vec(),
        }
    }

    /// Frame for a datagram the host received from `from` (guest view, see
    /// `guest_view`) for guest port `dport`. None until the guest's MAC is
    /// known or when the payload does not fit one frame.
    pub fn to_guest(&mut self, from: SocketAddrV4, dport: u16, payload: &[u8]) -> Option<Vec<u8>> {
        let mac = self.guest_mac?;
        if payload.len() > UDP_PAYLOAD_MAX {
            return None;
        }
        let ulen = 8 + payload.len();
        let mut f = Vec::with_capacity(14 + 20 + ulen);
        f.extend_from_slice(&mac);
        f.extend_from_slice(&GATEWAY_MAC);
        f.extend_from_slice(&[0x08, 0x00]);
        self.ip_id = self.ip_id.wrapping_add(1);
        let mut ip = [0u8; 20];
        ip[0] = 0x45;
        ip[2..4].copy_from_slice(&((20 + ulen) as u16).to_be_bytes());
        ip[4..6].copy_from_slice(&self.ip_id.to_be_bytes());
        ip[6] = 0x40; // DF
        ip[8] = 64;
        ip[9] = 17;
        ip[12..16].copy_from_slice(&from.ip().octets());
        ip[16..20].copy_from_slice(&GUEST_IP.octets());
        let c = csum_fold(csum_add(0, &ip));
        ip[10..12].copy_from_slice(&c.to_be_bytes());
        f.extend_from_slice(&ip);
        let mut u = Vec::with_capacity(ulen);
        u.extend_from_slice(&from.port().to_be_bytes());
        u.extend_from_slice(&dport.to_be_bytes());
        u.extend_from_slice(&(ulen as u16).to_be_bytes());
        u.extend_from_slice(&[0, 0]);
        u.extend_from_slice(payload);
        let c = udp_csum(*from.ip(), GUEST_IP, &u);
        u[6..8].copy_from_slice(&c.to_be_bytes());
        f.extend_from_slice(&u);
        if f.len() < FRAME_MIN {
            f.resize(FRAME_MIN, 0);
        }
        Some(f)
    }
}

/// Counters, for the run log.
#[derive(Default)]
pub struct Stats {
    pub frames_from_guest: AtomicU64,
    pub frames_to_guest: AtomicU64,
    pub dropped: AtomicU64,
}

struct Fwd {
    guest: u16,
    /// Host address the forward listens on (127.0.0.1 unless the user chose another).
    bind: Ipv4Addr,
    sock: Arc<UdpSocket>,
    peers: Mutex<HashSet<SocketAddrV4>>,
}

struct Flow {
    sock: Arc<UdpSocket>,
    sent_to: Arc<Mutex<HashSet<SocketAddrV4>>>,
}

struct Shared {
    nat: Mutex<Nat>,
    guest: UnixDatagram,
    stop: AtomicBool,
    stats: Stats,
    forwards: Vec<Fwd>,
    flows: Mutex<HashMap<u16, Flow>>,
    threads: Mutex<Vec<JoinHandle<()>>>,
    last_drop: Mutex<Option<&'static str>>,
    send_err: Mutex<Option<String>>,
}

/// A running user-mode network. Dropping it stops its threads.
pub struct UserNet {
    sh: Arc<Shared>,
}

/// Socket buffers for frames both ways: Apple's attachment documentation
/// asks for a receive buffer at least twice the send buffer.
fn set_buffers(s: &UnixDatagram) -> io::Result<()> {
    use std::os::fd::AsRawFd;
    extern "C" {
        fn setsockopt(
            fd: i32,
            level: i32,
            name: i32,
            val: *const core::ffi::c_void,
            len: u32,
        ) -> i32;
    }
    const SOL_SOCKET: i32 = 0xffff;
    const SO_SNDBUF: i32 = 0x1001;
    const SO_RCVBUF: i32 = 0x1002;
    for (opt, v) in [(SO_SNDBUF, 128 * 1024i32), (SO_RCVBUF, 512 * 1024i32)] {
        // SAFETY: setsockopt reads sizeof(int) bytes from &v.
        if unsafe { setsockopt(s.as_raw_fd(), SOL_SOCKET, opt, (&v as *const i32).cast(), 4) } != 0
        {
            return Err(io::Error::last_os_error());
        }
    }
    Ok(())
}

const POLL: Duration = Duration::from_millis(200);

impl UserNet {
    /// Binds every UDP forward on 127.0.0.1 and returns the network plus the
    /// socket end the hypervisor gets (one Ethernet frame per datagram).
    pub fn start(forwards: &[Forward]) -> io::Result<(UserNet, UnixDatagram)> {
        let (vm_end, ours) = UnixDatagram::pair()?;
        // VZ drains its end until EAGAIN: a blocking end hangs its network
        // queue after the first frame we send (OBSERVED, macOS 26.6.2).
        vm_end.set_nonblocking(true)?;
        set_buffers(&vm_end)?;
        set_buffers(&ours)?;
        ours.set_read_timeout(Some(POLL))?;
        let mut fw = Vec::new();
        for f in forwards {
            if f.proto != Proto::Udp {
                return Err(io::Error::new(
                    io::ErrorKind::Unsupported,
                    format!("{f}: the macOS user network forwards UDP only (yet)"),
                ));
            }
            let addr: Ipv4Addr = f.host_addr.parse().map_err(|_| {
                io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("{f}: host address must be IPv4"),
                )
            })?;
            let s = UdpSocket::bind(SocketAddrV4::new(addr, f.host_port))
                .map_err(|e| io::Error::new(e.kind(), format!("forward {f}: {e}")))?;
            s.set_read_timeout(Some(POLL))?;
            fw.push(Fwd {
                guest: f.guest_port,
                bind: addr,
                sock: Arc::new(s),
                peers: Mutex::new(HashSet::new()),
            });
        }
        let sh = Arc::new(Shared {
            nat: Mutex::new(Nat::default()),
            guest: ours,
            stop: AtomicBool::new(false),
            stats: Stats::default(),
            forwards: fw,
            flows: Mutex::new(HashMap::new()),
            threads: Mutex::new(Vec::new()),
            last_drop: Mutex::new(None),
            send_err: Mutex::new(None),
        });
        let mut t = Vec::new();
        let s = sh.clone();
        t.push(std::thread::spawn(move || guest_loop(&s)));
        for i in 0..sh.forwards.len() {
            let s = sh.clone();
            t.push(std::thread::spawn(move || forward_loop(&s, i)));
        }
        sh.threads.lock().unwrap().extend(t);
        Ok((UserNet { sh }, vm_end))
    }

    pub fn stats(&self) -> &Stats {
        &self.sh.stats
    }

    /// Host ports actually bound, in forward order.
    pub fn forward_ports(&self) -> Vec<u16> {
        self.sh
            .forwards
            .iter()
            .filter_map(|f| f.sock.local_addr().ok().map(|a| a.port()))
            .collect()
    }
}

impl Drop for UserNet {
    fn drop(&mut self) {
        self.sh.stop.store(true, Ordering::SeqCst);
        if std::env::var_os("AIENOS_RUN_NET_DEBUG").is_some() {
            let s = &self.sh.stats;
            eprintln!(
                "usernet: from_guest={} to_guest={} dropped={} last_drop={:?} send_err={:?}",
                s.frames_from_guest.load(Ordering::Relaxed),
                s.frames_to_guest.load(Ordering::Relaxed),
                s.dropped.load(Ordering::Relaxed),
                self.sh.last_drop.lock().unwrap(),
                self.sh.send_err.lock().unwrap()
            );
        }
        let t: Vec<_> = self.sh.threads.lock().unwrap().drain(..).collect();
        for h in t {
            let _ = h.join();
        }
    }
}

fn send_guest(sh: &Shared, frame: &[u8]) {
    match sh.guest.send(frame) {
        Ok(_) => {
            sh.stats.frames_to_guest.fetch_add(1, Ordering::Relaxed);
        }
        Err(e) => *sh.send_err.lock().unwrap() = Some(e.to_string()),
    }
}

fn deliver(sh: &Shared, from: SocketAddr, dport: u16, payload: &[u8]) {
    let SocketAddr::V4(from) = from else {
        sh.stats.dropped.fetch_add(1, Ordering::Relaxed);
        return;
    };
    let f = sh
        .nat
        .lock()
        .unwrap()
        .to_guest(Nat::guest_view(from), dport, payload);
    match f {
        Some(f) => send_guest(sh, &f),
        None => {
            sh.stats.dropped.fetch_add(1, Ordering::Relaxed);
        }
    }
}

fn guest_loop(sh: &Arc<Shared>) {
    let mut buf = vec![0u8; 65536];
    while !sh.stop.load(Ordering::SeqCst) {
        let n = match sh.guest.recv(&mut buf) {
            Ok(n) => n,
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) =>
            {
                continue
            }
            Err(_) => return,
        };
        sh.stats.frames_from_guest.fetch_add(1, Ordering::Relaxed);
        let a = sh.nat.lock().unwrap().from_guest(&buf[..n]);
        match a {
            Action::ToGuest(f) => send_guest(sh, &f),
            Action::Udp {
                sport,
                dst,
                payload,
            } => udp_out(sh, sport, dst, &payload),
            Action::Drop(why) => {
                sh.stats.dropped.fetch_add(1, Ordering::Relaxed);
                *sh.last_drop.lock().unwrap() = Some(why);
            }
        }
    }
}

fn udp_out(sh: &Arc<Shared>, sport: u16, dst: SocketAddrV4, payload: &[u8]) {
    // A forwarded guest port keeps one host endpoint: what it sends (replies,
    // and also first contact such as a gateway registration) leaves from the
    // forward's own host socket, so peers see the same address they dial. A
    // loopback-only forward can only reach loopback; other traffic from that
    // port falls through to a flow socket (a fresh host port).
    for f in &sh.forwards {
        if f.guest == sport && (!f.bind.is_loopback() || dst.ip().is_loopback()) {
            f.peers.lock().unwrap().insert(dst);
            let _ = f.sock.send_to(payload, dst);
            return;
        }
    }
    let mut flows = sh.flows.lock().unwrap();
    let flow = match flows.entry(sport) {
        std::collections::hash_map::Entry::Occupied(e) => e.into_mut(),
        std::collections::hash_map::Entry::Vacant(e) => {
            let Ok(s) = UdpSocket::bind(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 0)) else {
                sh.stats.dropped.fetch_add(1, Ordering::Relaxed);
                return;
            };
            let _ = s.set_read_timeout(Some(POLL));
            let flow = Flow {
                sock: Arc::new(s),
                sent_to: Arc::new(Mutex::new(HashSet::new())),
            };
            let (s2, sent, shc) = (flow.sock.clone(), flow.sent_to.clone(), sh.clone());
            sh.threads.lock().unwrap().push(std::thread::spawn(move || {
                flow_loop(&shc, &s2, &sent, sport)
            }));
            e.insert(flow)
        }
    };
    flow.sent_to.lock().unwrap().insert(dst);
    let _ = flow.sock.send_to(payload, dst);
}

fn flow_loop(sh: &Shared, s: &UdpSocket, sent_to: &Mutex<HashSet<SocketAddrV4>>, sport: u16) {
    let mut buf = vec![0u8; 65536];
    while !sh.stop.load(Ordering::SeqCst) {
        match s.recv_from(&mut buf) {
            Ok((n, SocketAddr::V4(from))) if sent_to.lock().unwrap().contains(&from) => {
                deliver(sh, SocketAddr::V4(from), sport, &buf[..n])
            }
            Ok(_) => {
                sh.stats.dropped.fetch_add(1, Ordering::Relaxed); // unsolicited
            }
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) => {}
            Err(_) => return,
        }
    }
}

fn forward_loop(sh: &Shared, i: usize) {
    let f = &sh.forwards[i];
    let mut buf = vec![0u8; 65536];
    while !sh.stop.load(Ordering::SeqCst) {
        match f.sock.recv_from(&mut buf) {
            Ok((n, SocketAddr::V4(from))) => {
                f.peers.lock().unwrap().insert(from);
                deliver(sh, SocketAddr::V4(from), f.guest, &buf[..n]);
            }
            Ok(_) => {
                sh.stats.dropped.fetch_add(1, Ordering::Relaxed);
            }
            Err(e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) => {}
            Err(_) => return,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const GMAC: [u8; 6] = [0x02, 0xa1, 0x33, 0x0a, 0xc4, 0x0d];

    fn arp_request(tpa: Ipv4Addr) -> Vec<u8> {
        let mut f = vec![0xff; 6];
        f.extend_from_slice(&GMAC);
        f.extend_from_slice(&[0x08, 0x06, 0, 1, 8, 0, 6, 4, 0, 1]);
        f.extend_from_slice(&GMAC);
        f.extend_from_slice(&GUEST_IP.octets());
        f.extend_from_slice(&[0; 6]);
        f.extend_from_slice(&tpa.octets());
        f
    }

    /// A UDP frame the way the kernel builds it (net_udp.c): guest -> dst.
    fn guest_udp(sport: u16, dst: SocketAddrV4, payload: &[u8]) -> Vec<u8> {
        let mut n = Nat {
            guest_mac: Some(GMAC),
            ip_id: 0,
        };
        // Build as if from `dst` to the guest, then swap direction fields.
        let mut f = n.to_guest(dst, sport, payload).unwrap();
        f[0..6].copy_from_slice(&GATEWAY_MAC);
        f[6..12].copy_from_slice(&GMAC);
        f[26..30].copy_from_slice(&GUEST_IP.octets());
        f[30..34].copy_from_slice(&dst.ip().octets());
        f[24..26].copy_from_slice(&[0, 0]);
        let c = csum_fold(csum_add(0, &f[14..34]));
        f[24..26].copy_from_slice(&c.to_be_bytes());
        f[34..36].copy_from_slice(&sport.to_be_bytes());
        f[36..38].copy_from_slice(&dst.port().to_be_bytes());
        f[40..42].copy_from_slice(&[0, 0]);
        let c = udp_csum(GUEST_IP, *dst.ip(), &f[34..42 + payload.len()]);
        f[40..42].copy_from_slice(&c.to_be_bytes());
        f
    }

    #[test]
    fn answers_arp_for_the_gateway_only() {
        let mut n = Nat::default();
        let Action::ToGuest(r) = n.from_guest(&arp_request(GATEWAY_IP)) else {
            panic!()
        };
        assert_eq!(&r[0..6], &GMAC);
        assert_eq!(&r[20..22], &[0, 2]); // reply
        assert_eq!(&r[22..28], &GATEWAY_MAC);
        assert_eq!(&r[28..32], &GATEWAY_IP.octets());
        assert_eq!(&r[38..42], &GUEST_IP.octets());
        assert_eq!(n.guest_mac, Some(GMAC));
        assert!(matches!(
            Nat::default().from_guest(&arp_request(Ipv4Addr::new(10, 0, 2, 3))),
            Action::Drop(_)
        ));
    }

    #[test]
    fn udp_to_gateway_goes_to_loopback() {
        let mut n = Nat::default();
        let f = guest_udp(
            47030,
            SocketAddrV4::new(GATEWAY_IP, 47029),
            b"AIENOS-CK-NET ping nonce=1",
        );
        assert_eq!(
            n.from_guest(&f),
            Action::Udp {
                sport: 47030,
                dst: SocketAddrV4::new(Ipv4Addr::LOCALHOST, 47029),
                payload: b"AIENOS-CK-NET ping nonce=1".to_vec()
            }
        );
    }

    #[test]
    fn refuses_bad_checksums_fragments_and_spoofed_sources() {
        let ok = guest_udp(1, SocketAddrV4::new(GATEWAY_IP, 2), b"x");
        let mut n = Nat::default();
        let mut f = ok.clone();
        f[24] ^= 1;
        assert_eq!(n.from_guest(&f), Action::Drop("ipv4: header checksum"));
        let mut f = ok.clone();
        f[42] ^= 1;
        assert_eq!(n.from_guest(&f), Action::Drop("udp: checksum"));
        let mut f = ok.clone();
        f[20] = 0x20; // MF
        let c = {
            f[24..26].copy_from_slice(&[0, 0]);
            csum_fold(csum_add(0, &f[14..34]))
        };
        f[24..26].copy_from_slice(&c.to_be_bytes());
        assert_eq!(n.from_guest(&f), Action::Drop("ipv4: fragment"));
        let f = guest_udp(1, SocketAddrV4::new(Ipv4Addr::new(10, 0, 2, 9), 2), b"x");
        assert_eq!(
            n.from_guest(&f),
            Action::Drop("udp: destination not routable")
        );
        assert!(matches!(n.from_guest(&ok[..20]), Action::Drop(_)));
    }

    #[test]
    fn frame_to_guest_has_valid_checksums() {
        let mut n = Nat::default();
        assert!(n
            .to_guest(SocketAddrV4::new(GATEWAY_IP, 5), 47030, b"hi")
            .is_none()); // MAC unknown
        n.guest_mac = Some(GMAC);
        let f = n
            .to_guest(SocketAddrV4::new(GATEWAY_IP, 5), 47030, b"hi")
            .unwrap();
        assert_eq!(f.len(), FRAME_MIN); // 44 bytes of frame, padded
        assert_eq!(u16::from_be_bytes([f[16], f[17]]), 20 + 8 + 2);
        assert_eq!(csum_fold(csum_add(0, &f[14..34])), 0);
        let s = csum_add(csum_add(0, &GATEWAY_IP.octets()), &GUEST_IP.octets()) + 17 + 10;
        assert_eq!(csum_fold(csum_add(s, &f[34..44])), 0);
        assert!(n
            .to_guest(
                SocketAddrV4::new(GATEWAY_IP, 5),
                1,
                &[0; UDP_PAYLOAD_MAX + 1]
            )
            .is_none());
    }

    fn recv_frame(vm: &UnixDatagram) -> Vec<u8> {
        vm.set_nonblocking(false).unwrap();
        vm.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let mut b = vec![0u8; 2048];
        let n = vm.recv(&mut b).expect("frame to guest");
        b.truncate(n);
        b
    }

    /// Interplane stub (L265 step 5): a host datagram to the forwarded port
    /// reaches the guest's 10.0.2.15:47030 and the guest's reply gets back
    /// to the host sender. Uses an ephemeral host port so it runs anywhere.
    #[test]
    fn forwarded_udp_port_reaches_the_guest_and_back() {
        let free = UdpSocket::bind("127.0.0.1:0")
            .unwrap()
            .local_addr()
            .unwrap()
            .port();
        let fw = [Forward {
            proto: Proto::Udp,
            host_addr: "127.0.0.1".into(),
            host_port: free,
            guest_port: 47030,
            label: "test".into(),
        }];
        let (net, vm) = UserNet::start(&fw).unwrap();
        // The guest speaks first (its ARP), so its MAC is known.
        vm.send(&arp_request(GATEWAY_IP)).unwrap();
        assert_eq!(&recv_frame(&vm)[20..22], &[0, 2]);

        let host = UdpSocket::bind("127.0.0.1:0").unwrap();
        host.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        host.send_to(b"interplane hello", ("127.0.0.1", free))
            .unwrap();
        let f = recv_frame(&vm);
        assert_eq!(&f[30..34], &GUEST_IP.octets());
        assert_eq!(u16::from_be_bytes([f[36], f[37]]), 47030);
        assert_eq!(&f[26..30], &GATEWAY_IP.octets()); // host loopback seen as the gateway
        assert_eq!(&f[42..58], b"interplane hello");

        let hp = host.local_addr().unwrap().port();
        vm.send(&guest_udp(47030, SocketAddrV4::new(GATEWAY_IP, hp), b"ack"))
            .unwrap();
        let mut b = [0u8; 64];
        let (n, from) = host.recv_from(&mut b).unwrap();
        assert_eq!(&b[..n], b"ack");
        assert_eq!(from.port(), free);
        drop(net);
    }

    /// Interplane (L263 docs/INTERPLANE_JOIN.md section 7): first contact from
    /// the forwarded guest port leaves from the forwarded host port, and the
    /// peer's answer to that port reaches the guest.
    #[test]
    fn forwarded_guest_port_sends_from_the_forwarded_host_port() {
        let free = UdpSocket::bind("127.0.0.1:0")
            .unwrap()
            .local_addr()
            .unwrap()
            .port();
        let fw = [Forward {
            proto: Proto::Udp,
            host_addr: "127.0.0.1".into(),
            host_port: free,
            guest_port: 47040,
            label: "test".into(),
        }];
        let (net, vm) = UserNet::start(&fw).unwrap();
        vm.send(&arp_request(GATEWAY_IP)).unwrap();
        assert_eq!(&recv_frame(&vm)[20..22], &[0, 2]);

        let gw = UdpSocket::bind("127.0.0.1:0").unwrap();
        gw.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let gp = gw.local_addr().unwrap().port();
        vm.send(&guest_udp(
            47040,
            SocketAddrV4::new(GATEWAY_IP, gp),
            b"register",
        ))
        .unwrap();
        let mut b = [0u8; 64];
        let (n, from) = gw.recv_from(&mut b).unwrap();
        assert_eq!(&b[..n], b"register");
        assert_eq!(from.port(), free, "sent from the forwarded host port");
        gw.send_to(b"welcome", from).unwrap();
        let f = recv_frame(&vm);
        assert_eq!(u16::from_be_bytes([f[34], f[35]]), gp);
        assert_eq!(u16::from_be_bytes([f[36], f[37]]), 47040);
        assert_eq!(&f[42..49], b"welcome");
        drop(net);
    }

    /// The kernel's selftest path: guest -> 10.0.2.2:P is an echo on the
    /// host's 127.0.0.1:P, the answer comes back as 10.0.2.2:P.
    #[test]
    fn outbound_udp_round_trip_through_loopback() {
        let echo = UdpSocket::bind("127.0.0.1:0").unwrap();
        let ep = echo.local_addr().unwrap().port();
        echo.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let (net, vm) = UserNet::start(&[]).unwrap();
        vm.send(&guest_udp(
            47030,
            SocketAddrV4::new(GATEWAY_IP, ep),
            b"ping",
        ))
        .unwrap();
        let mut b = [0u8; 64];
        let (n, from) = echo.recv_from(&mut b).unwrap();
        assert_eq!(&b[..n], b"ping");
        echo.send_to(b"pong", from).unwrap();
        // An unsolicited sender to the same flow socket is dropped.
        let stranger = UdpSocket::bind("127.0.0.1:0").unwrap();
        stranger.send_to(b"spoof", from).unwrap();
        let f = recv_frame(&vm);
        assert_eq!(&f[26..30], &GATEWAY_IP.octets());
        assert_eq!(u16::from_be_bytes([f[34], f[35]]), ep);
        assert_eq!(u16::from_be_bytes([f[36], f[37]]), 47030);
        assert_eq!(&f[42..46], b"pong");
        vm.set_read_timeout(Some(Duration::from_millis(400)))
            .unwrap();
        assert!(
            vm.recv(&mut [0u8; 64]).is_err(),
            "unsolicited datagram reached the guest"
        );
        drop(net);
    }

    #[test]
    fn tcp_forward_is_refused_not_ignored() {
        let e = UserNet::start(&[Forward::parse("tcp:1:1").unwrap()])
            .err()
            .unwrap();
        assert_eq!(e.kind(), io::ErrorKind::Unsupported);
    }
}
