//! Allen's driver builder (L268; device contracts: ADR 0021, L215).
//!
//! Right after boot the kernel prints one `inventory:` line per PCI function.
//! For each device Allen then takes one of three paths:
//! - a contract exists and is proven on this machine: load it;
//! - a contract exists but is not proven here: attended enable, receipt written;
//! - no contract: Allen builds one. The user's language model writes the
//!   device part of a contract, the builder supplies the transport part
//!   verbatim, and the result must pass the contract compiler's checks, the
//!   generated chip model and fuzzer, and (when one exists) a reference model,
//!   all in a sandbox, before it is staged. Nothing reaches the device until
//!   the user consents; the vendor driver is never deleted, and undo is one step.
//!
//! Every step prints a plain-English line for Allen's window (`allen-drivers: `)
//! and a machine line for the setup wizard (`allen-drivers-state: `).

pub mod builder;
pub mod inventory;
pub mod llm;
pub mod prompt;
pub mod registry;
pub mod sandbox;
pub mod stage;
pub mod transport;

/// Lower-case hex SHA-256 of `data`.
pub fn sha256_hex(data: &[u8]) -> String {
    aienos_crypto::sha256::hash(data)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// The time now as `YYYY-MM-DDTHH:MM:SSZ` (UTC), from the system clock.
pub fn utc_now() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let (days, rem) = (secs / 86_400, secs % 86_400);
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    let z = days as i64 + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!(
        "{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}Z",
        rem / 3600,
        rem % 3600 / 60,
        rem % 60
    )
}

/// One plain-English line for Allen's window.
pub fn say(msg: &str) {
    println!("allen-drivers: {msg}");
}

/// One machine line for the setup wizard: `<device> <path> <stage> <ok|fail>`.
pub fn state(device: &str, path: &str, stage: &str, ok: bool) {
    println!(
        "allen-drivers-state: {device} {path} {stage} {}",
        if ok { "ok" } else { "fail" }
    );
}

#[cfg(test)]
mod tests {
    #[test]
    fn sha256_of_abc_is_the_fips_vector() {
        assert_eq!(
            super::sha256_hex(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn utc_now_has_the_receipt_shape() {
        let t = super::utc_now();
        assert_eq!(t.len(), 20);
        assert!(t.starts_with("20") && t.ends_with('Z'));
    }
}
