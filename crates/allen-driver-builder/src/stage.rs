//! Staging, consent, enable and undo.
//!
//! State lives in one directory (`--state`):
//!   staged/<vvvv-dddd>/contract.devc   the contract that passed the sandbox
//!   staged/<vvvv-dddd>/driver/          the generated driver the kernel image links
//!   staged/<vvvv-dddd>/receipt.json     who wrote it (model, prompt sha), every test step and exit code
//!   staged/<vvvv-dddd>/consent.json     written only when the user says yes
//!   active.json                         which staged driver the next image carries (absent: none)
//!   history.jsonl                       every stage / consent / enable / undo, appended
//! Enable refuses without a consent record for exactly this contract (sha256).
//! Undo is one step: it removes the device from active.json, so the next
//! image is the one without the built driver. Nothing here deletes a vendor
//! driver or a previous image; an enable only adds.

use serde_json::{json, Value};
use std::io::Write;
use std::path::{Path, PathBuf};

pub fn staged_dir(state: &Path, pci_id: &str) -> PathBuf {
    state.join("staged").join(pci_id.replace(':', "-"))
}

fn history(state: &Path, v: Value) -> Result<(), String> {
    let mut f = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(state.join("history.jsonl"))
        .map_err(|e| e.to_string())?;
    writeln!(f, "{v}").map_err(|e| e.to_string())
}

fn read_json(p: &Path) -> Result<Value, String> {
    let b = std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?;
    serde_json::from_slice(&b).map_err(|e| format!("{}: {e}", p.display()))
}

fn write_json(p: &Path, v: &Value) -> Result<(), String> {
    let s = serde_json::to_string_pretty(v).map_err(|e| e.to_string())?;
    std::fs::write(p, s + "\n").map_err(|e| format!("{}: {e}", p.display()))
}

fn copy_dir(from: &Path, to: &Path) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|e| e.to_string())?;
    for e in std::fs::read_dir(from).map_err(|e| format!("{}: {e}", from.display()))? {
        let p = e.map_err(|e| e.to_string())?.path();
        if p.is_file() {
            std::fs::copy(&p, to.join(p.file_name().unwrap_or_default()))
                .map_err(|e| e.to_string())?;
        }
    }
    Ok(())
}

/// Stage a contract that passed: copy it and its generated driver, write the receipt.
pub fn stage(
    state: &Path,
    pci_id: &str,
    contract: &str,
    sandbox: &Path,
    receipt: Value,
) -> Result<PathBuf, String> {
    let dir = staged_dir(state, pci_id);
    if dir.join("consent.json").exists() {
        return Err(format!(
            "{} already holds a consented driver; undo it before staging another",
            dir.display()
        ));
    }
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    std::fs::write(dir.join("contract.devc"), contract).map_err(|e| e.to_string())?;
    copy_dir(&sandbox.join("gen/driver"), &dir.join("driver"))?;
    let sha = crate::sha256_hex(contract.as_bytes());
    let mut r = receipt;
    r["contract_sha256"] = json!(sha);
    r["status"] = json!("staged, waiting for the user's consent");
    write_json(&dir.join("receipt.json"), &r)?;
    history(
        state,
        json!({"time": crate::utc_now(), "event": "stage", "pci_id": pci_id, "contract_sha256": sha}),
    )?;
    Ok(dir)
}

/// Record the user's yes for exactly the staged contract.
pub fn consent(state: &Path, pci_id: &str, by: &str) -> Result<Value, String> {
    let dir = staged_dir(state, pci_id);
    let contract = std::fs::read(dir.join("contract.devc"))
        .map_err(|e| format!("nothing staged for {pci_id}: {e}"))?;
    let sha = crate::sha256_hex(&contract);
    let r = read_json(&dir.join("receipt.json"))?;
    if r["contract_sha256"] != json!(sha) {
        return Err("the staged contract changed after its receipt was written; refusing".into());
    }
    let c = json!({"time": crate::utc_now(), "pci_id": pci_id, "contract_sha256": sha, "by": by,
                   "scope": "attended enable of this one contract in the next image; undo removes it"});
    write_json(&dir.join("consent.json"), &c)?;
    history(
        state,
        json!({"time": crate::utc_now(), "event": "consent", "pci_id": pci_id, "contract_sha256": sha, "by": by}),
    )?;
    Ok(c)
}

/// Put the staged driver in the next image. Returns the staged directory
/// (the kernel build's CK_CONTRACT_RNG). Refuses without matching consent.
pub fn enable(state: &Path, pci_id: &str) -> Result<PathBuf, String> {
    let dir = staged_dir(state, pci_id);
    let contract = std::fs::read(dir.join("contract.devc"))
        .map_err(|e| format!("nothing staged for {pci_id}: {e}"))?;
    let sha = crate::sha256_hex(&contract);
    let c = read_json(&dir.join("consent.json"))
        .map_err(|_| format!("no consent recorded for {pci_id}: Allen asks the user first"))?;
    if c["contract_sha256"] != json!(sha) {
        return Err("the consent was given for a different contract; ask again".into());
    }
    let ap = state.join("active.json");
    let mut active = if ap.exists() {
        read_json(&ap)?
    } else {
        json!({})
    };
    active[pci_id] = json!({"staged": dir.display().to_string(), "contract_sha256": sha, "since": crate::utc_now()});
    write_json(&ap, &active)?;
    history(
        state,
        json!({"time": crate::utc_now(), "event": "enable", "pci_id": pci_id, "contract_sha256": sha}),
    )?;
    Ok(dir)
}

/// One-step undo: the next image no longer carries the built driver. The
/// staged files and the receipt stay (evidence); the consent is withdrawn.
pub fn undo(state: &Path, pci_id: &str) -> Result<bool, String> {
    let ap = state.join("active.json");
    let mut active = if ap.exists() {
        read_json(&ap)?
    } else {
        json!({})
    };
    let was = active
        .as_object_mut()
        .and_then(|m| m.remove(pci_id))
        .is_some();
    write_json(&ap, &active)?;
    let consent = staged_dir(state, pci_id).join("consent.json");
    if consent.exists() {
        let withdrawn = consent.with_file_name(format!(
            "consent.withdrawn-{}.json",
            crate::utc_now().replace(':', "")
        ));
        std::fs::rename(&consent, withdrawn).map_err(|e| e.to_string())?;
    }
    history(
        state,
        json!({"time": crate::utc_now(), "event": "undo", "pci_id": pci_id, "was_active": was}),
    )?;
    Ok(was)
}

/// The staged directory the next image carries for `pci_id`, if any.
pub fn active(state: &Path, pci_id: &str) -> Result<Option<PathBuf>, String> {
    let ap = state.join("active.json");
    if !ap.exists() {
        return Ok(None);
    }
    Ok(read_json(&ap)?[pci_id]["staged"]
        .as_str()
        .map(PathBuf::from))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(n: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("adb-stage-{n}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("sbx/gen/driver")).unwrap();
        std::fs::write(d.join("sbx/gen/driver/vrng_contract.c"), "/* gen */\n").unwrap();
        d
    }

    #[test]
    fn no_enable_without_consent_and_undo_is_one_step() {
        let d = tmp("flow");
        let st = d.join("state");
        std::fs::create_dir_all(&st).unwrap();
        let dir = stage(
            &st,
            "1af4:1044",
            "devc 1\n",
            &d.join("sbx"),
            json!({"llm": "x"}),
        )
        .unwrap();
        assert!(dir.join("driver/vrng_contract.c").exists());
        assert!(enable(&st, "1af4:1044").unwrap_err().contains("no consent"));
        consent(&st, "1af4:1044", "drake").unwrap();
        assert_eq!(enable(&st, "1af4:1044").unwrap(), dir);
        assert_eq!(active(&st, "1af4:1044").unwrap(), Some(dir.clone()));
        assert!(undo(&st, "1af4:1044").unwrap());
        assert_eq!(active(&st, "1af4:1044").unwrap(), None);
        assert!(
            dir.join("contract.devc").exists(),
            "undo keeps the evidence"
        );
        assert!(
            enable(&st, "1af4:1044").is_err(),
            "undo withdraws the consent"
        );
        let h = std::fs::read_to_string(st.join("history.jsonl")).unwrap();
        assert_eq!(
            h.lines().count(),
            4,
            "stage, consent, enable, undo; refusals change nothing"
        );
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn a_changed_contract_voids_the_consent() {
        let d = tmp("tamper");
        let st = d.join("state");
        std::fs::create_dir_all(&st).unwrap();
        let dir = stage(&st, "1af4:1044", "devc 1\n", &d.join("sbx"), json!({})).unwrap();
        consent(&st, "1af4:1044", "drake").unwrap();
        std::fs::write(
            dir.join("contract.devc"),
            "devc 1\nmatch pci 0x1af4 0x1041\n",
        )
        .unwrap();
        assert!(enable(&st, "1af4:1044")
            .unwrap_err()
            .contains("different contract"));
        let _ = std::fs::remove_dir_all(&d);
    }
}
