//! The program's behaviour: run, --update, --reset, --where.

use crate::backend::{self, VmExit, VmSpec};
use crate::bundle;
use crate::cli::{self, Action, Options};
use crate::console::{self, Console, RawTerminal, DEFAULT_READY_MARKERS};
use crate::disk;
use crate::fetch;
use crate::manifest::{hex_encode, Arch, ImageEntry, ImageKind, Manifest, SignatureState, Trust};
use crate::net;
use crate::paths::Paths;
use std::io::{IsTerminal, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

pub const DEFAULT_MANIFEST_URL: &str = "https://www.aienos.com/try/manifest.txt";

pub const EXIT_OK: i32 = 0;
pub const EXIT_VM_FAILED: i32 = 1;
pub const EXIT_USAGE: i32 = 2;
pub const EXIT_NO_BACKEND: i32 = 3;
pub const EXIT_FETCH: i32 = 4;
pub const EXIT_TIMEOUT: i32 = 5;

fn say(msg: &str) {
    eprintln!("aienos-run: {msg}");
}

pub fn main(args: Vec<String>) -> i32 {
    let opts = match cli::parse(args) {
        Ok(o) => o,
        Err(e) => {
            say(&e);
            return EXIT_USAGE;
        }
    };
    match opts.action {
        Action::Help => {
            print!("{}", cli::USAGE);
            return EXIT_OK;
        }
        Action::Version => {
            println!("aienos-run {}", env!("CARGO_PKG_VERSION"));
            return EXIT_OK;
        }
        _ => {}
    }
    let paths = match Paths::from_env(|k| std::env::var(k).ok()) {
        Ok(p) => p,
        Err(e) => {
            say(&e);
            return EXIT_USAGE;
        }
    };
    match opts.action {
        Action::Where => where_cmd(&opts, &paths),
        Action::Reset => reset_cmd(&opts, &paths),
        Action::Update => match update(&opts, &paths, true) {
            Ok(_) => match qemu_runtime(&opts, &paths) {
                Ok(()) => EXIT_OK,
                Err(code) => code,
            },
            Err(code) => code,
        },
        Action::Run => run_cmd(&opts, &paths),
        Action::Help | Action::Version => EXIT_OK,
    }
}

/// The guest arch's QEMU runtime: on Windows (or with `AIENOS_RUN_FETCH_QEMU=1`) the pinned
/// bundle is downloaded and unpacked when the qemu backend may run and no QEMU is found.
fn qemu_runtime(opts: &Options, paths: &Paths) -> Result<(), i32> {
    let forced = std::env::var("AIENOS_RUN_FETCH_QEMU").is_ok_and(|v| v == "1");
    if !(cfg!(windows) || forced) || !matches!(opts.backend.as_str(), "auto" | "qemu") {
        return Ok(());
    }
    if guest_arch(opts) != Ok(Arch::X86_64) {
        return Ok(());
    }
    let q = backend::qemu::QemuBackend::from_env();
    if !forced && backend::qemu::find_qemu(Arch::X86_64, &q.search_dirs).is_some() {
        return Ok(());
    }
    let override_url = std::env::var("AIENOS_RUN_QEMU_BUNDLE").ok();
    let url = bundle::source_url(
        override_url.as_deref(),
        &manifest_url(opts),
        &bundle::QEMU_BUNDLE,
    );
    match bundle::ensure(
        &paths.cache,
        &url,
        &bundle::QEMU_BUNDLE,
        &bundle::tar_extract,
        &say,
    ) {
        Ok(d) => {
            say(&format!("QEMU runtime ready in {}", d.display()));
            Ok(())
        }
        Err(e) => {
            say(&e);
            Err(EXIT_FETCH)
        }
    }
}

fn manifest_url(opts: &Options) -> String {
    opts.manifest
        .clone()
        .or_else(|| {
            std::env::var("AIENOS_RUN_MANIFEST")
                .ok()
                .filter(|s| !s.is_empty())
        })
        .unwrap_or_else(|| DEFAULT_MANIFEST_URL.to_string())
}

fn guest_arch(opts: &Options) -> Result<Arch, String> {
    match &opts.arch {
        Some(a) => Arch::parse(a).ok_or_else(|| format!("unknown arch {a:?}")),
        None => Ok(Arch::host()),
    }
}

fn where_cmd(opts: &Options, paths: &Paths) -> i32 {
    println!("images:        {}", paths.images().display());
    println!("manifest copy: {}", paths.manifest_copy().display());
    println!("store disk:    {}", paths.store_disk().display());
    println!("efi variables: {}", paths.firmware_vars().display());
    println!("manifest url:  {}", manifest_url(opts));
    let q = paths.cache.join(bundle::DIR);
    let state = if bundle::installed(&paths.cache, &bundle::QEMU_BUNDLE) {
        "installed"
    } else {
        "not downloaded"
    };
    println!("qemu runtime:  {} ({state})", q.display());
    let mut table = if opts.default_forwards {
        net::default_forwards()
    } else {
        Vec::new()
    };
    table.extend(opts.forwards.iter().cloned());
    for f in &table {
        println!("forward:       {f}");
    }
    EXIT_OK
}

fn reset_cmd(opts: &Options, paths: &Paths) -> i32 {
    let p = paths.store_disk();
    if !opts.yes {
        if !std::io::stdin().is_terminal() {
            say(
                "--reset erases the AienOS Store disk; add --yes to confirm when not at a terminal",
            );
            return EXIT_USAGE;
        }
        eprint!("Erase the AienOS Store disk at {}? Type yes: ", p.display());
        let _ = std::io::stderr().flush();
        let mut line = String::new();
        let _ = std::io::stdin().read_line(&mut line);
        if line.trim() != "yes" {
            say("not erased");
            return EXIT_OK;
        }
    }
    match disk::reset(&p, disk::DEFAULT_STORE_BYTES) {
        Ok(()) => {
            say(&format!("Store disk reset: {} (empty)", p.display()));
            EXIT_OK
        }
        Err(e) => {
            say(&format!("cannot reset {}: {e}", p.display()));
            EXIT_VM_FAILED
        }
    }
}

fn wanted_kinds(arch: Arch) -> &'static [ImageKind] {
    match arch {
        Arch::Aarch64 => &[ImageKind::UefiApp, ImageKind::DiskRaw, ImageKind::DiskQcow2],
        Arch::X86_64 => &[
            ImageKind::DiskQcow2,
            ImageKind::DiskRaw,
            ImageKind::Iso,
            ImageKind::UefiApp,
        ],
    }
}

/// Fetches the manifest (always when `force`, else only when no copy is cached), then
/// the image for the guest arch. Returns the image entry and its verified path.
fn update(opts: &Options, paths: &Paths, force: bool) -> Result<(ImageEntry, PathBuf), i32> {
    let arch = guest_arch(opts).map_err(|e| {
        say(&e);
        EXIT_USAGE
    })?;
    let url = manifest_url(opts);
    let trust = Trust {
        pinned: Vec::new(),
        allow_test_key: opts.allow_test_key,
        require_signature: false,
    };
    if let Err(e) = std::fs::create_dir_all(paths.images()) {
        say(&format!("cannot create {}: {e}", paths.images().display()));
        return Err(EXIT_FETCH);
    }
    let copy = paths.manifest_copy();
    let text = if force || !copy.exists() {
        say(&format!("fetching manifest {url}"));
        fetch::read_text(&url, &paths.cache.join("manifest.download")).map_err(|e| {
            say(&e.to_string());
            EXIT_FETCH
        })?
    } else {
        std::fs::read_to_string(&copy).map_err(|e| {
            say(&format!("cannot read {}: {e}", copy.display()));
            EXIT_FETCH
        })?
    };
    let m = Manifest::parse(&text, &trust).map_err(|e| {
        say(&e.to_string());
        EXIT_FETCH
    })?;
    match &m.signature {
        SignatureState::Unsigned => {
            say("manifest is UNSIGNED: integrity rests on the sha256 lines only")
        }
        SignatureState::TestKey => {
            say("manifest signed with the public TEST key (not a release signature)")
        }
        SignatureState::Pinned(k) => {
            say(&format!("manifest signed by pinned key {}", hex_encode(k)))
        }
    }
    let entry = m.image(arch, wanted_kinds(arch)).cloned().ok_or_else(|| {
        say(&format!("the manifest has no {} image", arch.as_str()));
        EXIT_FETCH
    })?;
    let file_url = crate::manifest::resolve_url(&url, &entry.file.url);
    let (path, downloaded) = fetch::fetch_verified(&file_url, &entry.file, &paths.images())
        .map_err(|e| {
            say(&format!("{}: {e}", entry.file.name));
            EXIT_FETCH
        })?;
    if force || !copy.exists() {
        let _ = std::fs::write(&copy, &text);
    }
    say(&format!(
        "image {} {} sha256 {} {}",
        entry.file.name,
        entry.kind.as_str(),
        hex_encode(&entry.file.sha256),
        if downloaded {
            "downloaded and verified"
        } else {
            "verified (cached)"
        }
    ));
    Ok((entry, path))
}

fn run_cmd(opts: &Options, paths: &Paths) -> i32 {
    let arch = match guest_arch(opts) {
        Ok(a) => a,
        Err(e) => {
            say(&e);
            return EXIT_USAGE;
        }
    };
    let (image, kind) = match &opts.image {
        Some(p) => {
            let k = opts.kind.as_deref().unwrap_or("uefi-app");
            let Some(k) = ImageKind::parse(k) else {
                say(&format!("unknown image kind {k:?}"));
                return EXIT_USAGE;
            };
            if !p.is_file() {
                say(&format!("no such image file {}", p.display()));
                return EXIT_USAGE;
            }
            (p.clone(), k)
        }
        None => match update(opts, paths, false) {
            Ok((e, p)) => (p, e.kind),
            Err(code) => return code,
        },
    };
    if let Err(code) = qemu_runtime(opts, paths) {
        return code;
    }
    let mut forwards = if opts.default_forwards {
        net::default_forwards()
    } else {
        Vec::new()
    };
    forwards.extend(opts.forwards.iter().cloned());
    if let Err(e) = net::check_table(&forwards) {
        say(&e);
        return EXIT_USAGE;
    }
    let store_disk = if opts.store {
        let p = paths.store_disk();
        match disk::ensure(&p, disk::DEFAULT_STORE_BYTES) {
            Ok(true) => say(&format!(
                "created the persistent Store disk {}",
                p.display()
            )),
            Ok(false) => {}
            Err(e) => {
                say(&format!("cannot create {}: {e}", p.display()));
                return EXIT_VM_FAILED;
            }
        }
        Some(p)
    } else {
        None
    };
    let scratch = std::env::temp_dir().join(format!("aienos-run-{}", std::process::id()));
    if let Err(e) = std::fs::create_dir_all(&scratch) {
        say(&format!("cannot create {}: {e}", scratch.display()));
        return EXIT_VM_FAILED;
    }
    let spec = VmSpec {
        arch,
        image,
        image_kind: kind,
        store_disk,
        firmware_vars: Some(paths.firmware_vars()),
        mem_mib: opts.mem_mib,
        cpus: opts.cpus,
        forwards,
        scratch: scratch.clone(),
    };
    let code = boot(opts, &spec);
    let _ = std::fs::remove_dir_all(&scratch);
    code
}

fn boot(opts: &Options, spec: &VmSpec) -> i32 {
    let mut b = match backend::select(&opts.backend, spec) {
        Ok(b) => b,
        Err(e) => {
            say(&e.to_string());
            return EXIT_NO_BACKEND;
        }
    };
    say(&format!(
        "booting {} ({} {}) with the {} backend, {} vCPU, {} MiB; Ctrl+] ends the session",
        spec.image.display(),
        spec.arch.as_str(),
        spec.image_kind.as_str(),
        b.name(),
        spec.cpus,
        spec.mem_mib
    ));
    for f in &spec.forwards {
        say(&format!("forward {f}"));
    }
    let mut markers: Vec<String> = DEFAULT_READY_MARKERS
        .iter()
        .map(|s| s.to_string())
        .collect();
    markers.extend(opts.ready_markers.iter().cloned());
    let t0 = Instant::now();
    // The Windows console refuses writes that are not valid UTF-8; guest bytes are anything.
    let out: Box<dyn Write + Send> = if cfg!(windows) {
        Box::new(console::Utf8Lossy::new(std::io::stdout()))
    } else {
        Box::new(std::io::stdout())
    };
    let mut c = Console::new(Some(out), &markers, t0);
    if let Some(p) = &opts.serial_log {
        match std::fs::File::create(p) {
            Ok(f) => c = c.with_log(f),
            Err(e) => {
                say(&format!("cannot write {}: {e}", p.display()));
                return EXIT_USAGE;
            }
        }
    }
    if opts.exit_on_ready {
        c = c.stop_when_ready();
    }
    let console = Arc::new(c);
    console::install_signal_stop();
    let raw = if opts.headless {
        None
    } else {
        Some(RawTerminal::enter())
    };
    if !opts.headless {
        console::spawn_stdin_pump(console.clone(), false);
    }
    let timed_out = Arc::new(AtomicBool::new(false));
    if let Some(s) = opts.timeout_s {
        let (c, t) = (console.clone(), timed_out.clone());
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_secs(s));
            t.store(true, Ordering::SeqCst);
            c.request_stop();
        });
    }
    let result = b.run(spec, &console);
    console::mark_session_done();
    drop(raw);
    let times = console.times();
    if opts.receipt {
        let ms = |d: Option<Duration>| match d {
            Some(d) => format!("{:.1} ms", d.as_secs_f64() * 1000.0),
            None => "NOT_SEEN".to_string(),
        };
        eprintln!();
        say(&format!(
            "receipt backend={} arch={}",
            b.name(),
            spec.arch.as_str()
        ));
        say(&format!("receipt vmm_started={}", ms(times.vmm_started)));
        say(&format!("receipt first_output={}", ms(times.first_byte)));
        match times.ready() {
            Some((m, t)) => say(&format!(
                "receipt console_ready={} marker={m:?}",
                ms(Some(t))
            )),
            None => say("receipt console_ready=NOT_SEEN"),
        }
        say(&format!("receipt total={}", ms(Some(console.elapsed()))));
    }
    match result {
        Ok(exit) => {
            let code = if timed_out.load(Ordering::SeqCst) && exit == VmExit::Stopped {
                say("timeout: VM stopped");
                EXIT_TIMEOUT
            } else {
                exit.exit_code()
            };
            say(&format!("VM ended: {exit:?}"));
            code
        }
        Err(e) => {
            say(&e.to_string());
            EXIT_VM_FAILED
        }
    }
}
