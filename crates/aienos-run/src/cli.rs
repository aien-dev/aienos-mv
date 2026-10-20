//! Argument parser (no crates). Exit codes: 0 ok, 1 runtime failure, 2 usage.

use crate::backend::PortForward;
use std::path::PathBuf;

pub const USAGE: &str = "usage: aienos-run [command] [options]

commands:
  (none) | run         boot AienOS and attach this terminal to its console
  measure --runs N     boot N times headless, print boot-to-marker receipts (JSON on stderr)
  --where              print the per-user paths (disks, EFI vars, console log)
  --reset              delete the Store disk and EFI variables (fresh start)
  --version | --help

options:
  --backend vz|qemu    hypervisor (macOS default: vz = Apple Virtualization.framework)
  --efi PATH           build the boot disk from this UEFI image (BOOTAA64.EFI)
  --boot-disk PATH     raw GPT boot disk (default: per-user cached disk)
  --boot-rw            attach the boot disk writable (diagnostic images that write reports)
  --store-disk PATH    persistent Store disk (default: per-user, created blank)
  --store-mib N        size of a new Store disk (default 256)
  --store-virtio       attach the Store disk as virtio-blk instead of NVMe
  --no-store           no Store disk
  --cpus N             vCPUs (default 2)
  --mem MIB            memory in MiB (default 1024)
  --forward P:H:G      extra port forward, e.g. udp:47031:47031 (udp:47030:47030 is always on)
  --no-net             no network device
  --window             show the VM framebuffer (qemu: cocoa window)
  --marker TEXT        console text that counts as \"booted\" (default \"kernel: alive\")
  --stop-at-marker     stop the VM as soon as the marker is seen
  --timeout S          ask the guest to stop after S seconds (default 86400)
  --log PATH           console log file
  --receipt            print the boot receipt (JSON) on exit
  --quiet              do not echo the console";

#[derive(Debug, PartialEq, Eq)]
pub enum Action {
    Run,
    Measure(u32),
    Where,
    Reset,
    Version,
    Help,
}

#[derive(Debug)]
pub struct Cmd {
    pub action: Action,
    pub backend: Option<String>,
    pub efi: Option<PathBuf>,
    pub boot_disk: Option<PathBuf>,
    pub boot_rw: bool,
    pub store_disk: Option<PathBuf>,
    pub store_mib: u64,
    pub store_virtio: bool,
    pub no_store: bool,
    pub efi_vars: Option<PathBuf>,
    pub cpus: u32,
    pub mem_mib: u64,
    pub forwards: Vec<PortForward>,
    pub no_net: bool,
    pub window: bool,
    pub marker: String,
    pub stop_at_marker: bool,
    pub timeout_s: u64,
    pub log: Option<PathBuf>,
    pub receipt: bool,
    pub quiet: bool,
    pub measuring: bool,
}

pub const DEFAULT_MARKER: &str = "kernel: alive";

pub fn parse(args: &[String]) -> Result<Cmd, String> {
    let mut c = Cmd {
        action: Action::Run,
        backend: None,
        efi: None,
        boot_disk: None,
        boot_rw: false,
        store_disk: None,
        store_mib: 256,
        store_virtio: false,
        no_store: false,
        efi_vars: None,
        cpus: 2,
        mem_mib: 1024,
        forwards: Vec::new(),
        no_net: false,
        window: false,
        marker: DEFAULT_MARKER.into(),
        stop_at_marker: false,
        timeout_s: 86400,
        log: None,
        receipt: false,
        quiet: false,
        measuring: false,
    };
    let mut runs: Option<u32> = None;
    let mut it = args.iter().peekable();
    let mut first = true;
    while let Some(a) = it.next() {
        let mut val = |name: &str| -> Result<String, String> {
            it.next().cloned().ok_or_else(|| format!("{name} needs a value"))
        };
        let num = |name: &str, v: String, lo: u64, hi: u64| -> Result<u64, String> {
            match v.parse::<u64>() {
                Ok(n) if (lo..=hi).contains(&n) => Ok(n),
                _ => Err(format!("{name} {v:?}: want a number {lo}..{hi}")),
            }
        };
        match a.as_str() {
            "run" if first => c.action = Action::Run,
            "measure" if first => c.action = Action::Measure(10),
            "--where" => c.action = Action::Where,
            "--reset" => c.action = Action::Reset,
            "--version" | "-V" => c.action = Action::Version,
            "--help" | "-h" => c.action = Action::Help,
            "--update" => return Err("--update: the image manifest is not published yet (see docs/AIENOS_RUN_MACOS.md); use --efi or --boot-disk".into()),
            "--runs" => runs = Some(num("--runs", val("--runs")?, 1, 1000)? as u32),
            "--backend" => c.backend = Some(val("--backend")?),
            "--efi" => c.efi = Some(val("--efi")?.into()),
            "--boot-disk" => c.boot_disk = Some(val("--boot-disk")?.into()),
            "--boot-rw" => c.boot_rw = true,
            "--store-disk" => c.store_disk = Some(val("--store-disk")?.into()),
            "--store-mib" => c.store_mib = num("--store-mib", val("--store-mib")?, 16, 1 << 20)?,
            "--store-virtio" => c.store_virtio = true,
            "--no-store" => c.no_store = true,
            "--efi-vars" => c.efi_vars = Some(val("--efi-vars")?.into()),
            "--cpus" => c.cpus = num("--cpus", val("--cpus")?, 1, 64)? as u32,
            "--mem" => c.mem_mib = num("--mem", val("--mem")?, 256, 1 << 20)?,
            "--forward" => c.forwards.push(PortForward::parse(&val("--forward")?)?),
            "--no-net" => c.no_net = true,
            "--window" => c.window = true,
            "--marker" => c.marker = val("--marker")?,
            "--stop-at-marker" => c.stop_at_marker = true,
            "--timeout" => c.timeout_s = num("--timeout", val("--timeout")?, 1, 1 << 32)?,
            "--log" => c.log = Some(val("--log")?.into()),
            "--receipt" => c.receipt = true,
            "--quiet" | "-q" => c.quiet = true,
            o => return Err(format!("unknown argument {o:?}")),
        }
        first = false;
    }
    if let Action::Measure(_) = c.action {
        c.action = Action::Measure(runs.unwrap_or(10));
        c.measuring = true;
        c.stop_at_marker = true;
        c.quiet = true;
    } else if runs.is_some() {
        return Err("--runs is only for `measure`".into());
    }
    Ok(c)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(s: &str) -> Result<Cmd, String> {
        parse(&s.split_whitespace().map(String::from).collect::<Vec<_>>())
    }

    #[test]
    fn defaults_boot() {
        let c = p("").unwrap();
        assert_eq!(c.action, Action::Run);
        assert_eq!((c.cpus, c.mem_mib, c.store_mib), (2, 1024, 256));
        assert_eq!(c.marker, DEFAULT_MARKER);
        assert!(!c.measuring);
    }

    #[test]
    fn measure_sets_headless_stop_at_marker() {
        let c = p("measure --runs 10 --backend vz --efi /x/BOOTAA64.EFI").unwrap();
        assert_eq!(c.action, Action::Measure(10));
        assert!(c.measuring && c.stop_at_marker && c.quiet);
        assert_eq!(c.backend.as_deref(), Some("vz"));
    }

    #[test]
    fn where_reset_version() {
        assert_eq!(p("--where").unwrap().action, Action::Where);
        assert_eq!(p("--reset").unwrap().action, Action::Reset);
        assert_eq!(p("--version").unwrap().action, Action::Version);
    }

    #[test]
    fn refusals() {
        for bad in ["--cpus 0", "--cpus", "--mem 10", "--bogus", "--runs 3", "--forward udp:0:1", "x", "--update"] {
            assert!(p(bad).is_err(), "{bad} accepted");
        }
    }

    #[test]
    fn forwards_accumulate() {
        let c = p("--forward udp:5000:5000 --forward tcp:8080:80").unwrap();
        assert_eq!(c.forwards.len(), 2);
    }
}
