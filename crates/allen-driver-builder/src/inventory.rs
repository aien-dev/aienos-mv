//! The hardware inventory: one `inventory:` line per PCI function, printed by
//! the kernel's devices stage after binding (format agreed with L267 by
//! ledger note):
//!
//! ```text
//! inventory: pci SSSS:BB:DD.F VVVV:DDDD class=CCCCCC subsys=VVVV:SSSS rev=RR bars=<i>:<addr>:<size>[,..] driver=<name|none> contract=<digest12|none> proven=<yes|no|n/a>
//! ```
//!
//! Every other line of a boot log is ignored, so a whole serial log can be
//! handed in as is.

/// One memory BAR as the kernel assigned it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Bar {
    pub index: u8,
    pub addr: u64,
    pub size: u64,
}

/// One PCI function.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Device {
    /// `SSSS:BB:DD.F`
    pub slot: String,
    pub vendor: u16,
    pub device: u16,
    /// class << 16 | subclass << 8 | progif
    pub class: u32,
    pub subsys: (u16, u16),
    pub rev: u8,
    pub bars: Vec<Bar>,
    /// The driver bound to it at boot, if any.
    pub driver: Option<String>,
    pub contract: Option<String>,
}

impl Device {
    /// A name a person understands, from the class code (PCI Code and ID
    /// Assignment Specification, class/subclass table).
    pub fn plain_name(&self) -> String {
        let what = match (self.class >> 16, (self.class >> 8) & 0xff) {
            (0x01, 0x08) => "storage controller (NVMe)",
            (0x01, 0x06) => "storage controller (SATA)",
            (0x01, _) => "storage controller",
            (0x02, _) => "network adapter",
            (0x03, _) => "display adapter (GPU)",
            (0x04, 0x03) => "sound device",
            (0x04, _) => "media device",
            (0x06, _) => "bridge",
            (0x0c, 0x03) => "USB controller",
            (0x0c, _) => "serial bus controller",
            (0x10, _) => "encryption controller",
            (0x12, _) => "processing accelerator",
            _ => match (self.vendor, self.device) {
                (0x1af4, 0x1044) | (0x1af4, 0x1005) => "random-number generator (virtio)",
                (0x1234, 0x11e8) => "test device (QEMU edu)",
                _ => "device",
            },
        };
        what.to_string()
    }

    pub fn id(&self) -> String {
        format!("{:04x}:{:04x}", self.vendor, self.device)
    }
}

fn hex<T: TryFrom<u64>>(s: &str) -> Option<T> {
    let s = s.strip_prefix("0x").unwrap_or(s);
    T::try_from(u64::from_str_radix(s, 16).ok()?).ok()
}

fn pair(s: &str) -> Option<(u16, u16)> {
    let (a, b) = s.split_once(':')?;
    Some((hex(a)?, hex(b)?))
}

/// Parse one `inventory:` line; `None` for any other line.
pub fn parse_line(line: &str) -> Option<Result<Device, String>> {
    let rest = line.trim().strip_prefix("inventory: pci ")?;
    Some(parse_fields(rest).ok_or_else(|| format!("malformed inventory line: {}", line.trim())))
}

fn parse_fields(rest: &str) -> Option<Device> {
    let mut w = rest.split_whitespace();
    let slot = w.next()?.to_string();
    let (vendor, device) = pair(w.next()?)?;
    let mut d = Device {
        slot,
        vendor,
        device,
        class: 0,
        subsys: (0, 0),
        rev: 0,
        bars: Vec::new(),
        driver: None,
        contract: None,
    };
    for f in w {
        let (k, v) = f.split_once('=')?;
        match k {
            "class" => d.class = hex(v)?,
            "subsys" => d.subsys = pair(v)?,
            "rev" => d.rev = hex(v)?,
            "bars" if v != "none" => {
                for b in v.split(',') {
                    let mut p = b.split(':');
                    d.bars.push(Bar {
                        index: p.next()?.parse().ok()?,
                        addr: hex(p.next()?)?,
                        size: hex(p.next()?)?,
                    });
                }
            }
            "driver" if v != "none" => d.driver = Some(v.to_string()),
            "contract" if v != "none" => d.contract = Some(v.to_string()),
            _ => {}
        }
    }
    Some(d)
}

/// Every device in a boot log, in the order printed.
pub fn parse(log: &str) -> Result<Vec<Device>, String> {
    log.lines().filter_map(parse_line).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    const LINE: &str =
        "inventory: pci 0000:00:02.0 1af4:1044 class=00ff00 subsys=1af4:1100 rev=01 \
                        bars=4:0x8000004000:0x4000 driver=none contract=none proven=n/a";

    #[test]
    fn a_driverless_rng_is_read_from_its_line() {
        let d = parse_line(LINE).unwrap().unwrap();
        assert_eq!(d.slot, "0000:00:02.0");
        assert_eq!(d.id(), "1af4:1044");
        assert_eq!(d.class, 0x00ff00);
        assert_eq!(d.subsys, (0x1af4, 0x1100));
        assert_eq!(d.rev, 1);
        assert_eq!(
            d.bars,
            vec![Bar {
                index: 4,
                addr: 0x80_0000_4000,
                size: 0x4000
            }]
        );
        assert_eq!(d.driver, None);
        assert_eq!(d.plain_name(), "random-number generator (virtio)");
    }

    #[test]
    fn other_boot_lines_are_ignored_and_bound_drivers_kept() {
        let log = format!(
            "pci: 00:01.0 1af4:1041 class=020000 network\n{LINE}\n\
             inventory: pci 0000:00:01.0 1af4:1041 class=020000 subsys=1af4:1100 rev=01 bars=none driver=virtio_net contract=none proven=n/a\n"
        );
        let v = parse(&log).unwrap();
        assert_eq!(v.len(), 2);
        assert_eq!(v[1].driver.as_deref(), Some("virtio_net"));
        assert!(v[1].bars.is_empty());
        assert_eq!(v[1].plain_name(), "network adapter");
    }

    #[test]
    fn a_broken_line_is_an_error_not_a_skip() {
        assert!(parse("inventory: pci 0000:00:02.0 zz:1044 class=00ff00\n").is_err());
        assert!(parse("inventory: pci 0000:00:02.0 1af4:1044 bars=4:1\n").is_err());
    }
}
