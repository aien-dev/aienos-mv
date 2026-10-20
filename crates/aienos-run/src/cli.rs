//! Command-line parsing (own parser, no outside crate).

use crate::net::Forward;
use std::path::PathBuf;

pub const USAGE: &str = "\
aienos-run: boot AienOS in a virtual machine on this computer

usage:
  aienos-run [options]           boot AienOS (downloads and verifies the image on first use)
  aienos-run --update            fetch the newest image named by the manifest
  aienos-run --reset [--yes]     erase the persistent AienOS Store disk (a fresh, empty disk)
  aienos-run --where             show where images, the Store disk and settings live
  aienos-run --help | --version

options:
  --image <file>          boot this local image instead of the downloaded one
  --kind <kind>           image kind for --image: uefi-app (default), disk-raw, disk-qcow2, disk-vhdx, iso
  --arch <arch>           guest architecture: aarch64 or x86_64 (default: this computer's)
  --backend <name>        auto (default), kvm (own VMM, Linux), vz (Virtualization.framework, macOS), qemu
  --manifest <url>        manifest location (default $AIENOS_RUN_MANIFEST or the aienos.com one)
  --allow-test-key        accept a manifest signed with the public TEST key (never a release)
  --cpus <n>              virtual CPUs (default 1)
  --mem <MiB>             guest memory (default 1024)
  --forward <spec>        add a port forward: tcp|udp:[addr:]host-port:guest-port (repeatable)
  --no-default-forwards   drop the default signed-transport UDP forward
  --no-store              boot without the persistent Store disk
  --serial-log <file>     also write everything the guest prints to <file>
  --receipt               print boot timings (first output, console ready) when the VM ends
  --exit-on-ready         stop the VM as soon as AienOS reaches its console (measurements)
  --timeout <seconds>     stop the VM after this long
  --headless              do not attach the terminal (output still logged/printed)

in the console: everything you type goes to AienOS; Ctrl+] ends the session.
exit codes: 0 clean end, 1 VM failed, 2 usage error, 3 no backend can run here,
            4 download or verification failed, 5 timeout.
";

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Action {
    Run,
    Update,
    Reset,
    Where,
    Help,
    Version,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Options {
    pub action: Action,
    pub image: Option<PathBuf>,
    pub kind: Option<String>,
    pub arch: Option<String>,
    pub backend: String,
    pub manifest: Option<String>,
    pub allow_test_key: bool,
    pub cpus: u32,
    pub mem_mib: u32,
    pub forwards: Vec<Forward>,
    pub default_forwards: bool,
    pub store: bool,
    pub serial_log: Option<PathBuf>,
    pub receipt: bool,
    pub exit_on_ready: bool,
    pub timeout_s: Option<u64>,
    pub headless: bool,
    pub yes: bool,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            action: Action::Run,
            image: None,
            kind: None,
            arch: None,
            backend: "auto".into(),
            manifest: None,
            allow_test_key: false,
            cpus: 1,
            mem_mib: 1024,
            forwards: Vec::new(),
            default_forwards: true,
            store: true,
            serial_log: None,
            receipt: false,
            exit_on_ready: false,
            timeout_s: None,
            headless: false,
            yes: false,
        }
    }
}

pub fn parse<I: IntoIterator<Item = String>>(args: I) -> Result<Options, String> {
    let mut o = Options::default();
    let mut it = args.into_iter();
    let mut action_set = false;
    let mut set_action = |o: &mut Options, a: Action| -> Result<(), String> {
        if action_set {
            return Err("only one of --update, --reset, --where may be given".into());
        }
        action_set = true;
        o.action = a;
        Ok(())
    };
    while let Some(a) = it.next() {
        let mut val = |name: &str| it.next().ok_or_else(|| format!("{name} needs a value"));
        match a.as_str() {
            "-h" | "--help" => o.action = Action::Help,
            "-V" | "--version" => o.action = Action::Version,
            "--update" => set_action(&mut o, Action::Update)?,
            "--reset" => set_action(&mut o, Action::Reset)?,
            "--where" => set_action(&mut o, Action::Where)?,
            "--yes" | "-y" => o.yes = true,
            "--image" => o.image = Some(PathBuf::from(val("--image")?)),
            "--kind" => o.kind = Some(val("--kind")?),
            "--arch" => o.arch = Some(val("--arch")?),
            "--backend" => {
                let b = val("--backend")?;
                if !matches!(b.as_str(), "auto" | "kvm" | "qemu" | "whpx" | "vz") {
                    return Err(format!("unknown backend {b:?}"));
                }
                o.backend = b;
            }
            "--manifest" => o.manifest = Some(val("--manifest")?),
            "--allow-test-key" => o.allow_test_key = true,
            "--cpus" => {
                o.cpus = val("--cpus")?
                    .parse()
                    .ok()
                    .filter(|n| (1..=64).contains(n))
                    .ok_or("--cpus must be 1..64")?
            }
            "--mem" => {
                o.mem_mib = val("--mem")?
                    .parse()
                    .ok()
                    .filter(|n| (256..=65536).contains(n))
                    .ok_or("--mem must be 256..65536 MiB")?
            }
            "--forward" => o.forwards.push(Forward::parse(&val("--forward")?)?),
            "--no-default-forwards" => o.default_forwards = false,
            "--no-store" => o.store = false,
            "--serial-log" => o.serial_log = Some(PathBuf::from(val("--serial-log")?)),
            "--receipt" => o.receipt = true,
            "--exit-on-ready" => o.exit_on_ready = true,
            "--timeout" => {
                o.timeout_s = Some(
                    val("--timeout")?
                        .parse()
                        .ok()
                        .filter(|n| *n > 0)
                        .ok_or("--timeout must be a positive number of seconds")?,
                )
            }
            "--headless" => o.headless = true,
            other => return Err(format!("unknown argument {other:?} (try --help)")),
        }
    }
    if o.kind.is_some() && o.image.is_none() {
        return Err("--kind only applies with --image".into());
    }
    Ok(o)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(a: &[&str]) -> Result<Options, String> {
        parse(a.iter().map(|s| s.to_string()))
    }

    #[test]
    fn defaults_and_actions() {
        let o = p(&[]).unwrap();
        assert_eq!(o.action, Action::Run);
        assert_eq!((o.cpus, o.mem_mib, o.backend.as_str()), (1, 1024, "auto"));
        assert_eq!(p(&["--update"]).unwrap().action, Action::Update);
        assert_eq!(p(&["--where"]).unwrap().action, Action::Where);
        let r = p(&["--reset", "--yes"]).unwrap();
        assert!(r.yes && r.action == Action::Reset);
        assert!(p(&["--update", "--reset"]).is_err());
        assert_eq!(p(&["--help"]).unwrap().action, Action::Help);
    }

    #[test]
    fn options() {
        let o = p(&[
            "--image",
            "/x.efi",
            "--backend",
            "kvm",
            "--cpus",
            "4",
            "--mem",
            "2048",
            "--forward",
            "tcp:2222:22",
            "--no-default-forwards",
            "--receipt",
            "--exit-on-ready",
            "--timeout",
            "30",
        ])
        .unwrap();
        assert_eq!(o.image, Some(PathBuf::from("/x.efi")));
        assert_eq!(o.backend, "kvm");
        assert_eq!((o.cpus, o.mem_mib), (4, 2048));
        assert_eq!(o.forwards.len(), 1);
        assert!(!o.default_forwards && o.receipt && o.exit_on_ready);
        assert_eq!(o.timeout_s, Some(30));
    }

    #[test]
    fn errors() {
        for bad in [
            &["--cpus"][..],
            &["--cpus", "0"],
            &["--mem", "1"],
            &["--backend", "vbox"],
            &["--forward", "x"],
            &["--frob"],
            &["--kind", "iso"],
            &["--timeout", "0"],
        ] {
            assert!(p(bad).is_err(), "{bad:?}");
        }
    }
}
