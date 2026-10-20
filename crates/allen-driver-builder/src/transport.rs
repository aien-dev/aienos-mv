//! The part of a contract the language model never writes.
//!
//! The chip model is generated from the contract under test, so a wrong
//! register offset would pass its own model. The builder therefore supplies
//! the transport block itself: identity (`devc`, `device`, `prefix`, `class`,
//! `transport`, `ring-kind`), the `match` lines taken from the device the
//! kernel saw, the `proven` line (always `pending`), the four virtio-pci
//! windows, the common-configuration registers at their virtio 1.2 4.1.4.3
//! offsets, the status bits, and the protocol rules the chip model enforces.
//! The model writes only the device part that follows. A device part that
//! tries to restate any of these is refused before the compiler sees it.

use crate::inventory::Device;

/// A contract class the contract compiler (devc 1) can build, chosen from the
/// device's identity.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Class {
    pub class: &'static str,
    pub prefix: &'static str,
    pub device: &'static str,
    /// The C header the generator writes for this class (the kernel bind includes it).
    pub header: &'static str,
}

/// Modern virtio PCI ids are 0x1040 + the virtio device id (virtio 1.2 4.1.2.1).
pub fn class_for(d: &Device) -> Option<Class> {
    if d.vendor != 0x1af4 || d.device < 0x1040 {
        return None;
    }
    match d.device - 0x1040 {
        4 => Some(Class {
            class: "entropy-poll",
            prefix: "vrng",
            device: "virtio-rng",
            header: "aienos_virtio_rng.h",
        }),
        1 => Some(Class {
            class: "net-poll",
            prefix: "vnet",
            device: "virtio-net",
            header: "aienos_virtio_net.h",
        }),
        _ => None,
    }
}

/// The fixed block for `d` (class `c`), ending in a newline.
pub fn block(d: &Device, c: &Class, platform: &str, machine: &str, date: &str) -> String {
    format!(
        "# ---- transport block: written by Allen's driver builder, never by the language model ----\n\
devc 1\n\
device {dev}\n\
prefix {prefix}\n\
class {class}\n\
transport virtio-pci\n\
ring-kind virtq-split\n\
match pci 0x{v:04x} 0x{id:04x}\n\
proven {platform} {machine} {date} AIENOS_CK_DRIVER_BUILDER pending\n\
window common 0 virtio-cap 1 min 0x38 tag CC\n\
window notify 1 virtio-cap 2 min 2 multiplier\n\
window isr 2 virtio-cap 3 min 0\n\
window device 3 virtio-cap 4 min 0\n\
reg common DFSELECT 0x00 4 rw reset 0\n\
reg common DF 0x04 4 ro bank DFSELECT role offer\n\
reg common GFSELECT 0x08 4 rw reset 0\n\
reg common GF 0x0c 4 rw bank GFSELECT role accept\n\
reg common MSIX 0x10 2 rw reset 0xffff\n\
reg common NUMQ 0x12 2 ro role numq\n\
reg common STATUS 0x14 1 rw reset 0 role status\n\
reg common CFGGEN 0x15 1 ro role cfggen\n\
reg common Q_SELECT 0x16 2 rw reset 0\n\
reg common Q_SIZE 0x18 2 rw bank Q_SELECT role qsize\n\
reg common Q_MSIX 0x1a 2 rw bank Q_SELECT reset 0xffff\n\
reg common Q_ENABLE 0x1c 2 rw bank Q_SELECT reset 0 role qenable\n\
reg common Q_NOFF 0x1e 2 ro bank Q_SELECT role qnoff\n\
reg common Q_DESCLO 0x20 4 rw bank Q_SELECT role qdesc\n\
reg common Q_DESCHI 0x24 4 rw bank Q_SELECT\n\
reg common Q_AVAILLO 0x28 4 rw bank Q_SELECT role qavail\n\
reg common Q_AVAILHI 0x2c 4 rw bank Q_SELECT\n\
reg common Q_USEDLO 0x30 4 rw bank Q_SELECT role qused\n\
reg common Q_USEDHI 0x34 4 rw bank Q_SELECT\n\
reg notify DOORBELL 0x00 2 wo queue-notify\n\
bits STATUS S ACK=0 DRIVER=1 DRIVER_OK=2 FEATURES_OK=3 NEEDS_RESET=6 FAILED=7\n\
failmark STATUS FAILED\n\
rule GF write requires-status DRIVER lacks FEATURES_OK\n\
rule Q_SIZE write requires-status FEATURES_OK lacks DRIVER_OK\n\
rule Q_ENABLE write requires-status FEATURES_OK lacks DRIVER_OK\n\
rule Q_DESCLO write requires-status FEATURES_OK lacks DRIVER_OK\n\
rule Q_AVAILLO write requires-status FEATURES_OK lacks DRIVER_OK\n\
rule Q_USEDLO write requires-status FEATURES_OK lacks DRIVER_OK\n\
rule DOORBELL write requires-status DRIVER_OK lacks FAILED\n\
# ---- device part: written by the language model ----\n",
        dev = c.device,
        prefix = c.prefix,
        class = c.class,
        v = d.vendor,
        id = d.device,
    )
}

/// Statements only the transport block may make.
const RESERVED: &[&str] = &[
    "devc",
    "device",
    "prefix",
    "class",
    "transport",
    "ring-kind",
    "match",
    "proven",
    "window",
    "failmark",
];

/// Refuse a device part that restates the transport block (or binds another
/// device, or claims a proof). Returns every offending line, numbered.
pub fn check_device_part(part: &str) -> Result<(), String> {
    let mut bad = Vec::new();
    for (n, raw) in part.lines().enumerate() {
        let line = raw.split('#').next().unwrap_or("");
        let w: Vec<&str> = line.split_whitespace().collect();
        let reason = match w.as_slice() {
            [k, ..] if RESERVED.contains(k) => {
                Some(format!("`{k}` lines come from the transport block"))
            }
            ["reg", "common", ..] | ["reg", "notify", ..] => {
                Some("common and notify registers come from the transport block".to_string())
            }
            ["bits", "STATUS", ..] => {
                Some("the STATUS bits come from the transport block".to_string())
            }
            _ => None,
        };
        if let Some(r) = reason {
            bad.push(format!("line {}: {} ({})", n + 1, raw.trim(), r));
        }
    }
    if bad.is_empty() {
        Ok(())
    } else {
        Err(format!(
            "the device part restates the transport block:\n  {}",
            bad.join("\n  ")
        ))
    }
}

/// The device part of a full contract the model returned anyway: everything
/// after the transport block's statements, when the model echoed the block.
/// Lines identical to a block line are dropped; anything else is kept, so
/// `check_device_part` still sees a changed block line.
pub fn strip_echoed_block(reply: &str, block: &str) -> String {
    let fixed: std::collections::BTreeSet<&str> = block
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .collect();
    reply
        .lines()
        .filter(|l| !fixed.contains(l.trim()))
        .map(|l| format!("{l}\n"))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::inventory::parse_line;

    fn rng() -> Device {
        parse_line("inventory: pci 0000:00:03.0 1af4:1044 class=00ff00 subsys=1af4:1100 rev=01 bars=4:0x10000000:0x4000 driver=none contract=none proven=n/a")
            .unwrap()
            .unwrap()
    }

    #[test]
    fn an_rng_gets_the_entropy_class_and_its_own_ids() {
        let d = rng();
        let c = class_for(&d).unwrap();
        assert_eq!((c.class, c.prefix), ("entropy-poll", "vrng"));
        let b = block(&d, &c, "qemu-virt-aarch64", "m1", "2026-10-20");
        assert!(b.contains("\nmatch pci 0x1af4 0x1044\n"));
        assert!(b.contains("AIENOS_CK_DRIVER_BUILDER pending\n"));
        assert!(b.contains("reg common STATUS 0x14 1 rw reset 0 role status\n"));
    }

    #[test]
    fn unknown_devices_have_no_class_yet() {
        let mut d = rng();
        d.vendor = 0x8086;
        assert_eq!(class_for(&d), None);
        d.vendor = 0x1af4;
        d.device = 0x1005; // transitional rng: legacy layout, not built
        assert_eq!(class_for(&d), None);
    }

    #[test]
    fn a_device_part_cannot_rebind_or_claim_proof() {
        assert!(check_device_part("const BUF_LEN 64\nring REQ 0 virtq-split device-writes buf BUF_LEN stride BUF_LEN hdr 0\n").is_ok());
        let e = check_device_part(
            "const A 1\nmatch pci 0x1af4 0x1041\nproven x y z g abc\nreg common STATUS 0x10 1 rw\n",
        )
        .unwrap_err();
        assert!(
            e.contains("line 2") && e.contains("line 3") && e.contains("line 4"),
            "{e}"
        );
    }

    #[test]
    fn an_echoed_block_is_dropped_but_a_changed_line_is_caught() {
        let d = rng();
        let b = block(&d, &class_for(&d).unwrap(), "p", "m", "2026-10-20");
        let reply = format!("{b}const X 1\n");
        assert_eq!(strip_echoed_block(&reply, &b), "const X 1\n");
        let tampered = reply.replace("match pci 0x1af4 0x1044", "match pci 0x1af4 0x1041");
        assert!(check_device_part(&strip_echoed_block(&tampered, &b)).is_err());
    }
}
