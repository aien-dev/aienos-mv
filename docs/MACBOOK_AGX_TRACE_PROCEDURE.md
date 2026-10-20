# MacBook AGX trace: the one attended reboot step

Ledger L214, step 4. Machine: MacBook Air Mac14,2, Apple M2 (T8112), GPU
G14G, macOS 26.6.2 (25G83).

**Status: prepared, NOT executed.** Nothing in this document has been run.
The agent that wrote it did not:

- change any boot policy;
- create, resize or delete any volume or partition;
- reboot.

Every step below needs a person at the Mac. Each step says what it changes
and how to undo it.

Related documents:

- `docs/AGX_FIRMWARE_INTERFACE.md`: what is already known, and the UNKNOWN
  rows U1 to U8 this trace resolves.
- `docs/AGX_PRIOR_ART.md`: licences.
- `docs/AGX_ISA_NOTES.md`: the shader ISA.

Claim tags:

- **OBSERVED**: measured on this Mac, or produced by a command whose output is
  quoted.
- **INFERRED**: taken from a named source.
- **UNKNOWN**: not settled. The row names the experiment that settles it.

## 1. What the trace does, in one paragraph

A second macOS, version 13.5, is installed on a new volume next to the
current one. The current macOS, its data and its security settings are left
alone.

Only that second install is set to Permissive Security, and only its boot
object is replaced, by m1n1 (the Asahi bootloader and hypervisor, built here
from source). m1n1 then runs that macOS 13.5 as a guest, under the control of
a second computer connected by USB-C. While Apple's own GPU driver runs our
vector add through Metal, m1n1 records:

- every GPU register access;
- the firmware mailbox messages;
- the shared-memory structures (InitData, work queues, the compute command);
- the GPU interrupts.

The record lands on the second computer. Afterwards the Mac boots back into
its normal macOS.

## 2. Why macOS 13.5, not the macOS 26.6.2 this Mac runs

| Fact | Status, source |
|---|---|
| The running GPU firmware (macOS 26.6.2) accepts only InitData whose first 8 bytes are `0x0c89c32b039004c8`, and copies 0xc8 bytes. drm/asahi and m1n1 describe no such interface. | OBSERVED, step 3 disassembly (`AGX_FIRMWARE_INTERFACE.md` section 2.1) |
| The macOS 13.5 GPU firmware checks `0x00b006011f286ba0`, which is exactly drm/asahi's G14 V13_5 `ver_info`, with the flag and pointer offsets drm/asahi uses. | OBSERVED, same section |
| m1n1's GPU structure decoders have no layout newer than V13_5. Its firmware version map reads the guest's iBoot string; `iBoot-8422.141.2` (macOS 13.5) maps to `V13_5`. No macOS 26 iBoot is in the map. | INFERRED, m1n1 7cde7d1a `proxyclient/m1n1/proxyutils.py` lines 24 to 40, `constructutils.py` line 804 |
| The Asahi hypervisor guide lists macOS 13.5 and 14.8.3 as the supported guests. | INFERRED, asahi-docs `docs/sw/m1n1-hypervisor.md` |
| macOS 13.5 ships kernelcache `kernelcache.release.mac14g` for this model (`j413ap`). It contains the drivers `com.apple.AGXG14G`, `com.apple.AGXFirmwareKextG14GRTBuddy` and `com.apple.iokit.IOGPUFamily`. | OBSERVED: fetched from Apple's 13.5 (22G74) restore image by HTTP range; `otool -l` fileset entries |

So the trace runs on macOS 13.5. The result is the 13.5 interface, which is
the one AienOS targets (the same firmware an Asahi-style 13.5 stub loads).

## 3. What changes on this Mac, and how each change is undone

| # | Change | Where | Size | Undo | Status |
|---|---|---|---|---|---|
| C1 | New APFS volume `macOS135`, which the installer turns into a volume group (System + Data) | Inside the existing container `disk3`. No partition is created or resized. | macOS 13.5 needs about 26 GB (Apple's stated requirement for installing Ventura), shared from the container's free space. | `diskutil apfs deleteVolumeGroup <VG135>` from the main macOS (section 9) | INFERRED size; container OBSERVED below |
| C2 | macOS 13.5 boot files (its own iBoot stage 2, firmware, kernelcache) | `Preboot` volume, in a folder named after `<VG135>` | under 1 GB, INFERRED | Removed with C1. Check: no `<VG135>` folder remains under `/System/Volumes/Preboot` (UNKNOWN until checked, section 9). | INFERRED |
| C3 | Local boot policy of `<VG135>` set to Permissive Security, with m1n1 enrolled as its custom boot object. The policy for the main macOS (Full Security) is not touched. | SEP-signed LocalPolicy, one per OS install | none | `bputil -f -v <VG135>` (back to Full Security, drops m1n1), or `bputil -r <VG135>` after deleting the volume group | INFERRED: Apple keeps a LocalPolicy per OS install; `bputil -h` OBSERVED (section 10) |
| C4 | Default startup disk set to macOS 13.5 for the trace | NVRAM | none | Startup Disk set back to `Macintosh HD` (section 8) | INFERRED |
| C5 | Installer files: `InstallAssistant.pkg` (11.93 GB), then `Install macOS Ventura.app` (about 12 GB) | A USB drive, recommended; on the internal disk only if there is room | 12 to 24 GB | Delete both files | OBSERVED size (archive.org metadata) |
| C6 | Workload and m1n1 files copied into the 13.5 install | `/Users/Shared/agx` on the 13.5 Data volume | under 1 MB | Removed with C1 | not applicable |

Not changed:

- the main macOS volumes, their FileVault, their SIP and their LocalPolicy;
- NVRAM `boot-args`: not set, because m1n1 passes boot arguments to the guest
  itself;
- the partition map;
- system firmware.

SIP on the 13.5 install is left as `bputil` leaves it. It is not disabled
unless row U-T3 requires it.

### Baseline OBSERVED on 2026-10-20 (before anything)

| Item | Value |
|---|---|
| `csrutil status` | `System Integrity Protection status: enabled.` |
| APFS | Container `disk3`, 494.4 GB ceiling, **34.0 GB not allocated** (93.1 % used). One volume group: `Macintosh HD` + `Macintosh HD - Data`, FileVault on. |
| Other containers | `disk1` (4 volumes, no volume group), `disk2` (2 volumes, no volume group): the system ISC and recovery containers |
| `bputil -e` | Requires root. Not run by the agent; the person runs it in step P2. |

**Disk space is the first blocker.** 34.0 GB unallocated is OBSERVED. The 13.5
install needs about 26 GB (INFERRED, Apple). The installer needs another 12
to 24 GB on top of that, unless it sits on a USB drive. Requirement before
starting: at least 40 GB unallocated with the installer on a USB drive, or 64
GB with it on the internal disk.

## 4. What is needed

| Item | Detail | Status |
|---|---|---|
| A second computer (the host) | Any Mac, or any Linux machine, with Python 3.9 or later. It runs m1n1's proxyclient and receives the trace. The office iMac or the Spark both qualify; it must be physically next to the MacBook. | INFERRED, asahi-docs `tethered-boot-macos-host.md` |
| USB-C data cable | USB-C to USB-C, or USB-A to USB-C. It goes into the MacBook's DFU port: the leftmost USB-C port when facing the left side of the Mac, the rear-left port. | INFERRED, Apple support 120694 ("All other models: The leftmost USB-C port when you're facing the left side of the Mac") and asahi-docs `hw/soc/serial-debug.md` ("On MacBooks, this is the rear left port") |
| USB drive of 32 GB or more | Holds the installer (C5) | recommended, see section 3 |
| Machine Owner password | The MacBook's admin password, typed by its owner in Recovery | required by Apple: changes to boot policy happen only in One True Recovery with owner authentication (INFERRED, asahi-docs `platform/security.md`) |
| Charger | MagSafe stays free for power, because the DFU port carries the cable | INFERRED |

### Files on the host

| File | sha256 | How it was made |
|---|---|---|
| m1n1 source at `7cde7d1a784d851ebfee5579fa363c38b14150c0` | (git commit) | `git clone https://github.com/AsahiLinux/m1n1` and check out that commit. The proxyclient must be the same commit as the enrolled m1n1, because m1n1's host interface is not stable (INFERRED, asahi-docs `m1n1-hypervisor.md`, "m1n1 ABI synchronisation"). |
| `m1n1.bin` | `7666ce4ddd5473f48ccce571cfed24c1e24526d63c07036441171ca21a59c98b` | Built on this Mac at that commit, twice, with identical hashes: `gmake -j4 USE_CLANG=0 ARCH=aarch64-elf- TOOLCHAIN=/opt/homebrew/bin/`, with Homebrew `aarch64-elf-gcc` 16.2.0 and Rust 1.97.1 (`aarch64-unknown-none-softfloat`) in a scratch toolchain. 0 warnings. `BUILD_TAG` `7cde7d1`. OBSERVED. |
| `m1n1.macho` | `1557cb28e57f04f041d36ba4a8bb05573b97c58d523e5f0f84c42c0cee882b99` | Same build; used only to chainload |
| `kernelcache-13.5-22G74-mac14g.macho` | `36de6685089e79a695a319c0789ecffd6bd8c2a5c6002a40a8acef7bb8e6ab8d` (96,550,912 bytes) | IM4P `kernelcache.release.mac14g` (sha256 `538b1dc0d038e371aed6b88c3da0e2873b5f13de856e5e49fac7815c08476d43`) fetched from Apple's `UniversalMac_13.5_22G74_Restore.ipsw` by HTTP range. Its payload is LZFSE (`bvx2`), and the IM4P states 0x05c14000 bytes uncompressed. Decompressed with macOS `compression_tool -decode -a lzfse`. OBSERVED. |
| `agx_trace_plan.txt` | `f1fef538379c2d7f7c804e4d767b4ccf00b226114d7313b056eea362bf5b53db` | This repository, `native/gpu/agx/trace/` |
| `aienos_agx_trace.py` | `5d02239a436cda583ec518881e401e1afd6dee4057e59d877e55e9493cc0be85` | m1n1 hypervisor module, written for AienOS. It is kept outside this repository, which carries no Python: `~/handoffs-mac/l214-artifacts/agx-trace/` on the MacBook, mirrored at `spark:handoffs/l214-artifacts/agx-trace/`. It is licensed GPL-2.0-or-later because it imports m1n1. It copies no m1n1 code; m1n1's own `AGXTracer` does the GPU decoding. |

Host Python packages are those m1n1's proxyclient imports: `pyserial`,
`construct`, `asn1`. Install them in a virtual environment on the host, not
system-wide.

Checks run without hardware (OBSERVED, on the MacBook):

- `python3 -I aienos_agx_trace.py --check agx_trace_plan.txt` resolves the
  plan against the macOS 26 device-tree values. Result: 5 ranges, 5
  interrupts.
- An import check against m1n1 7cde7d1a confirms the names the module uses:
  - `HV.trace_range(zone, mode, read, write, name)`;
  - `HV.trace_irq(device, num, count, flags)`;
  - `HV.IRQTRACE_IRQ` = 1, `TraceMode.SYNC` = 5;
  - the `AGXTracer` options `trace_kernmap`, `trace_usermap`, `trace_kernva`,
    `trace_userva`, `redump` and `cmd_dump_dir`.

### The workload

`native/gpu/agx/tools/agx_metal_run.m` runs one `agx_cases.h` case on the
GPU through Metal. It compiles the kernel at runtime from
`tests/fixtures/corpus/<case>.metal`, then compares the output with the C
reference.

- It is built with deployment target 13.0, so the same binary runs in the
  13.5 guest.
- On this Mac under macOS 26.6.2, `make gpu-ref` gives MATCH on all six cases
  (OBSERVED):

  | Case | Hash |
  |---|---|
  | `vadd` | `756c2c450553232e` |
  | `vadd_i32` | `86155d9e444cd3f4` |
  | `vadd_bounds` | `2f7e701fd70bfe49` |
  | `saxpy` | `0a638a1562db4a85` |
  | `matmul_naive` | `cf1cc1653f9d0535` |
  | `matmul_tiled` | `cf1cc1653f9d0535` |

## 5. Preparation (normal macOS, no reboot)

P1. Back up the Mac with Time Machine. Nothing below touches the main macOS
volumes, but a backup turns any mistake into an inconvenience.

P2. Record the baseline:

```sh
mkdir -p ~/agx-trace-baseline && cd ~/agx-trace-baseline
csrutil status > csrutil.txt
sudo bputil -e > bputil-all.txt
diskutil apfs listVolumeGroups > vgroups.txt
diskutil apfs list > apfs.txt
```

P3. Free disk space until `diskutil apfs list` shows at least 40 GB
"Capacity Not Allocated" for `disk3` (64 GB if the installer stays on the
internal disk). Stop here if that is not possible.

P4. Get the macOS 13.5 installer. Apple's software catalog no longer offers
it (OBSERVED 2026-10-20: the oldest Ventura installer in Apple's catalog is
13.7.8, 22H730).

1. Download `InstallAssistant.pkg` from the archive.org item
   `install-assistant_20240930` (linked from asahi-docs `m1n1-hypervisor.md`)
   onto the USB drive.
2. Check it against the archive.org metadata (OBSERVED 2026-10-20): 11,934,300,398
   bytes, md5 `d53a82646e70f3e131c56a3972420e9e`, sha1
   `9ceeac3a75aaf3d6e477990caaf2c695f31624e2`.

   ```sh
   md5 InstallAssistant.pkg; shasum -a 1 InstallAssistant.pkg
   pkgutil --check-signature InstallAssistant.pkg
   ```

   The signature check must report `Status: signed Apple Software` with a
   chain ending in `Apple Root CA`.
3. Even a modified installer could not produce a 13.5 system that boots: the
   SEP refuses any system volume whose seal is not signed by Apple (INFERRED,
   asahi-docs `platform/security.md`, "System data integrity").
4. Extract the app onto the USB drive:

   ```sh
   sudo installer -pkg /Volumes/<usb>/InstallAssistant.pkg -target /Volumes/<usb>
   ```

   This writes `/Volumes/<usb>/Applications/Install macOS Ventura.app`
   (INFERRED, standard `installer` behaviour).

P5. Create the volume. This is change C1. It needs no reboot and no
partitioning.

```sh
diskutil apfs addVolume disk3 APFS macOS135
```

Do not pass `-role`. The asahi guide warns that a system role on this volume
"will mess with your existing system" (INFERRED, `m1n1-hypervisor.md`).

## 6. Install macOS 13.5 (reboots into the new install only)

I1. Open `Install macOS Ventura.app` and choose `macOS135` as the destination.
The installer restarts the Mac into the new install. This is the first
reboot.

I2. Setup Assistant:

- create a local administrator account (name it e.g. `agx`);
- skip Apple ID;
- do **not** turn on FileVault, so the main macOS can copy files into this
  install;
- decline analytics and Location Services.

I3. In 13.5, run `sw_vers` and check for `13.5` and `22G74`. If the build
differs, stop: the firmware and kernelcache hashes above are for 22G74.

I4. Copy the workload and m1n1 into the 13.5 install (change C6). From the
host, or from a USB stick, put these in `/Users/Shared/agx/`:

- `agx_metal_run`;
- the `corpus/*.metal` files;
- `m1n1.bin`.

Then check:

```sh
shasum -a 256 /Users/Shared/agx/m1n1.bin   # must equal 7666ce4d...c98b
```

I5. Run the workload natively once, as a 13.5 baseline:

```sh
for c in vadd vadd_i32 vadd_bounds saxpy matmul_naive matmul_tiled; do
  /Users/Shared/agx/agx_metal_run /Users/Shared/agx/corpus $c; done
```

Each line must say `MATCH`, with the same hashes as in section 4.

I6. Make 13.5 the default startup disk: System Settings, then General, then
Startup Disk, then `macOS135` (change C4). Shut down.

## 7. Permit m1n1 on the 13.5 install only (One True Recovery)

R1. With the Mac off, press and hold the power button until "Loading startup
options" appears. Choose Options, then Continue, and authenticate as the
Mac's owner. Open Utilities, then Terminal.

R2. Find the 13.5 volume group UUID:

```sh
diskutil apfs listVolumeGroups
```

Write down the UUID of the group that contains `macOS135`. Call it
`<VG135>`. Check it twice: it must **not** be the group containing
`Macintosh HD`.

R3. Lower security for that group only (change C3):

```sh
bputil -nkcas -v <VG135>
```

The flags come from `bputil -h`, OBSERVED on this Mac:

| Flag | Meaning |
|---|---|
| `-n` | Permissive Security |
| `-k` | locally signed kexts |
| `-c` | disable kernel CTRR |
| `-a` | allow custom boot args |
| `-s` | disable SSV |

This is the flag set from asahi-docs `m1n1-hypervisor.md`. Enter the owner's
password when asked.

R4. Mount the 13.5 data volume and enrol m1n1 as its boot object:

```sh
diskutil mount "macOS135 - Data"
kmutil configure-boot -c "/Volumes/macOS135 - Data/Users/Shared/agx/m1n1.bin" \
  --raw --entry-point 2048 --lowest-virtual-address 0 -v /Volumes/macOS135
```

The options are from `kmutil configure-boot -h` (OBSERVED on this Mac) and
asahi-docs `m1n1-hypervisor.md`. Enter the owner's password again when asked.

R5. Do not run `csrutil disable` on the first attempt (see U-T3). Do not set
`nvram boot-args`.

R6. Run `bputil -d -v <VG135>` and check that it shows Permissive Security
and a custom boot object. Then shut down.

## 8. Trace (host computer drives it)

T1. Connect the USB-C cable from the host to the MacBook's DFU port
(section 4). Power on the MacBook. It starts the 13.5 install, which is now
m1n1. The m1n1 stage waits for the host because it has no payload (INFERRED,
m1n1 user guide).

T2. On the host, find the serial device:

- macOS host: `/dev/cu.usbmodemP_01` is typical; list with `ls /dev/cu.usbmodem*`.
- Linux host: `/dev/ttyACM0`.

If no device appears within a minute, try the other left-side port and
power-cycle the MacBook.

T3. On the host, in the m1n1 checkout at `7cde7d1a`:

```sh
mkdir -p ~/agx-trace/$(date +%Y%m%d-%H%M) && OUT=$_
export M1N1DEVICE=/dev/cu.usbmodemP_01        # or /dev/ttyACM0
export AIENOS_AGX_TRACE_PLAN=<aienos>/native/gpu/agx/trace/agx_trace_plan.txt
export AIENOS_AGX_TRACE_OUT=$OUT
python3 proxyclient/tools/run_guest.py -m <artifacts>/aienos_agx_trace.py \
  <path>/kernelcache-13.5-22G74-mac14g.macho \
  -- "debug=0x14e serial=3 apcie=0xfffffffe -enable-kprintf-spam wdt=-1 clpc=0"
```

The boot arguments are those of asahi-docs `m1n1-hypervisor.md` (INFERRED).
The module checks that the guest is G14 with firmware V13_5. If it is not, it
stops before tracing and leaves you in the m1n1 shell, and the guest does not
start (INFERRED, `run_guest.py` lines 148 to 153).

T4. The 13.5 guest boots on the MacBook's screen, slowly, because every GPU
register access stops the guest. Log in and open Terminal:

```sh
/Users/Shared/agx/agx_metal_run --repeat 5 --sleep-ms 2000 /Users/Shared/agx/corpus vadd
/Users/Shared/agx/agx_metal_run /Users/Shared/agx/corpus matmul_tiled
```

Each run must print `MATCH`. Write down the wall-clock time of each command.
The trace log is time-stamped, so this pairs a submission with its log lines.

T5. Shut the guest down from the Apple menu. When the host shows the m1n1
shell, press Ctrl-D, then hold the MacBook's power button for 10 seconds.

T6. Copy the trace off the host and record its hash:

```sh
cd $OUT/.. && tar czf $(basename $OUT).tgz $(basename $OUT)
shasum -a 256 $(basename $OUT).tgz
scp $(basename $OUT).tgz spark:handoffs/l214-trace/
```

The directory contains:

- `hv.log`: register, mailbox and interrupt trace;
- `cmds/`: one file per GPU command, decoded by m1n1;
- `manifest.txt`: the resolved 13.5 addresses and the guest firmware version.

## 9. Boot back into the normal macOS

B1. Press and hold the power button until the startup options appear. Select
`Macintosh HD`, hold the Option key, and click "Always Use". This reverts
change C4. The main macOS starts as before.

B2. Verify nothing changed on the main install:

```sh
csrutil status        # must still say: enabled
sudo bputil -e > ~/agx-trace-baseline/bputil-all-after.txt
diff <(grep -v '^$' ~/agx-trace-baseline/bputil-all.txt) <(grep -v '^$' ~/agx-trace-baseline/bputil-all-after.txt)
```

The diff may only show the added `<VG135>` policy. The `Macintosh HD`
entries must be identical. If they are not, go to 1TR and run `bputil -f`
for the main volume group.

## 10. Undo

Choose one level.

**Level 1: keep 13.5, remove m1n1.** In 1TR (as R1):

```sh
bputil -f -v <VG135>
```

Full Security re-creates the policy without the custom boot object, so 13.5
boots its own kernel again (INFERRED from `bputil -h`, OBSERVED: "-f Changes
security mode to Full Security").

**Level 2: remove everything.** Starting from the main macOS:

1. Delete the volume group:

   ```sh
   diskutil apfs listVolumeGroups      # find <VG135> again
   diskutil apfs deleteVolumeGroup <VG135>
   ```

   `diskutil apfs` lists `deleteVolumeGroup` ("Remove grouped APFS Volumes
   from its APFS Container"), OBSERVED.
2. Remove the boot policy that is left over. In 1TR, run either of:

   ```sh
   bputil -r <VG135>
   bputil -z
   ```

   `-r` removes the policies of one group. `-z` removes them for every group
   no longer present, and it also removes policies for disconnected external
   boot disks (OBSERVED, `bputil -h`).
3. Back in the main macOS, check:
   - `ls /System/Volumes/Preboot` shows no `<VG135>` folder (U-T4);
   - `diskutil apfs list` shows the container's unallocated space back near
     the P3 value.
4. Delete the installer files (C5).

If the Mac boots nothing:

- Hold the power button and choose `Macintosh HD` in startup options.
- If even startup options fail, Apple's DFU "Revive" from another Mac
  restores the recovery and firmware without erasing data (INFERRED,
  asahi-docs `sw/sonoma-display-bug.md`, Apple support 108900).

## 11. What the trace answers

| Row | Question | What in the trace answers it |
|---|---|---|
| U2 | Is the 13.5 interface right beyond the init handshake on this chip? | `cmds/` decodes InitData, RunCompute and the microsequence with m1n1's V13_5 layouts. Any decode failure or nonsense field is a mismatch at a known offset. |
| U3 | Values macOS puts in the `unk_*` compute fields | The `cmds/` dumps of the vadd and matmul_tiled submissions |
| U4 | Which uniform holds which buffer for a Metal dispatch | The USC word list and uniform upload in the RunCompute dump. For vadd, `du0`, `du2` and `du4` must hold the three buffer addresses (`AGX_ISA_NOTES.md`). |
| U6 | Does macOS read `ttbat-phys-addr-base`? | `hv.log` accesses during AGX kext initialisation |
| U7 | Which GPU interrupt line fires for a compute completion | `hv.log` IRQ events for `/arm-io/sgx` lines, matched to the T4 times |
| sgx_win1 | Who touches GPU MMIO window 1 (0x204d00000 on 26) | `hv.log` entries named `sgx_win1` |

U1, U5 and U8 concern the macOS 26 firmware. This trace cannot answer them.

## 12. Open questions about this procedure itself

| Row | Question | Status | Experiment that settles it |
|---|---|---|---|
| U-T1 | Does the archive.org `InstallAssistant.pkg` install build 22G74 (the build whose kernelcache is prepared)? | UNKNOWN. The metadata says "macOS Ventura 13.5"; the build is not stated. | `sw_vers` in step I3. If the build differs, fetch that build's `kernelcache.release.mac14g` the same way (section 4) before T3. |
| U-T2 | Is the kernelcache in the 13.5 install's Preboot byte-identical to the IPSW payload prepared here? | INFERRED (same build, same IM4P), not checked | Optional: in 13.5, extract the IM4P payload of `/System/Volumes/Preboot/<VG135>/boot/*/System/Library/Caches/com.apple.kernelcaches/kernelcache` and compare its sha256 with `538b1dc0...` minus the IMG4 wrapper. |
| U-T3 | Does the 13.5 guest need SIP disabled to boot under m1n1? | UNKNOWN. The asahi guide disables it but gives no reason tied to the hypervisor. | First attempt with SIP as `bputil` leaves it. Only if the guest panics citing SIP: return to 1TR and run `csrutil disable`, which asks for the target install; choose `macOS135`. Then repeat B2. |
| U-T4 | Does `deleteVolumeGroup` also remove the group's Preboot folder? | UNKNOWN | `ls /System/Volumes/Preboot` after step 10 level 2 |
| U-T5 | Is the guest's display usable under the hypervisor on this model? | UNKNOWN. asahi-docs report macOS guests with display on other models. | Step T4. If the screen stays dark, enable Remote Login in step I2 and run T4 over `ssh` from the host instead. |
| U-T6 | Does the 13.5 compiler emit the same shader bytes as 26.6.2's for the corpus kernels? | UNKNOWN; not needed for the trace | Compare the USC shader bytes in `cmds/` with `tests/fixtures/corpus/vadd.bin` |

## 13. Alternative not chosen: an Asahi-installer stub partition

The Asahi installer can create a small separate APFS container that holds a
macOS 13.5 stub and m1n1, without a full macOS (INFERRED, asahi-installer
`src/main.py`, which uses the 13.5 IPSW).

That route shrinks the main container, which is partitioning. It runs Linux
(drm/asahi) or AienOS as the guest, not Apple's driver. drm/asahi's GPU
interaction is already public source, so tracing it teaches nothing new.

The stub route is the right one later, for running AienOS's own GPU path on
bare metal with the 13.5 firmware. It is not the route for observing Apple's
driver.
