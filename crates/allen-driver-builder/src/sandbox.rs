//! The sandbox a candidate contract must pass before it is staged.
//!
//! Everything runs on the host in a fresh directory, against simulated
//! devices only; no step opens a device node or touches a PCI function.
//! Steps, each a separate process with its command and exit code recorded:
//!   1. `aienos-devc check`    the contract compiler's own checks
//!   2. `aienos-devc gen`      driver + chip model + fuzzer from the contract
//!   3. compile                the generated C with -Werror
//!   4. selftest               a hand-written class test (never generated, never
//!                             written by a language model) drives the generated
//!                             driver against the generated chip model
//!   5. fuzz                   the generated state-machine fuzzer
//!   6. reference (optional)   the same class test drives the candidate driver
//!                             against the chip model generated from a reference
//!                             contract the language model never saw
//! Each process runs with a CPU-time, file-size and core-file limit and a wall
//! clock timeout (no address-space limit: AddressSanitizer reserves terabytes
//! of virtual memory), under ASan + UBSan. What it is NOT: a namespace or seccomp jail (unprivileged
//! user namespaces are not permitted on this host: `unshare -rn` fails); the
//! generated C is compiled from a checked contract, not taken from the model.

use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Debug, Clone)]
pub struct Step {
    pub name: String,
    pub command: String,
    pub exit: i32,
    /// The last lines of stdout + stderr.
    pub tail: String,
}

#[derive(Debug, Clone)]
pub struct Report {
    pub steps: Vec<Step>,
    pub dir: PathBuf,
}

impl Report {
    pub fn passed(&self) -> bool {
        !self.steps.is_empty() && self.steps.iter().all(|s| s.exit == 0)
    }

    /// What failed, for the next prompt and the attempt log.
    pub fn failure(&self) -> String {
        self.steps
            .iter()
            .filter(|s| s.exit != 0)
            .map(|s| {
                format!(
                    "step {} failed (exit {}): {}\n{}",
                    s.name, s.exit, s.command, s.tail
                )
            })
            .collect::<Vec<_>>()
            .join("\n")
    }

    pub fn summary(&self) -> String {
        self.steps
            .iter()
            .map(|s| format!("{}={}", s.name, s.exit))
            .collect::<Vec<_>>()
            .join(" ")
    }
}

/// Where the tools and fixed sources live.
#[derive(Debug, Clone)]
pub struct Tools {
    /// The contract compiler (`AIENOS_DEVC`).
    pub devc: PathBuf,
    /// The repository root (native/net/aienos_virtio_pci.c, the class tests).
    pub repo: PathBuf,
    pub cc: String,
    pub timeout_s: u32,
}

fn tail(bytes: &[u8], lines: usize) -> String {
    let s = String::from_utf8_lossy(bytes);
    let v: Vec<&str> = s.lines().collect();
    v[v.len().saturating_sub(lines)..].join("\n")
}

/// Run `argv` in `dir` with the limits; never through a shell string built
/// from untrusted text (the arguments are passed as separate words).
fn run(t: &Tools, dir: &Path, name: &str, argv: &[String]) -> Step {
    let limits = format!(
        "ulimit -t {} -c 0 -f 262144; exec timeout {} \"$@\"",
        t.timeout_s, t.timeout_s
    );
    let out = Command::new("sh")
        .arg("-c")
        .arg(&limits)
        .arg("sandbox")
        .args(argv)
        .current_dir(dir)
        .env_clear()
        .env("PATH", "/usr/local/bin:/usr/bin:/bin")
        .output();
    let command = argv.join(" ");
    match out {
        Ok(o) => {
            let mut both = o.stdout.clone();
            both.extend_from_slice(&o.stderr);
            Step {
                name: name.into(),
                command,
                exit: o.status.code().unwrap_or(-1),
                tail: tail(&both, 30),
            }
        }
        Err(e) => Step {
            name: name.into(),
            command,
            exit: -1,
            tail: format!("could not start: {e}"),
        },
    }
}

fn s(p: &Path) -> String {
    p.display().to_string()
}

/// Compile one test binary from a generated driver dir and a model dir.
fn compile(
    t: &Tools,
    dir: &Path,
    name: &str,
    drv: &Path,
    model: &Path,
    main: &Path,
    prefix: &str,
    out: &str,
) -> Step {
    let mut argv = vec![
        t.cc.clone(),
        "-std=c11".into(),
        "-O1".into(),
        "-g".into(),
        "-Wall".into(),
        "-Wextra".into(),
        "-Werror".into(),
        "-fsanitize=address,undefined".into(),
        "-fno-sanitize-recover=all".into(),
        format!("-I{}", s(drv)),
        format!("-I{}", s(model)),
        format!("-I{}", s(&t.repo.join("native/net"))),
        "-o".into(),
        out.into(),
        s(main),
        s(&drv.join(format!("{prefix}_contract.c"))),
        s(&model.join(format!("{prefix}_model.c"))),
        s(&t.repo.join("native/net/aienos_virtio_pci.c")),
    ];
    if main
        .file_name()
        .is_some_and(|n| n.to_string_lossy().ends_with("_fuzz.c"))
    {
        argv.retain(|a| a != "-Wextra"); // the generated fuzzer is not -Wextra clean in devc 1
    }
    run(t, dir, name, &argv)
}

/// The class test for `prefix` (hand-written, in this crate).
pub fn class_test(repo: &Path, prefix: &str) -> PathBuf {
    repo.join(format!(
        "crates/allen-driver-builder/sandbox/{prefix}_selftest.c"
    ))
}

/// Run every step for `contract` in `dir` (created fresh). `reference` is a
/// contract the language model never saw, for step 6.
pub fn test(
    t: &Tools,
    dir: &Path,
    contract: &str,
    prefix: &str,
    reference: Option<&Path>,
) -> Report {
    let mut r = Report {
        steps: Vec::new(),
        dir: dir.to_path_buf(),
    };
    if let Err(e) = std::fs::create_dir_all(dir) {
        r.steps.push(Step {
            name: "setup".into(),
            command: format!("mkdir {}", dir.display()),
            exit: -1,
            tail: e.to_string(),
        });
        return r;
    }
    let cpath = dir.join("contract.devc");
    if let Err(e) = std::fs::write(&cpath, contract) {
        r.steps.push(Step {
            name: "setup".into(),
            command: format!("write {}", cpath.display()),
            exit: -1,
            tail: e.to_string(),
        });
        return r;
    }
    let gen = dir.join("gen");
    let devc = s(&t.devc);
    for (name, argv) in [
        ("check", vec![devc.clone(), "check".into(), s(&cpath)]),
        ("gen", vec![devc.clone(), "gen".into(), s(&cpath), s(&gen)]),
    ] {
        let st = run(t, dir, name, &argv);
        let ok = st.exit == 0;
        r.steps.push(st);
        if !ok {
            return r;
        }
    }
    let (drv, model) = (gen.join("driver"), gen.join("model"));
    let selftest = class_test(&t.repo, prefix);
    let fuzz_src = model.join(format!("{prefix}_fuzz.c"));
    for (name, main, out) in [
        ("compile-selftest", &selftest, "selftest"),
        ("compile-fuzz", &fuzz_src, "fuzz"),
    ] {
        let st = compile(t, dir, name, &drv, &model, main, prefix, out);
        let ok = st.exit == 0;
        r.steps.push(st);
        if !ok {
            return r;
        }
    }
    r.steps
        .push(run(t, dir, "selftest", &[s(&dir.join("selftest"))]));
    r.steps.push(run(
        t,
        dir,
        "fuzz",
        &[s(&dir.join("fuzz")), "0x6a5e11".into(), "3000".into()],
    ));
    if let Some(refc) = reference {
        let rgen = dir.join("ref");
        let st = run(t, dir, "ref-gen", &[devc, "gen".into(), s(refc), s(&rgen)]);
        let ok = st.exit == 0;
        r.steps.push(st);
        if ok {
            let st = compile(
                t,
                dir,
                "compile-reference",
                &drv,
                &rgen.join("model"),
                &selftest,
                prefix,
                "reference",
            );
            let ok = st.exit == 0;
            r.steps.push(st);
            if ok {
                r.steps
                    .push(run(t, dir, "reference", &[s(&dir.join("reference"))]));
            }
        }
    }
    r
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_report_passes_only_when_every_step_exits_zero() {
        let mk = |e| Step {
            name: "x".into(),
            command: "c".into(),
            exit: e,
            tail: "t".into(),
        };
        let mut r = Report {
            steps: vec![],
            dir: PathBuf::new(),
        };
        assert!(!r.passed(), "no steps is not a pass");
        r.steps = vec![mk(0), mk(0)];
        assert!(r.passed());
        r.steps.push(mk(1));
        assert!(!r.passed());
        assert!(r.failure().contains("exit 1"));
        assert_eq!(r.summary(), "x=0 x=0 x=1");
    }

    #[test]
    fn a_missing_compiler_is_a_failed_step_not_a_pass() {
        let dir = std::env::temp_dir().join(format!("adb-sbx-{}", std::process::id()));
        let t = Tools {
            devc: "/nonexistent/aienos-devc".into(),
            repo: ".".into(),
            cc: "cc".into(),
            timeout_s: 5,
        };
        let r = test(&t, &dir, "devc 1\n", "vrng", None);
        assert!(!r.passed());
        assert_eq!(r.steps[0].name, "check");
        assert_ne!(r.steps[0].exit, 0);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
