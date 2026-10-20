# aienos-run on Linux

`aienos-run` boots AienOS (the Aienux kernel) in a virtual machine on your Linux
computer, in one command, without touching your system: no root, no installer, nothing
outside your home directory. It is the Linux member of one program that also runs on
Windows (ledger L261, `docs/AIENOS_RUN_WINDOWS.md`) and macOS; all three share the same
core (`crates/aienos-run`) and differ only in the backend that drives the hardware.

## Try it

    curl -fsSL https://www.aienos.com/try | sh

That script (`scripts/try.sh` in this repo):

1. downloads the aienos-run manifest (a short text file listing every file, its size and
   its SHA-256),
2. downloads `aienos-run` for your computer (Linux or macOS, aarch64 or x86_64), checks
   its size and SHA-256 against the manifest and refuses to install it on any mismatch,
3. installs it as `~/.local/bin/aienos-run`,
4. runs it. `aienos-run` downloads the AienOS image named by the same manifest, checks
   its SHA-256 the same way, and boots it to the AienOS console.

Press **Ctrl+]** to end the session. Everything else you type goes to AienOS.

Arguments go through to aienos-run: `curl -fsSL https://www.aienos.com/try | sh -s -- --backend qemu`.

Status (2026-10-20): the page at aienos.com/try and the published files are ledger L264's
job; until they are live the command fails at step 1. Everything else is tested on the
Spark with local files (`scripts/aienos_run_try_test.sh`, see Evidence).

## Commands (WSL-like)

| Command | What it does |
| --- | --- |
| `aienos-run` | Boots AienOS. First use downloads and verifies the image. |
| `aienos-run --update` | Fetches the manifest again and downloads a newer image if there is one. |
| `aienos-run --reset` | Erases the persistent AienOS Store disk (asks; `--yes` to skip the question). |
| `aienos-run --where` | Shows where the image, the Store disk and the settings live, and the port forwards. |

Useful options: `--backend kvm|qemu`, `--cpus N`, `--mem MiB`, `--forward udp:47040:47040`,
`--image <file>` (boot a local image), `--serial-log <file>`, `--receipt` (print boot timings),
`--headless`. `aienos-run --help` lists them all.

Exit codes: 0 clean end (AienOS powered off or reset, or you pressed Ctrl+]), 1 the VM
failed, 2 usage error, 3 no backend can run here, 4 download or verification failed,
5 timeout.

## Where files live

- Images: `~/.cache/aienos-run/images` (`$XDG_CACHE_HOME`), plus the manifest copy.
- Persistent Store disk: `~/.local/share/aienos-run/store.img` (`$XDG_DATA_HOME`), 1 GiB,
  sparse. It survives `--update`; `--reset` recreates it empty.
- `AIENOS_RUN_HOME=<dir>` moves everything under `<dir>`.

## Backends on Linux

| Backend | Runs | How |
| --- | --- | --- |
| `kvm` (default on aarch64) | aarch64 AienOS on an aarch64 Linux host | Our own VMM on `/dev/kvm`, direct boot (below) |
| `qemu` | aarch64 or x86_64 images | QEMU with KVM when the architectures match, TCG emulation when they do not |

**x86_64 images (the PC edition, ledger L260) on the aarch64 Spark run under QEMU TCG
only: emulated instruction by instruction, never hardware-accelerated.** On an x86_64
Linux PC they would run under QEMU with KVM; the own VMM is aarch64-only today.

`--backend auto` takes `kvm` when it can run the image and `qemu` otherwise.

### The own VMM (`--backend kvm`)

About 600 lines of Rust in `crates/aienos-run/src/backend/kvm/`, no outside VMM library:

- `/dev/kvm` ioctls (`sys.rs`): every number was printed from the host's own kernel
  headers by a C program, not typed from memory.
- **Direct boot, no firmware.** The VMM loads the kernel's PE image (`BOOTAA64.EFI`) into
  guest RAM, writes ACPI tables (RSDP, XSDT, FADT with PSCI over HVC, MADT with one GICC
  per vCPU and its real MPIDR, SPCR for the PL011, DSDT), a UEFI-format memory map and a
  56-byte direct-boot record (`native/boot/direct_boot.h`). It leaves the boot vCPU at EL1h
  with an identity map and caches on, exactly as UEFI would, and starts the image's own
  entry point with `x0 = "AIENDIRB"` (never a valid UEFI handle) and `x1 = the record`.
  The boot stub (`native/boot/efi_main.c`, `direct_boot()`) builds the same CHANDOF3
  handoff the UEFI path builds and enters the kernel. The direct path is compiled out of
  `CK_HARDWARE_STAGING` images.
- KVM supplies the CPU, the GICv3 (in-kernel vGIC), the generic timer and PSCI. The VMM
  emulates the PL011 serial port and ends the run on PSCI SYSTEM_OFF / SYSTEM_RESET.
- References read, nothing copied: the Linux KVM API document (GPL-2.0 documentation),
  kvmtool (GPL-2.0) and Firecracker (Apache-2.0) for the arm64 vCPU and vGIC setup order.

## Networking and port forwards

The QEMU backend gives AienOS user-mode networking (the guest reaches your LAN through
your computer's own network stack; no root, no bridge) with a port-forward table. The
default table forwards the signed transport's UDP port **47040, provisional**: no port
number is fixed on aienos main yet, 47040 sits clear of the iMac lab ports 47029-47035,
and the Interplane lane (ledger L263) owns confirming it. Add more with `--forward`.

The own VMM has no network device yet (see Not yet).

## Evidence

(filled in from the runs on the Spark: commands, exit codes, receipts)

## Not yet

- The own VMM boots the core kernel image to its console report. It has no virtio
  devices: the aarch64 kernel has no virtio-blk or virtio-mmio driver on main (OBSERVED:
  no such driver in `native/kernel/dev`), so the Store disk and networking are QEMU-only
  until both sides exist.
- The manifest is unsigned until the owner release key exists (owner ceremony
  BLOCKED_OPERATOR); integrity today rests on HTTPS plus the SHA-256 lines. aienos-run
  verifies Ed25519 signatures (pinned keys; the public TEST key only with
  `--allow-test-key`), ready for the release key.
- Release binaries and the aienos.com page are not published (L264).
