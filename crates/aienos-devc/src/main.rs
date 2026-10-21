//! aienos-devc: the device contract compiler (ADR 0021).
//!
//!   aienos-devc gen   CONTRACT OUTDIR   write every generated file
//!   aienos-devc check CONTRACT OUTDIR   regenerate in memory; exit 1 on any difference (drift)
//!   aienos-devc stats CONTRACT          print the declarative/template line counts
//!   aienos-devc infer TRACE DEVICE      print a draft contract inferred from a register trace
//!   aienos-devc covers CONTRACT TRACE   exit 1 unless every traced access is a declared one
//!
//! One contract, five outputs: driver header + C, chip model header + C,
//! fuzzer C, documentation page, Omegese form.

mod contract;
mod doc;
mod driver;
mod fuzz;
mod gpu;
mod gpuboot;
mod infer;
mod model;

use std::path::Path;
use std::process::ExitCode;

fn hex(d: &[u8]) -> String {
    d.iter().map(|b| format!("{b:02x}")).collect()
}

/// Every generated file, by name. The digest covers the contract bytes.
fn outputs(src: &str, name: &str) -> Result<(Vec<(String, String)>, String), String> {
    if gpu::is_gpu(src) {
        let g = gpu::parse(src, name).map_err(|e| e.to_string())?;
        let digest = hex(&aienos_crypto::sha256::hash(src.as_bytes()));
        let c = gpu::source(&g, &digest);
        let md = gpu::markdown(&g, &digest, c.lines().count());
        let stem = g.device.replace('-', "_");
        return Ok((vec![("gch_contract.c".into(), c), (format!("{stem}.md"), md)], digest));
    }
    if gpuboot::is_gpu_boot(src) {
        let g = gpuboot::parse(src, name).map_err(|e| e.to_string())?;
        let digest = hex(&aienos_crypto::sha256::hash(src.as_bytes()));
        let c = gpuboot::source(&g, &digest);
        let md = gpuboot::markdown(&g, &digest, c.lines().count());
        let stem = g.device.replace('-', "_");
        return Ok((vec![("gsb_contract.c".into(), c), (format!("{stem}.md"), md)], digest));
    }
    let c = contract::parse(src, name).map_err(|e| e.to_string())?;
    let digest = hex(&aienos_crypto::sha256::hash(src.as_bytes()));
    let drv = driver::source(&c, &digest);
    let driver_lines = drv.lines().count();
    let step_lines = count_step_lines(&c);
    let stem = c.device.replace('-', "_");
    Ok((
        vec![
            ("driver/aienos_virtio_net.h".into(), driver::header(&c)),
            ("driver/vnet_contract.c".into(), drv),
            ("model/vnet_model.h".into(), model::header(&c)),
            ("model/vnet_model.c".into(), model::source(&c)),
            ("model/vnet_fuzz.c".into(), fuzz::source(&c)),
            (
                format!("{stem}.md"),
                doc::markdown(&c, &digest, driver_lines, step_lines),
            ),
            (format!("{stem}.omegese"), doc::omegese(&c)),
        ],
        digest,
    ))
}

/// Lines of driver C that come from the contract's own steps (init + qsetup).
fn count_step_lines(c: &contract::Contract) -> usize {
    let mut n = 0;
    let mut armed = false;
    for s in &c.init {
        if matches!(s, contract::Step::ArmFail) {
            armed = true;
        }
        n += driver::step_c_lines(c, s, armed, false);
    }
    for s in &c.qsetup {
        n += driver::step_c_lines(c, s, false, true);
    }
    n
}

fn run(args: &[String]) -> Result<ExitCode, String> {
    let usage = "usage: aienos-devc gen|check CONTRACT OUTDIR | stats CONTRACT | infer TRACE DEVICE | covers CONTRACT TRACE";
    let cmd = args.get(1).ok_or(usage)?;
    if cmd == "infer" {
        let t = args.get(2).ok_or(usage)?;
        let dev = args.get(3).ok_or(usage)?;
        let trace = std::fs::read_to_string(t).map_err(|e| format!("{t}: {e}"))?;
        print!("{}", infer::draft(&infer::infer(&trace), dev, t));
        return Ok(ExitCode::SUCCESS);
    }
    let path = Path::new(args.get(2).ok_or(usage)?);
    let src = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let name = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("contract");
    let (files, digest) = outputs(&src, name)?;
    match cmd.as_str() {
        "gen" => {
            let out = Path::new(args.get(3).ok_or(usage)?);
            std::fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
            for (n, body) in &files {
                {
                    let f = out.join(n);
                    if let Some(d) = f.parent() {
                        std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
                    }
                    std::fs::write(f, body)
                }
                .map_err(|e| format!("{n}: {e}"))?;
            }
            println!(
                "aienos-devc: {name} {digest} -> {} files in {}",
                files.len(),
                out.display()
            );
            Ok(ExitCode::SUCCESS)
        }
        "check" => {
            let out = Path::new(args.get(3).ok_or(usage)?);
            let mut drift = 0;
            for (n, body) in &files {
                match std::fs::read_to_string(out.join(n)) {
                    Ok(old) if old == *body => {}
                    _ => {
                        println!("DRIFT {}", out.join(n).display());
                        drift += 1;
                    }
                }
            }
            println!(
                "AIENOS_DEVC_CHECK: {} ({name} {digest})",
                if drift == 0 { "PASS" } else { "FAIL" }
            );
            Ok(if drift == 0 {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            })
        }
        "stats" => {
            let c = contract::parse(&src, name).map_err(|e| e.to_string())?;
            let drv = &files[1].1;
            println!(
                "contract_statements={} driver_lines={} step_lines={} hand_logic_lines=0 registers={} windows={} rules={}",
                c.stmt_count,
                drv.lines().count(),
                count_step_lines(&c),
                c.regs.len(),
                c.windows.len(),
                c.rules.len()
            );
            Ok(ExitCode::SUCCESS)
        }
        "covers" => {
            let t = args.get(3).ok_or(usage)?;
            let trace = std::fs::read_to_string(t).map_err(|e| format!("{t}: {e}"))?;
            let c = contract::parse(&src, name).map_err(|e| e.to_string())?;
            let inf = infer::infer(&trace);
            let bad = infer::covered_by(&inf, &c);
            for m in &bad {
                println!("OUTSIDE {}", m.0);
            }
            println!(
                "AIENOS_DEVC_COVERS: {} ({} accesses, {} registers, {} outside {name})",
                if bad.is_empty() { "PASS" } else { "FAIL" },
                inf.accesses,
                inf.regs.len(),
                bad.len()
            );
            Ok(if bad.is_empty() && inf.accesses > 0 {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            })
        }
        _ => Err(usage.into()),
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    match run(&args) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("aienos-devc: {e}");
            ExitCode::from(2)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const VNET: &str = include_str!("../../../native/net/contract/virtio_net.devc");

    #[test]
    fn virtio_net_contract_compiles() {
        let (files, digest) = outputs(VNET, "virtio_net.devc").expect("contract");
        assert_eq!(files.len(), 7);
        assert_eq!(digest.len(), 64);
        assert!(files[1].1.contains("static inline void wr_STATUS("));
    }

    /// Each of these edits must be refused: the checker is the security boundary.
    #[test]
    fn checker_refuses_reach_outside_the_contract() {
        let bad = [
            (
                "  write STATUS 0\n",
                "  write NOSUCH 0\n",
                "undeclared register",
            ),
            ("  write STATUS 0\n", "  write NUMQ 0\n", "read-only"),
            (
                "  write MSIX 0xffff\n",
                "  write MSIX 0x1ffff\n",
                "does not fit",
            ),
            (
                "reg common Q_USEDHI 0x34 4 rw bank Q_SELECT\n",
                "reg common Q_USEDHI 0x38 4 rw bank Q_SELECT\n",
                "outside window",
            ),
            (
                "reg device MAC 0x00 1 ro count 6 role config\n",
                "reg device MAC 0x00 1 ro count 8 role config\n",
                "outside window",
            ),
            (
                "feature MAC 5 accept needs device 6\n",
                "feature MAC 5 accept\n",
                "outside window",
            ),
            (
                "  kick RX\n",
                "  kick RX\n  write DOORBELL 0\n",
                "rung by `kick`",
            ),
            (
                "  write MSIX 0xffff\n",
                "  write MSIX 0xffff\n  write Q_SIZE 4\n",
                "banked by the queue selector",
            ),
            ("manual ", "#manual ", "at least one `manual`"),
            ("irq none\n", "irq legacy\n", "polled"),
            (
                "error DEVICE 11 HostileDevice\n",
                "error DEVICE 10 HostileDevice\n",
                "unique",
            ),
        ];
        for (from, to, want) in bad {
            assert!(VNET.contains(from), "fixture line missing: {from}");
            let src = VNET.replace(from, to);
            let e = contract::parse(&src, "t").expect_err(&format!("accepted: {to}"));
            assert!(e.msg.contains(want), "{to}: got `{}`, want `{want}`", e.msg);
        }
    }
}
