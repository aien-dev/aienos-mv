//! aienos-run: boots AienOS in a VM on the host's own hypervisor.
//! See docs/AIENOS_RUN_MACOS.md (and the Linux / Windows docs).

use aienos_run::backend::{self, NetMode, StoreBus, VmSpec};
use aienos_run::{cli, console, paths};
use std::io::Write;
use std::time::{Duration, Instant};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let code = match cli::parse(&args) {
        Ok(c) => run(c),
        Err(e) => {
            eprintln!("aienos-run: {e}\n\n{}", cli::USAGE);
            2
        }
    };
    std::process::exit(code);
}

fn run(c: cli::Cmd) -> i32 {
    match c.action {
        cli::Action::Help => {
            println!("{}", cli::USAGE);
            0
        }
        cli::Action::Version => {
            println!("aienos-run {} ({})", env!("CARGO_PKG_VERSION"), backend::names().join(", "));
            0
        }
        cli::Action::Where => {
            let p = paths::Paths::for_user();
            println!("home:        {}", p.home.display());
            println!("boot disk:   {}", p.boot_disk.display());
            println!("store disk:  {}", p.store_disk.display());
            println!("efi vars:    {}", p.vars(c.backend.as_deref().unwrap_or("vz")).display());
            println!("console log: {}", p.console_log.display());
            0
        }
        cli::Action::Reset => {
            let p = paths::Paths::for_user();
            match p.reset_store() {
                Ok(()) => {
                    println!("aienos-run: Store disk and EFI variables removed; the next run starts fresh");
                    0
                }
                Err(e) => {
                    eprintln!("aienos-run: reset: {e}");
                    1
                }
            }
        }
        cli::Action::Run => boot(&c, 1).map(|_| 0).unwrap_or_else(|e| {
            eprintln!("aienos-run: {e}");
            1
        }),
        cli::Action::Measure(n) => measure(&c, n),
    }
}

struct Receipt {
    first_byte: Option<Duration>,
    marker: Option<Duration>,
    started: Option<Duration>,
    exit: String,
}

fn spec_for(c: &cli::Cmd, p: &paths::Paths, be: &str) -> Result<VmSpec, String> {
    let boot_disk = c.boot_disk.clone().unwrap_or_else(|| p.boot_disk.clone());
    if !boot_disk.exists() {
        return Err(format!("boot disk {} not found (pass --boot-disk or --efi)", boot_disk.display()));
    }
    let store_disk = if c.no_store {
        None
    } else {
        let s = c.store_disk.clone().unwrap_or_else(|| p.store_disk.clone());
        paths::ensure_blank_disk(&s, c.store_mib)?;
        Some(s)
    };
    let mut forwards = backend::default_forwards();
    forwards.extend(c.forwards.iter().cloned());
    forwards.dedup_by(|a, b| a.host == b.host && a.proto == b.proto);
    Ok(VmSpec {
        boot_disk,
        boot_read_only: !c.boot_rw,
        store_disk,
        store_bus: if c.store_virtio { StoreBus::VirtioBlk } else { StoreBus::Nvme },
        efi_vars: c.efi_vars.clone().unwrap_or_else(|| p.vars(be)),
        cpus: c.cpus,
        mem_mib: c.mem_mib,
        net: if c.no_net { NetMode::None } else { NetMode::Nat },
        mac: Some(paths::stable_mac(&p.home)),
        forwards,
        framebuffer: c.window,
    })
}

fn boot(c: &cli::Cmd, run_no: u32) -> Result<Receipt, String> {
    let p = paths::Paths::for_user();
    p.ensure()?;
    if let Some(efi) = &c.efi {
        let disk = c.boot_disk.clone().unwrap_or_else(|| p.boot_disk.clone());
        aienos_run::esp::write_boot_disk(&disk, efi, 64)?;
    }
    let be = backend::for_host(c.backend.as_deref())?;
    be.available()?;
    let spec = spec_for(c, &p, be.name())?;
    let interactive = !c.measuring;
    let t0 = Instant::now();
    let (mut vm, con) = be.boot(&spec)?;
    let log = std::fs::File::create(c.log.clone().unwrap_or_else(|| p.console_log.clone())).ok();
    let echo: Option<Box<dyn Write + Send>> = if c.quiet { None } else { Some(Box::new(std::io::stdout())) };
    let pump = console::spawn(con.from_guest, t0, c.marker.clone(), echo, log);
    if interactive {
        let mut to = con.to_guest;
        std::thread::spawn(move || {
            let mut b = [0u8; 256];
            use std::io::Read;
            let mut stdin = std::io::stdin();
            while let Ok(n) = stdin.read(&mut b) {
                if n == 0 || to.write_all(&b[..n]).is_err() {
                    break;
                }
                let _ = to.flush();
            }
        });
    }
    let deadline = Duration::from_secs(c.timeout_s);
    let mut started = None;
    let mut stop_sent: Option<Instant> = None;
    let exit;
    loop {
        if started.is_none() {
            if let Some(r) = vm.started() {
                r.map_err(|e| format!("{} start: {e}", be.name()))?;
                started = Some(t0.elapsed());
            }
        }
        if let Some(r) = vm.poll_exit() {
            exit = match r {
                Ok(()) => "guest stopped".to_string(),
                Err(e) => e,
            };
            break;
        }
        let tm = pump.timing.lock().unwrap().clone();
        if c.stop_at_marker && tm.marker.is_some() && stop_sent.is_none() {
            vm.force_stop();
            exit = "stopped by runner at marker".into();
            break;
        }
        if t0.elapsed() > deadline && stop_sent.is_none() {
            vm.request_stop();
            stop_sent = Some(Instant::now());
        }
        if let Some(s) = stop_sent {
            if s.elapsed() > Duration::from_secs(10) {
                vm.force_stop();
                exit = format!("timeout after {} s, forced", c.timeout_s);
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    // Let the pump drain what is already in the pipe.
    std::thread::sleep(Duration::from_millis(50));
    let tm = pump.timing.lock().unwrap().clone();
    let r = Receipt { first_byte: tm.first_byte, marker: tm.marker, started, exit };
    if c.measuring || c.receipt {
        let ms = |d: Option<Duration>| d.map(|d| format!("{:.1}", d.as_secs_f64() * 1000.0)).unwrap_or_else(|| "null".into());
        eprintln!(
            "{{\"run\":{run_no},\"backend\":\"{}\",\"start_confirmed_ms\":{},\"first_console_byte_ms\":{},\"marker\":{:?},\"marker_ms\":{},\"console_bytes\":{},\"exit\":{:?}}}",
            be.name(), ms(r.started), ms(r.first_byte), c.marker, ms(r.marker), tm.bytes, r.exit
        );
    }
    Ok(r)
}

fn measure(c: &cli::Cmd, n: u32) -> i32 {
    let mut marks = Vec::new();
    for i in 1..=n {
        match boot(c, i) {
            Ok(r) => match r.marker {
                Some(m) => marks.push(m.as_secs_f64() * 1000.0),
                None => {
                    eprintln!("aienos-run: run {i}: marker {:?} not seen ({})", c.marker, r.exit);
                    return 1;
                }
            },
            Err(e) => {
                eprintln!("aienos-run: run {i}: {e}");
                return 1;
            }
        }
    }
    marks.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let median = if marks.len() % 2 == 1 {
        marks[marks.len() / 2]
    } else {
        (marks[marks.len() / 2 - 1] + marks[marks.len() / 2]) / 2.0
    };
    eprintln!(
        "{{\"summary\":true,\"runs\":{},\"marker\":{:?},\"min_ms\":{:.1},\"median_ms\":{:.1},\"max_ms\":{:.1}}}",
        marks.len(), c.marker, marks[0], median, marks[marks.len() - 1]
    );
    0
}
