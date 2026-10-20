//! The build loop: ask the user's model for the device part, compose it
//! behind the transport block, test it in the sandbox, feed failures back,
//! and log every attempt. A contract that passes is staged, never enabled:
//! enabling needs the user's consent (stage.rs).

use crate::inventory::Device;
use crate::llm::Endpoint;
use crate::prompt::{self, Evidence};
use crate::sandbox::{self, Tools};
use crate::transport::{self, Class};
use serde_json::json;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::time::Instant;

pub struct Job {
    pub device: Device,
    pub class: Class,
    pub evidence: Evidence,
    pub endpoint: Endpoint,
    pub tools: Tools,
    /// Attempts, prompts, replies and sandboxes go here.
    pub work: PathBuf,
    pub rounds: u32,
    pub max_tokens: u32,
    pub platform: String,
    pub machine: String,
    /// The language reference and the example contract shown to the model.
    pub grammar: String,
    pub example: String,
    pub class_notes: String,
    /// A contract the model never sees, for the sandbox's reference step.
    pub reference: Option<PathBuf>,
}

pub struct Outcome {
    pub passed: bool,
    pub rounds: u32,
    /// The composed contract of the last attempt (the passing one when passed).
    pub contract: Option<String>,
    pub sandbox: Option<PathBuf>,
    pub prompt_sha256: String,
    pub last_failure: String,
}

fn append(log: &Path, v: &serde_json::Value) -> std::io::Result<()> {
    let mut f = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(log)?;
    writeln!(f, "{v}")
}

/// Strip a reasoning preamble (`<think>...</think>`, Qwen3) before looking for the block.
pub fn strip_think(reply: &str) -> &str {
    match reply.rfind("</think>") {
        Some(i) => &reply[i + "</think>".len()..],
        None => reply,
    }
}

/// The device part in a reply: the first ```devc block (or, failing that, the
/// first fenced block), with an echoed transport block removed.
pub fn device_part(reply: &str, block: &str) -> Option<String> {
    let body = strip_think(reply);
    let fenced = prompt::extract_contract(body).or_else(|| {
        let i = body.find("```")?;
        let after = &body[i + 3..];
        let start = after.find('\n')? + 1;
        let end = after[start..].find("```")?;
        Some(after[start..start + end].to_string())
    })?;
    Some(transport::strip_echoed_block(&fenced, block))
}

pub fn run(job: &Job) -> Result<Outcome, String> {
    std::fs::create_dir_all(&job.work).map_err(|e| format!("{}: {e}", job.work.display()))?;
    let log = job.work.join("attempts.jsonl");
    let date = crate::utc_now()[..10].to_string();
    let block = transport::block(&job.device, &job.class, &job.platform, &job.machine, &date);
    let system = prompt::system_prompt(&job.grammar, &job.example);
    let first = prompt::first_user_prompt(&job.evidence, &block, &job.class_notes);
    let name = job.device.plain_name();
    let mut user = first.clone();
    let mut out = Outcome {
        passed: false,
        rounds: 0,
        contract: None,
        sandbox: None,
        prompt_sha256: crate::sha256_hex(format!("{system}\n{first}").as_bytes()),
        last_failure: String::new(),
    };
    for round in 1..=job.rounds {
        out.rounds = round;
        let dir = job.work.join(format!("attempt-{round:02}"));
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        std::fs::write(dir.join("system.txt"), &system).map_err(|e| e.to_string())?;
        std::fs::write(dir.join("user.txt"), &user).map_err(|e| e.to_string())?;
        let prompt_sha = crate::sha256_hex(format!("{system}\n{user}").as_bytes());
        crate::say(&format!(
            "asking {} to write a driver for the {name} (attempt {round} of {})",
            job.endpoint.label, job.rounds
        ));
        let t0 = Instant::now();
        let reply = job.endpoint.chat(&system, &user, job.max_tokens, 600);
        let llm_s = t0.elapsed().as_secs_f64();
        let mut rec = json!({
            "time": crate::utc_now(),
            "device": job.device.slot,
            "pci_id": job.device.id(),
            "round": round,
            "llm": {"label": job.endpoint.label, "place": job.endpoint.place(),
                    "base_url": job.endpoint.base_url, "model": job.endpoint.model},
            "prompt_sha256": prompt_sha,
            "llm_seconds": llm_s,
        });
        let reply = match reply {
            Ok(r) => r,
            Err(e) => {
                rec["outcome"] = json!("llm-error");
                rec["error"] = json!(e);
                append(&log, &rec).map_err(|e| e.to_string())?;
                crate::state(&job.device.slot, "build", "llm", false);
                return Err(format!("the model did not answer: {e}"));
            }
        };
        std::fs::write(dir.join("reply.txt"), &reply).map_err(|e| e.to_string())?;
        rec["reply_sha256"] = json!(crate::sha256_hex(reply.as_bytes()));
        let failure = match device_part(&reply, &block) {
            None => "your reply had no fenced ```devc block".to_string(),
            Some(part) => match transport::check_device_part(&part) {
                Err(e) => e,
                Ok(()) => {
                    let contract = format!("{block}{part}");
                    rec["contract_sha256"] = json!(crate::sha256_hex(contract.as_bytes()));
                    std::fs::write(dir.join("contract.devc"), &contract)
                        .map_err(|e| e.to_string())?;
                    crate::say(&format!(
                        "testing attempt {round} in the sandbox (simulated device only)"
                    ));
                    let rep = sandbox::test(
                        &job.tools,
                        &dir.join("sandbox"),
                        &contract,
                        job.class.prefix,
                        job.reference.as_deref(),
                    );
                    rec["sandbox"] = json!(rep
                        .steps
                        .iter()
                        .map(|s| json!({"step": s.name, "command": s.command, "exit": s.exit}))
                        .collect::<Vec<_>>());
                    out.contract = Some(contract);
                    out.sandbox = Some(rep.dir.clone());
                    if rep.passed() {
                        rec["outcome"] = json!("passed");
                        append(&log, &rec).map_err(|e| e.to_string())?;
                        crate::say(&format!(
                            "attempt {round} passed every sandbox test ({})",
                            rep.summary()
                        ));
                        crate::state(&job.device.slot, "build", "sandbox", true);
                        out.passed = true;
                        return Ok(out);
                    }
                    rep.failure()
                }
            },
        };
        rec["outcome"] = json!("failed");
        rec["failure"] = json!(failure.chars().take(4000).collect::<String>());
        append(&log, &rec).map_err(|e| e.to_string())?;
        crate::say(&format!(
            "attempt {round} did not pass; sending the test output back to the model"
        ));
        out.last_failure = failure.clone();
        let previous = out
            .contract
            .as_deref()
            .map(|c| c.strip_prefix(block.as_str()).unwrap_or(c).to_string())
            .unwrap_or_else(|| strip_think(&reply).chars().take(6000).collect());
        user = prompt::retry_user_prompt(&first, &previous, &failure);
    }
    crate::state(&job.device.slot, "build", "sandbox", false);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_thinking_preamble_is_skipped() {
        let r =
            "<think>maybe ```devc\nmatch pci 1 2\n```</think>\nHere:\n```devc\nconst A 1\n```\n";
        assert_eq!(device_part(r, "").unwrap(), "const A 1\n");
    }

    #[test]
    fn a_plain_fence_is_accepted_and_an_echoed_block_removed() {
        let block = "devc 1\nprefix vrng\n";
        let r = "```\ndevc 1\nprefix vrng\nconst A 1\n```";
        assert_eq!(device_part(r, block).unwrap(), "const A 1\n");
        assert_eq!(device_part("no code here", block), None);
    }
}
