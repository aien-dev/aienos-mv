//! User-mode networking settings shared by every backend: the guest sits behind the
//! host's own network stack (no root, no bridge, no TAP), and a port-forward table maps
//! host ports to guest ports.
//!
//! The signed transport's UDP port is forwarded by default. Its number is not fixed
//! anywhere on aienos main yet (OBSERVED 2026-10-20: no port constant in native/net or
//! native/sig); 47040 is PROVISIONAL, chosen clear of the iMac lab ports 47029..47035,
//! and owned by the Interplane lane (ledger L263) to confirm or change.

use std::fmt;

pub const SIGNED_TRANSPORT_UDP_PORT: u16 = 47040;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Proto {
    Tcp,
    Udp,
}

impl Proto {
    pub fn as_str(self) -> &'static str {
        match self {
            Proto::Tcp => "tcp",
            Proto::Udp => "udp",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Forward {
    pub proto: Proto,
    /// Host address to listen on; 127.0.0.1 unless the user asks for another.
    pub host_addr: String,
    pub host_port: u16,
    pub guest_port: u16,
    pub label: String,
}

impl fmt::Display for Forward {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{} {}:{} -> guest:{} ({})",
            self.proto.as_str(),
            self.host_addr,
            self.host_port,
            self.guest_port,
            self.label
        )
    }
}

impl Forward {
    /// Parses `<proto>:<host-port>:<guest-port>` or `<proto>:<addr>:<host-port>:<guest-port>`.
    pub fn parse(s: &str) -> Result<Forward, String> {
        let parts: Vec<&str> = s.split(':').collect();
        let (proto, addr, hp, gp) = match parts.as_slice() {
            [p, h, g] => (*p, "127.0.0.1", *h, *g),
            [p, a, h, g] => (*p, *a, *h, *g),
            _ => {
                return Err(format!(
                    "forward {s:?}: expected proto:host-port:guest-port"
                ))
            }
        };
        let proto = match proto {
            "tcp" => Proto::Tcp,
            "udp" => Proto::Udp,
            _ => return Err(format!("forward {s:?}: protocol must be tcp or udp")),
        };
        let port = |p: &str| -> Result<u16, String> {
            match p.parse::<u16>() {
                Ok(0) | Err(_) => Err(format!("forward {s:?}: bad port {p:?}")),
                Ok(n) => Ok(n),
            }
        };
        if addr.is_empty() || addr.contains(',') || addr.contains(' ') {
            return Err(format!("forward {s:?}: bad host address"));
        }
        Ok(Forward {
            proto,
            host_addr: addr.to_string(),
            host_port: port(hp)?,
            guest_port: port(gp)?,
            label: "user".to_string(),
        })
    }
}

/// The default table: the signed transport, UDP, same port on both sides.
pub fn default_forwards() -> Vec<Forward> {
    vec![Forward {
        proto: Proto::Udp,
        host_addr: "127.0.0.1".to_string(),
        host_port: SIGNED_TRANSPORT_UDP_PORT,
        guest_port: SIGNED_TRANSPORT_UDP_PORT,
        label: "signed transport (provisional port, L263)".to_string(),
    }]
}

/// Refuses two forwards that would bind the same host port and protocol.
pub fn check_table(table: &[Forward]) -> Result<(), String> {
    for (i, a) in table.iter().enumerate() {
        for b in &table[i + 1..] {
            if a.proto == b.proto && a.host_port == b.host_port && a.host_addr == b.host_addr {
                return Err(format!(
                    "two forwards use host {} port {}",
                    a.proto.as_str(),
                    a.host_port
                ));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_forms() {
        let f = Forward::parse("udp:47040:47040").unwrap();
        assert_eq!(f.proto, Proto::Udp);
        assert_eq!(f.host_addr, "127.0.0.1");
        let f = Forward::parse("tcp:0.0.0.0:2222:22").unwrap();
        assert_eq!(
            (f.host_addr.as_str(), f.host_port, f.guest_port),
            ("0.0.0.0", 2222, 22)
        );
        for bad in [
            "udp:1",
            "icmp:1:1",
            "tcp:0:1",
            "tcp:70000:1",
            "tcp:a,b:1:1",
            "tcp::1:1",
        ] {
            assert!(Forward::parse(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn default_has_signed_transport_and_table_check() {
        let mut t = default_forwards();
        assert_eq!(t[0].proto, Proto::Udp);
        assert_eq!(t[0].host_port, SIGNED_TRANSPORT_UDP_PORT);
        assert!(check_table(&t).is_ok());
        t.push(Forward::parse("udp:47040:9").unwrap());
        assert!(check_table(&t).is_err());
        t.pop();
        t.push(Forward::parse("tcp:47040:9").unwrap());
        assert!(check_table(&t).is_ok(), "tcp and udp may share a number");
    }
}
