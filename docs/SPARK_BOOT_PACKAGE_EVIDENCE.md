# Spark boot package: what was proven without rebooting (L197, 2026-10-20)

Companion to `docs/SPARK_BOOT_CHECKLIST.md`. Everything here ran on the Spark
(`spark-b87b`) under Linux, on branch `feat/spark-boot-package`. Nothing was
staged, no boot variable or ESP file was written, nothing rebooted, no GPU
firmware was loaded. Raw logs: `~/handoffs/L197/` on the Spark.

## 1. Builds

Built at `961bceb` with `~/handoffs/L197/build.sh` under `quiet-build`. Every
step exit 0, zero compiler warnings in every log.

| Image | Command | sha256 at 961bceb | Size |
| --- | --- | --- | --- |
| Rust handoff image (the boot candidate) | `AIENOS_COMMIT=<commit> cargo build --release -p aienos-boot --target aarch64-unknown-uefi --features handoff,hardware-staging --bin aienos-handoff`, then `cargo run --release -p aienos-evidence -- verify-efi` (PASS) | `f4ffd9580fb2a9bb5881d0fb9c4bd52b7e7d74e94aaf96ce5157baad2af45b0c` | 144384 |
| C kernel inference image (what `stage_one_time_boot_ck.sh` stages) | `cargo build --release --target aarch64-unknown-none --manifest-path crates/aienos-infer-kernel/Cargo.toml` (target dir `target/infer-kernel`), then `make -C native/kernel OUT=target/native-kernel-l6c-staging CK_INFER_LIB=<lib> CK_FB_HOLD_S=120`, `make ... model-map` | `f445e7f7c23b81afc0897fede94931e211c370a4b01344759ac79809f20f7f91` | 487424 |
| C kernel core image | `make -C native/kernel` | `36f2b19a493413136ffabe91d8f1ed6cdb0dd61f92a478e02d18eb504343fd07` | 159744 |
| C kernel full image | `make -C native/kernel full` | `686716a384bef866fb0508919488839b75f10039f6140563c642f0495ef4f43a` | 364544 |

The images carry the commit they were built from, so their digests change
with every commit. The staging command builds at the commit it runs on and
prints the digest it staged; that printed digest is the one the evidence
uses. The handoff dry run at `70ca2c4` built
`87a2d3d0e02aecd6c14ebc99df811865cd93eaa0269586ac7369a8fa167e0c91`.

## 2. Gates

Run by `~/handoffs/L197/gates.sh` at `d3594cc`, sequentially, under
`quiet-build`. QEMU is an emulator: every PASS here is emulator evidence,
hardware NOT_RUN.

| Gate | Command | Exit | Verdict |
| --- | --- | --- | --- |
| Preflight self-test | `bash scripts/native_boot_preflight.sh --self-test` | 0 | `NATIVE_PREFLIGHT_SELF_TEST: PASS` |
| Handoff image boot in QEMU | `./scripts/qemu_boot_test.sh` | 0 | `QEMU_BOOT: PASS` |
| One-time boot rollback, six branches | `./scripts/qemu_native_rollback_test.sh` | 0 | `M0_NATIVE_ROLLBACK_QEMU: PASS` |
| C kernel QEMU gates | `./scripts/ck_gates.sh` | 0 | 26 PASS, 0 FAIL, 1 NOT_RUN: `AIENOS_CK_SLOT_LOADER: NOT_RUN` because the QEMU gate lock was held by a parallel run of the same suite in another lane (L201, `qemu_ck_slot_loader_test` pid 2078053); receipt `evidence/ck_gates_75ffb00fa3702fc2e56764b8e474cd48fd27b3f791d5ac1e4860708faa6b599f.json`. Retry: section 2.1 |
| Full strict verification | `AIENOS_STRICT=1 ./scripts/verify_all.sh` | 0 | `VERIFY_ALL: PASS (every step ran; ...)`, zero warnings |

Note on the handoff image: `qemu_boot_test.sh` builds it with features
`handoff` and a 1 s countdown (`AIENOS_RESTART_SECS=1`); the staged image
adds `hardware-staging` (which only changes the reported tier) and keeps the
30 s countdown. The emulator has no GB10, so it shows the `gb10: unavailable`
branch.

### 2.1 Retries after the lock contention

`~/handoffs/L197/qemu-retry.sh` at `70ca2c4` retries a gate while it reports NOT_RUN (exit 3, lock held), once a minute:

| Gate | Command | Exit | Verdict |
| --- | --- | --- | --- |
| C kernel slot loader | `./scripts/qemu_ck_slot_loader_test.sh` | 0 (5th try) | `AIENOS_CK_SLOT_LOADER: PASS (15 rows, 23 boots, 161 checks; QEMU, not hardware)` |
| Handoff image boot, serial kept | `AIENOS_LOG_DIR=~/handoffs/L197 ./scripts/qemu_boot_test.sh` | 0 | `QEMU_BOOT: PASS` |

With the retry, all 27 C kernel gates have passed on this branch (26 in the
receipt at `d3594cc`, the slot loader at `70ca2c4`, a commit that adds only
that receipt).

## 3. The machine, read-only (2026-10-20 ~16:06Z)

`efibootmgr -v`, the SecureBoot variable, `mokutil --sb-state`, `findmnt`,
`df`, `lsblk`, `lspci -nn -d 10de:`, `ls /boot/efi/EFI/AIENOS`
(`~/handoffs/L197/machine-readonly.txt`):

- BootCurrent `0001`, BootOrder `0001,0003`, no BootNext.
- Entries: `Boot0000 AIENOS Allen` (`\EFI\AIENOS\ALLEN.EFI`, staged
  2026-10-18, not in BootOrder), `Boot0001 ubuntu` (shim), `Boot0003` PXE.
  **No recovery stick entry**: the stick is not plugged in (`lsusb` lists only
  the UPS and the YubiKey; no keyboard either).
- Secure Boot: disabled (variable byte 0; `mokutil`: SecureBoot disabled).
- ESP `/dev/nvme0n1p1` vfat `9DA2-3597` at `/boot/efi`, 298 MB, 291 MB free.
  Root `/dev/nvme0n1p2` ext4 `d27bfd26-...` (3.7 TB).
- GB10 `000f:01:00.0` VGA `[10de:2e12]` behind bridge `000f:00:00.0`
  `[10de:22d1]`.
- `\EFI\AIENOS` exists: `aienos-handoff.efi` (84480 B, 2026-09-23),
  `ALLEN.EFI` (2026-10-18), empty `BOOTREPORT.TXT`, `STAGED.TXT`.

## 4. Dry runs

Both at `70ca2c4`, read-only (`~/handoffs/L197/dryrun-*.txt`).

- `scripts/stage_one_time_boot.sh --dry-run`: image built and verified;
  preflight `NOT_READY (1 STOP)`, the STOP being the recovery stick not
  listed; every other check PASS (Linux first, no BootNext, running from
  Linux, ESP mounted, no quiet flag, clean tree, image digest matches).
- `scripts/stage_one_time_boot_ck.sh --dry-run`: one STOP, the same stick.

### Bug found and fixed on the branch

Before `a99c551` both dry runs would have refused **even with the stick
plugged in**: the four C kernel stagers and `native_boot_preflight.sh`
required a `Boot0004` line containing the word `AIENOSRECOV`, but the
firmware names the stick's entry itself (`UEFI: USB USB Hard Drive,
Partition 1`, `evidence/gate1_machine1_selftest_report_2026-09-24.txt`) and
picks its number. The self-test used an invented `Boot0004* AIENOSRECOV`
line, so it never saw this. Now `scripts/lib_recovery_entry.sh` finds the
entry by the stick's GPT partition GUID
`f227357c-1e80-4fd0-a55e-2efbef1dd25c` under any number. Red before (three
self-test cases FAIL, including "an entry merely labelled AIENOSRECOV on
another disk passes"), green after. Checked against a stand-in `efibootmgr`
listing the real stick line as `Boot0002`: all four C kernel stagers report
`recovery: Boot0002 AIENOSRECOV present`.

`961bceb` brought the handoff stager up to the C kernel stagers' safety
level: `--apply` writes nothing unless the preflight is READY for the exact
image digest, takes the pre-boot capture itself, re-checks the stick before
setting BootNext, and refuses to run under `sudo`.

## 5. What the emulator shows (handoff image)

`AIENOS_LOG_DIR=~/handoffs/L197 ./scripts/qemu_boot_test.sh` at `70ca2c4`
(exit 0, `QEMU_BOOT: PASS`), serial console, abridged. On the Spark the same
lines appear on the HDMI screen, with the Spark firmware vendor, 20 cores,
and either the `gb10_pci:` lines or the Spark root bridges (segment 15 for
the GB10) in place of the emulator values.

    report_version: 1
    aienos_commit: 70ca2c4747260430903f0d13db2c5e6ce960915c
    report_kind: pre_exit
    firmware_vendor: Ubuntu distribution of EDK II
    gb10: unavailable
    gb10_pci_root_bridges: 1
    gb10_pci_root[0]: segment 0 devices 2 bridges 0 windows 0
    cpu_cores: 4
    framebuffer: 800x600 stride 800 Bgr base 0xbc7a0000 size 3145728
    exiting firmware boot services
    report file: \EFI\AIENOS\BOOTREPORT.TXT saved
    ...
    report_kind: final
    nvram_write_index: 2 of 3
    kernel_el: EL1h
    mmu: enabled
    AIENOS
    boot: native
    gb10: unavailable
    kernel: alive
    gb10_pci_root_bridges: 1
    gb10_pci_root[0]: segment 0 devices 2 bridges 0 windows 0
    progress_record: saved
    pre_exit_file: saved

Full capture: `~/handoffs/L197/qemu_boot_serial.log`.

## 6. Limits

- No physical boot was done; every screen line above for the Spark is
  *expected*, from the code and the 2026-09-24 boot, not observed.
- The GB10 discovery path fixed in #56 has never run on the Spark.
- The firmware boot-menu key for this machine is not recorded anywhere we
  have; the checklist says to read it off the logo screen.
- Whether the firmware gives the stick the number `0004` again is the
  firmware's choice; the scripts no longer depend on it.
