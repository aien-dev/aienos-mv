//! What Allen tells the model: the evidence about one device and the contract
//! language, and how the contract comes back out of the reply.

use crate::inventory::Device;

/// One piece of evidence with where it came from.
#[derive(Debug, Clone)]
pub struct Source {
    /// What it is, e.g. `datasheet "OASIS virtio v1.2 cs01" 5.4 Entropy Device`.
    pub title: String,
    /// The licence it is read under (references are read-only, never copied in).
    pub licence: String,
    pub sha256: String,
    pub text: String,
}

impl Source {
    pub fn from_file(
        path: &std::path::Path,
        title: &str,
        licence: &str,
    ) -> std::io::Result<Source> {
        let bytes = std::fs::read(path)?;
        Ok(Source {
            title: title.to_string(),
            licence: licence.to_string(),
            sha256: crate::sha256_hex(&bytes),
            text: String::from_utf8_lossy(&bytes).into_owned(),
        })
    }
}

/// Everything Allen knows about one device before asking the model.
#[derive(Debug, Clone)]
pub struct Evidence {
    pub device: Device,
    /// Lines the kernel printed about this function (capabilities, BAR decode).
    pub boot_lines: Vec<String>,
    pub sources: Vec<Source>,
    /// A recorded register trace of the vendor driver, when a host launcher recorded one.
    pub trace: Option<Source>,
}

impl Evidence {
    pub fn render(&self) -> String {
        let d = &self.device;
        let mut s = format!(
            "DEVICE\n  slot {}\n  pci id {} (vendor 0x{:04x} device 0x{:04x})\n  class 0x{:06x} ({})\n  subsystem {:04x}:{:04x}\n  revision 0x{:02x}\n",
            d.slot,
            d.id(),
            d.vendor,
            d.device,
            d.class,
            d.plain_name(),
            d.subsys.0,
            d.subsys.1,
            d.rev
        );
        for b in &d.bars {
            s.push_str(&format!(
                "  bar{} at 0x{:x} size 0x{:x}\n",
                b.index, b.addr, b.size
            ));
        }
        if !self.boot_lines.is_empty() {
            s.push_str("\nWHAT THE KERNEL SAW AT BOOT\n");
            for l in &self.boot_lines {
                s.push_str(&format!("  {l}\n"));
            }
        }
        s.push_str("\nVENDOR DRIVER TRACE\n");
        match &self.trace {
            Some(t) => s.push_str(&format!(
                "  {} (sha256 {})\n{}\n",
                t.title, t.sha256, t.text
            )),
            None => s.push_str("  none recorded on this machine\n"),
        }
        for src in &self.sources {
            s.push_str(&format!(
                "\nSOURCE: {} (licence: {}; sha256 {})\n{}\n",
                src.title, src.licence, src.sha256, src.text
            ));
        }
        s
    }
}

/// The fixed instructions. `grammar` is the contract language reference and
/// `example` a complete contract that compiles (both read from the contract
/// compiler's tree, never retyped here).
pub fn system_prompt(grammar: &str, example: &str) -> String {
    format!(
        "You write device contracts for AienOS. A device contract (.devc) is the whole driver: \
the contract compiler turns it into the C driver, a chip model of the device, a fuzzer and a \
documentation page, and the kernel grants the driver exactly the registers, DMA and interrupts \
the contract declares and nothing else.\n\n\
Rules:\n\
1. The transport block (identity, match, proof, windows, common registers, status bits, protocol \
rules) is written by the builder and given to you. Do not repeat or change it.\n\
2. Write only the device part that follows it: constants, features, flags, errors, resources, \
rings, the init and qsetup state machines, device registers if the device has any, and `manual` \
lines.\n\
3. Answer with exactly one fenced block that starts with ```devc and ends with ```, holding only \
the device part.\n\
4. Use only statements the language reference and the example use. Every number must come from \
the evidence.\n\
5. Quote the evidence you used in `manual` lines (document, section, the exact sentence).\n\
6. If a previous attempt failed, the compiler and test output follow; fix exactly what they report.\n\n\
LANGUAGE REFERENCE\n{grammar}\n\nA COMPLETE CONTRACT THAT COMPILES (for a different device and class)\n\
```devc\n{example}\n```\n"
    )
}

pub fn first_user_prompt(ev: &Evidence, block: &str, class_notes: &str) -> String {
    format!(
        "Write the device part of the contract for this device.\n\n{}\n\
WHAT THIS CONTRACT CLASS NEEDS\n{class_notes}\n\n\
THE TRANSPORT BLOCK (supplied; your device part is appended after it)\n```devc\n{block}```\n",
        ev.render()
    )
}

pub fn retry_user_prompt(first: &str, previous: &str, failure: &str) -> String {
    format!(
        "{first}\n\nYOUR PREVIOUS CONTRACT\n```devc\n{previous}\n```\n\nWHAT FAILED\n{failure}\n\nWrite the corrected contract."
    )
}

/// The contract in a reply: the first fenced block whose first statement is `devc`.
pub fn extract_contract(reply: &str) -> Option<String> {
    let mut rest = reply;
    while let Some(i) = rest.find("```") {
        let after = &rest[i + 3..];
        let body_start = after.find('\n')? + 1;
        let body = &after[body_start..];
        let end = body.find("```")?;
        let block = &body[..end];
        let first = block
            .lines()
            .map(|l| l.split('#').next().unwrap_or("").trim())
            .find(|l| !l.is_empty());
        if first.is_some_and(|l| l.starts_with("devc ")) {
            return Some(block.trim_end().to_string() + "\n");
        }
        rest = &body[end + 3..];
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_contract_block_is_found_among_others() {
        let r = "Here is my reasoning.\n```text\nnot this\n```\nAnd the contract:\n```devc\n# rng\ndevc 1\ndevice virtio-rng\n```\nDone.";
        assert_eq!(
            extract_contract(r).unwrap(),
            "# rng\ndevc 1\ndevice virtio-rng\n"
        );
        // an unlabelled fence works too when it holds a contract
        assert!(extract_contract("```\ndevc 1\n```").is_some());
        assert_eq!(extract_contract("no block at all"), None);
        assert_eq!(extract_contract("```devc\nunterminated"), None);
    }

    #[test]
    fn evidence_names_its_sources_and_the_missing_trace() {
        let d = crate::inventory::parse_line(
            "inventory: pci 0000:00:02.0 1af4:1044 class=00ff00 subsys=1af4:1100 rev=01 bars=4:0x10000:0x4000 driver=none contract=none proven=n/a",
        )
        .unwrap()
        .unwrap();
        let ev = Evidence {
            device: d,
            boot_lines: vec!["virtio_caps: common bar4+0x0".into()],
            sources: vec![Source {
                title: "spec".into(),
                licence: "OASIS IPR (read-only)".into(),
                sha256: "ab".into(),
                text: "5.4 Entropy Device".into(),
            }],
            trace: None,
        };
        let t = ev.render();
        assert!(t.contains("pci id 1af4:1044"));
        assert!(t.contains("bar4 at 0x10000 size 0x4000"));
        assert!(t.contains("none recorded on this machine"));
        assert!(t.contains("licence: OASIS IPR (read-only)"));
    }
}
