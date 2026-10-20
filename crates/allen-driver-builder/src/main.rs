//! allen-driver-builder: the command Allen runs after boot (see lib.rs).
//!
//!   plan    --inventory LOG --contracts DIR [--state DIR] [--platform P] [--machine M]
//!   build   --inventory LOG --slot SSSS:BB:DD.F --llm CONF --state DIR --work DIR
//!           [--rounds N] [--max-tokens N] [--evidence DIR] [--reference FILE]
//!           [--grammar FILE] [--example FILE] [--platform P] [--machine M]
//!   consent --state DIR --pci-id VVVV:DDDD --by NAME
//!   enable  --state DIR --pci-id VVVV:DDDD     prints the staged dir for the image build
//!   undo    --state DIR --pci-id VVVV:DDDD
//!   active  --state DIR --pci-id VVVV:DDDD     prints the staged dir or `none`
//!   model   --llm CONF                         prints the model the endpoint serves
//! Environment: AIENOS_DEVC (contract compiler binary), AIENOS_REPO (repo root,
//! default the current directory), CC (default cc).
//! Exit: 0 ok, 1 refused or failed, 2 usage.

use allen_driver_builder::builder::{self, Job};
use allen_driver_builder::inventory;
use allen_driver_builder::llm::Endpoint;
use allen_driver_builder::prompt::{Evidence, Source};
use allen_driver_builder::registry::{self, Decision};
use allen_driver_builder::sandbox::Tools;
use allen_driver_builder::{say, stage, state, transport};
use serde_json::json;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

struct Args(BTreeMap<String, String>);

impl Args {
    fn parse(v: &[String]) -> Result<Args, String> {
        let mut m = BTreeMap::new();
        let mut it = v.iter();
        while let Some(k) = it.next() {
            let k = k
                .strip_prefix("--")
                .ok_or(format!("unexpected argument {k}"))?;
            let val = it.next().ok_or(format!("--{k} needs a value"))?;
            m.insert(k.to_string(), val.clone());
        }
        Ok(Args(m))
    }
    fn get(&self, k: &str) -> Result<&str, String> {
        self.0
            .get(k)
            .map(String::as_str)
            .ok_or(format!("--{k} is required"))
    }
    fn opt(&self, k: &str, d: &str) -> String {
        self.0.get(k).cloned().unwrap_or_else(|| d.to_string())
    }
    fn path(&self, k: &str) -> Result<PathBuf, String> {
        self.get(k).map(PathBuf::from)
    }
}

fn read(p: &Path) -> Result<String, String> {
    std::fs::read_to_string(p).map_err(|e| format!("{}: {e}", p.display()))
}

fn repo() -> PathBuf {
    std::env::var_os("AIENOS_REPO")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."))
}

fn plan(a: &Args) -> Result<bool, String> {
    let devices = inventory::parse(&read(&a.path("inventory")?)?)?;
    let contracts = registry::scan(&a.path("contracts")?).map_err(|e| e.to_string())?;
    let (platform, machine) = (
        a.opt("platform", "qemu-virt-aarch64"),
        a.opt("machine", "unknown"),
    );
    say(&format!("found {} devices on this computer", devices.len()));
    for d in &devices {
        let name = d.plain_name();
        match registry::decide(d, &contracts, &platform, &machine) {
            Decision::Skip => {}
            Decision::Load { contract } => {
                say(&format!(
                    "the {name} has a driver proven on this machine; loading it"
                ));
                state(&d.slot, "load", &contract.display().to_string(), true);
            }
            Decision::Attended { contract } => {
                say(&format!(
                    "the {name} has a driver that has not been proven on this machine yet; Allen will ask before switching it on"
                ));
                state(&d.slot, "attended", &contract.display().to_string(), true);
            }
            Decision::Build if d.driver.is_some() => {
                say(&format!(
                    "the {name} already works with the {} driver; Allen can build his own later",
                    d.driver.as_deref().unwrap_or("")
                ));
                state(&d.slot, "keep", d.driver.as_deref().unwrap_or("none"), true);
            }
            Decision::Build => match transport::class_for(d) {
                Some(c) => {
                    say(&format!(
                        "the {name} has no driver; Allen can build one with your language model ({} class)",
                        c.class
                    ));
                    state(&d.slot, "build", "ready", true);
                }
                None => {
                    say(&format!(
                        "the {name} ({}) has no driver, and Allen cannot build this kind of device yet",
                        d.id()
                    ));
                    state(&d.slot, "build", "no-class", false);
                }
            },
        }
    }
    Ok(true)
}

fn evidence(
    d: &inventory::Device,
    dir: Option<PathBuf>,
    serial: Option<PathBuf>,
) -> Result<Evidence, String> {
    let mut sources = Vec::new();
    if let Some(dir) = dir {
        let mut files: Vec<PathBuf> = std::fs::read_dir(&dir)
            .map_err(|e| format!("{}: {e}", dir.display()))?
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| p.is_file() && p.extension().is_none_or(|x| x != "licence"))
            .collect();
        files.sort();
        for f in files {
            let licence = std::fs::read_to_string(f.with_extension("licence"))
                .map(|s| s.trim().to_string())
                .unwrap_or_else(|_| "not stated (read only, never copied into AienOS)".into());
            let title = f
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .into_owned();
            sources.push(Source::from_file(&f, &title, &licence).map_err(|e| e.to_string())?);
        }
    }
    let mut boot_lines = Vec::new();
    if let Some(s) = serial {
        let short = d.slot.get(5..).unwrap_or(&d.slot).to_string();
        for l in read(&s)?.lines() {
            if (l.contains(&short) || l.contains(&d.id())) && !l.starts_with("inventory:") {
                boot_lines.push(l.to_string());
            }
        }
    }
    Ok(Evidence {
        device: d.clone(),
        boot_lines,
        sources,
        trace: None,
    })
}

fn build(a: &Args) -> Result<bool, String> {
    let devices = inventory::parse(&read(&a.path("inventory")?)?)?;
    let slot = a.get("slot")?;
    let d = devices
        .iter()
        .find(|d| d.slot == slot)
        .ok_or(format!("no device {slot} in the inventory"))?;
    let class = transport::class_for(d).ok_or(format!("no contract class for {} yet", d.id()))?;
    let mut ep = Endpoint::parse_conf(&read(&a.path("llm")?)?)?;
    ep.resolve_model()?;
    let repo = repo();
    let st = a.path("state")?;
    std::fs::create_dir_all(&st).map_err(|e| e.to_string())?;
    let tools = Tools {
        devc: std::env::var_os("AIENOS_DEVC")
            .map(PathBuf::from)
            .ok_or("AIENOS_DEVC (the contract compiler) is not set")?,
        repo: repo.clone(),
        cc: std::env::var("CC").unwrap_or_else(|_| "cc".into()),
        timeout_s: 120,
    };
    let ev = evidence(
        d,
        a.0.get("evidence").map(PathBuf::from),
        a.0.get("serial").map(PathBuf::from),
    )?;
    let job = Job {
        device: d.clone(),
        class,
        evidence: ev,
        endpoint: ep.clone(),
        tools,
        work: a.path("work")?,
        rounds: a
            .opt("rounds", "4")
            .parse()
            .map_err(|_| "--rounds is a number")?,
        max_tokens: a
            .opt("max-tokens", "8192")
            .parse()
            .map_err(|_| "--max-tokens is a number")?,
        platform: a.opt("platform", "qemu-virt-aarch64"),
        machine: a.opt("machine", "unknown"),
        grammar: read(&PathBuf::from(
            a.opt(
                "grammar",
                &repo
                    .join("docs/adr/0021-driver-contract-model.md")
                    .display()
                    .to_string(),
            ),
        ))?,
        example: read(&PathBuf::from(
            a.opt(
                "example",
                &repo
                    .join("native/net/contract/virtio_net.devc")
                    .display()
                    .to_string(),
            ),
        ))?,
        class_notes: read(&repo.join(format!(
            "crates/allen-driver-builder/sandbox/{}.md",
            class.class
        )))?,
        reference: a.0.get("reference").map(PathBuf::from),
    };
    say(&format!(
        "building a driver for the {} with {} (model {})",
        d.plain_name(),
        ep.label,
        ep.model
    ));
    let out = builder::run(&job)?;
    if !out.passed {
        say(&format!(
            "no attempt passed after {} rounds; every attempt is logged in {}",
            out.rounds,
            job.work.join("attempts.jsonl").display()
        ));
        println!(
            "AIENOS_DRIVER_BUILD: NOT_PASSED rounds={} prompt_sha256={}",
            out.rounds, out.prompt_sha256
        );
        return Ok(false);
    }
    let contract = out.contract.unwrap_or_default();
    let sandbox = out.sandbox.unwrap_or_default();
    let receipt = json!({
        "time": allen_driver_builder::utc_now(),
        "device": {"slot": d.slot, "pci_id": d.id(), "what": d.plain_name()},
        "class": class.class,
        "llm": {"label": ep.label, "place": ep.place(), "base_url": ep.base_url, "model": ep.model},
        "prompt_sha256": out.prompt_sha256,
        "rounds": out.rounds,
        "attempts_log": job.work.join("attempts.jsonl").display().to_string(),
        "sandbox_dir": sandbox.display().to_string(),
        "reference_contract": job.reference.as_ref().map(|p| p.display().to_string()),
        "written_by": "device part: the language model above; transport block: the builder",
    });
    let dir = stage::stage(&st, &d.id(), &contract, &sandbox, receipt)?;
    say(&format!(
        "the new driver for the {} passed every sandbox test and is staged; it stays off until you say yes",
        d.plain_name()
    ));
    state(&d.slot, "build", "staged", true);
    println!(
        "AIENOS_DRIVER_BUILD: PASSED rounds={} staged={} contract_sha256={}",
        out.rounds,
        dir.display(),
        allen_driver_builder::sha256_hex(contract.as_bytes())
    );
    Ok(true)
}

fn run(args: &[String]) -> Result<bool, String> {
    let (cmd, rest) = args
        .split_first()
        .ok_or("usage: allen-driver-builder <command> ...")?;
    let a = Args::parse(rest)?;
    match cmd.as_str() {
        "plan" => plan(&a),
        "build" => build(&a),
        "consent" => {
            let c = stage::consent(&a.path("state")?, a.get("pci-id")?, a.get("by")?)?;
            say(&format!(
                "you said yes; the driver for {} goes into the next start-up",
                a.get("pci-id")?
            ));
            println!("{c}");
            Ok(true)
        }
        "enable" => {
            let dir = stage::enable(&a.path("state")?, a.get("pci-id")?)?;
            say("the new driver is switched on for the next start-up; one step undoes it");
            println!("{}", dir.display());
            Ok(true)
        }
        "undo" => {
            let was = stage::undo(&a.path("state")?, a.get("pci-id")?)?;
            say(if was {
                "undone: the next start-up runs without the new driver"
            } else {
                "nothing to undo: the new driver was not switched on"
            });
            Ok(true)
        }
        "active" => {
            match stage::active(&a.path("state")?, a.get("pci-id")?)? {
                Some(d) => println!("{}", d.display()),
                None => println!("none"),
            }
            Ok(true)
        }
        "model" => {
            let mut ep = Endpoint::parse_conf(&read(&a.path("llm")?)?)?;
            ep.resolve_model()?;
            println!("{} {} {}", ep.place(), ep.base_url, ep.model);
            Ok(true)
        }
        _ => Err(format!("unknown command {cmd}")),
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args) {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::from(1),
        Err(e) => {
            say(&format!("stopped: {e}"));
            eprintln!("allen-driver-builder: {e}");
            ExitCode::from(if e.starts_with("usage") || e.contains("is required") {
                2
            } else {
                1
            })
        }
    }
}
