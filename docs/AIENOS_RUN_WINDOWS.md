# AienOS on Windows: aienos-run.exe

`aienos-run.exe` runs AienOS in a virtual machine on a Windows 10 or 11 PC. It is one
program file: no installer, and no WSL. It downloads AienOS (the Aienux kernel and the
system around it), checks every byte against a signed list, and opens the AienOS console
in the window you started it from.

Ledger L261. Branch `feat/aienos-run-windows`. Shared core: `crates/aienos-run` (L262).

Every claim below carries a label. **OBSERVED** means seen on a real run, with the command
named. **INFERRED** means it follows from documentation or code but was not run. **UNKNOWN**
means not checked yet. No Windows PC was available while this was written, so nothing here
has been run on Windows yet.

## 1. Turn on the Windows Hypervisor Platform (once, then restart)

AienOS runs on the hypervisor that ships with Windows. It is switched off by default.

1. Press Start, type **Turn Windows features on or off**, and open it.
2. Tick **Windows Hypervisor Platform**, then click **OK**.
3. Restart the PC when Windows asks.

The same thing from an administrator PowerShell, followed by a restart:

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -All
```

or `DISM /Online /Enable-Feature /FeatureName:HypervisorPlatform /All`.

- Windows 11 **Home** has this feature (INFERRED from a third-party report showing it
  enabled on Home build 26200; not checked by us on a Home PC).
- The processor's virtualization must also be on in the PC's firmware setup. Task Manager >
  Performance > CPU shows **Virtualization: Enabled** when it is.
- If the feature is off, aienos-run stops before starting the VM and prints these steps
  (OBSERVED in source and unit tests; the Windows message itself is NOT_RUN).

## 2. Download and run

1. Download `aienos-run-windows-x86_64.exe` from `https://www.aienos.com/try/`.
   UNKNOWN: it is not published there yet; publishing is ledger L264's step.
2. Optional, to check the download: in PowerShell run
   `Get-FileHash .\aienos-run-windows-x86_64.exe` and compare the hash with the
   `runner windows-x86_64` line in section 7 and in `https://www.aienos.com/try/manifest.txt`.
3. Double-click the file, or run it from PowerShell or Windows Terminal.

**SmartScreen.** The exe is not code-signed yet, so the first start may show
"Windows protected your PC". Click **More info**, then **Run anyway**.

**First start** downloads, checks and keeps, under `%LOCALAPPDATA%\AienOS\cache`:

- the manifest and the AienOS disk image it names. Every file is checked against the
  SHA-256 the manifest lists. The manifest itself comes over HTTPS. Its Ed25519 signature
  line is optional today: no release signing key exists yet. aienos-run says on every start
  whether the manifest was unsigned, signed with the public TEST key, or signed with a
  pinned key.
- the QEMU runtime `aienos-qemu-win64.zip` (99,782,827 bytes, SHA-256
  `a88479b9c084a6edc6ba70849256524e81fbcd0474d07fa7947503e25d0f7056`, pinned inside the exe;
  unpacked with Windows' own `tar.exe`, then the zip is deleted). Later starts reuse it.

OBSERVED on the MacBook through the same code path (`AIENOS_RUN_FETCH_QEMU=1
AIENOS_RUN_QEMU_BUNDLE=<zip> aienos-run --update --arch x86_64 ...`, exit 0): 116 files
unpacked, the stamp written, the zip deleted, the second start reused it. A zip with one
byte changed was refused with exit 4 and nothing was installed.

## 3. Using the console

- Everything you type goes to AienOS. The prompt is `aienos> ` once the console is ready.
- **Ctrl+]** ends the session. **Ctrl+C**, Ctrl+Break and closing the window also stop the
  VM cleanly. aienos-run presses the VM's ACPI power button, so AienOS powers itself off.
  If AienOS does not respond, it types `poweroff`, then tells QEMU to quit, and only as the
  last step ends QEMU. It waits for QEMU to exit before it exits itself.
- The window stays open after a double-click start until you press Enter, so the last
  lines stay readable.

## 4. Commands

| command | what it does |
|---|---|
| `aienos-run` | boot AienOS (downloads on first use) |
| `aienos-run --update` | fetch the newest image the manifest names (the Store disk is kept) |
| `aienos-run --reset` | erase the AienOS Store disk and start with an empty one (asks first; `--yes` to skip) |
| `aienos-run --where` | show where the images, the Store disk, the QEMU runtime and the port forwards are |
| `aienos-run --backend whpx` | use the experimental own VMM (section 6) instead of QEMU |
| `aienos-run --help` | all options |

Exit codes: `0` clean end, `1` the VM failed, `2` usage error, `3` no backend can run on
this PC (the message says why, for example the feature in section 1), `4` download or
verification failed, `5` timeout.

**Store disk.** AienOS keeps its persistent Store on a 1 GiB disk at
`%LOCALAPPDATA%\AienOS\data\store.img`, one per Windows user. It survives `--update` and
new versions of the exe; only `--reset` empties it.

## 5. Network

The VM uses user-mode networking: it reaches the internet through Windows without any
adapter or administrator setup. Port forwards connect a port on this PC to a port in
AienOS. The default table forwards the signed transport:

| protocol | this PC | AienOS | why |
|---|---|---|---|
| UDP | 127.0.0.1:47030 | 47030 | signed transport (the port the kernel binds, `CK_NET_LOCAL_PORT`; provisional, owned by L263) |

Add more with `--forward udp:47031:47031` or `--forward tcp:0.0.0.0:8080:80`;
`--no-default-forwards` drops the default. `--where` prints the table in effect.

## 6. What it is, and what it is not yet

What it is:

- **Default path (candidate A):** QEMU 11.1.2 from the pinned bundle, run with
  `-accel whpx` (the Windows Hypervisor Platform), q35 machine, OVMF UEFI firmware, the
  serial console over a loopback socket, QMP for a clean stop.
- **Experimental path (candidate B, `--backend whpx`):** our own VMM written in Rust on the
  Windows Hypervisor Platform API, without QEMU or firmware. It reads `EFI/BOOT/BOOTX64.EFI`
  out of the image, loads it into guest memory and answers the few UEFI calls the kernel
  makes. It models COM1, HPET, the I/O APIC and ACPI power off; Windows emulates the local
  APIC.

What it is not yet:

- **Not run on Windows.** No Windows PC was reachable. Both paths compile for Windows and
  pass their unit tests, but neither has booted on Windows (NOT_RUN).
- **Candidate B is console-only:** one vCPU, no disk (no Store), no network, raw or qcow2
  images only. `auto` therefore always picks QEMU first.
- **No PC-edition image published yet.** The tests used a stand-in image (the AienOS x86
  kernel `IMACKERN.EFI` on a FAT disk, `scripts/aienos_run_standin_image.sh`).
- **Not code-signed** (hence the SmartScreen step), and the manifest has no release
  signature yet.
- x86-64 PCs only; no Windows on ARM build.

## 7. The exe and its manifest line

Built by `scripts/aienos_run_windows_exe.sh`, which cross-builds for
`x86_64-pc-windows-gnu`. Builder paths are stripped and the linker writes no time stamp. The
script fails if the exe names the build machine's home folder or imports any DLL that is
not a Windows system DLL. The hypervisor DLLs are loaded at run time, from System32 only.

OBSERVED on the MacBook, from commit `b6f4977` (a clean rebuild gave the same bytes):

```text
runner windows-x86_64 aienos-run-windows-x86_64.exe 1977911 96d9968e19e2e3f53c6209fc3cf68daa334c4b25eec5468621aa876c656d4489 aienos-run-windows-x86_64.exe
```

Put this line in the published manifest (before the `signature` line, then re-sign),
next to the exe and `aienos-qemu-win64.zip`. The zip is built by
`scripts/aienos_run_qemu_bundle.sh OUTDIR`, which must print
`AIENOS_RUN_QEMU_BUNDLE: PASS a88479b9...0f7056 99782827`.

## 8. Measurements

| what | where | result |
|---|---|---|
| A: launcher to VM started / first output / `console: ready` | MacBook (M2), same launcher, QEMU x86 **TCG** (no hardware acceleration for x86 on ARM), 10 runs, `scripts/aienos_run_measure.sh` | OBSERVED, ms min / median / max: 39.6 / 104.0 / 531.1; 1309.0 / 1605.0 / 3101.8; 2520.0 / 3233.7 / 5933.9 (load average about 9 from other jobs) |
| A on Windows with WHPX | Windows PC | NOT_RUN |
| B: build the machine (read the qcow2, extract the kernel, 1024 MiB RAM, tables, load) | MacBook, release build, 10 runs, stand-in test with `--nocapture` | OBSERVED, 0.27 to 1.37 ms (most runs about 0.27 ms) |
| B: hypervisor start and boot to console | Windows PC | NOT_RUN (the WHPX API exists only on Windows) |

The Mac numbers measure the launcher and the guest under emulation. They say nothing about
WHPX speed on Windows.
