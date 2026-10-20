# Spark first native boot: the checklist for Drake

Ledger L197, 2026-10-20. Machine: the DGX Spark `spark-b87b` (GB10). Branch
`feat/spark-boot-package`. Everything on this page that is marked *checked*
was checked on 2026-10-20 without rebooting; nothing here has been run on the
machine yet. The boot itself is yours to do.

## What this boot is, in plain words

One restart of the Spark into AienOS (our own system, no Linux underneath),
for about a minute. It shows a report on the screen, counts down 30 seconds,
and restarts back into Ubuntu by itself. Ubuntu stays the normal default the
whole time; AienOS is chosen for this one restart only, and the firmware
forgets that choice as soon as it uses it.

The image is the **AienOS handoff image** (`aienos-handoff.efi`). It is the
only AienOS image that has already started on this Spark and come back
safely (2026-09-24, `evidence/m2_first_boot_2026-09-24.md`). New this time:
it looks for the GB10 chip and, if it finds it, reads two read-only identity
numbers from it. It loads no firmware into the GPU, runs nothing on it, and
writes nothing to the Spark's main disk. The only writes are three files in
the boot folder `\EFI\AIENOS` (the image, a note of what was staged, and the
report file `BOOTREPORT.TXT` the image saves itself) and one small report the
firmware keeps for Linux to read afterwards.

What it proves if it works: AienOS still starts on this firmware at the
current code, and it either finds the GB10 (the 2026-09-24 boot did not) or
tells us exactly why not.

## What to have at the machine

| Item | Why | State checked 2026-10-20 |
| --- | --- | --- |
| HDMI monitor on the Spark | The report is only on the screen | not checkable from software |
| Wired USB keyboard | Only needed if Ubuntu does not come back (firmware boot menu) | **not plugged in** (no keyboard on USB) |
| The AIENOSRECOV recovery stick (1 GB) | The way back if Ubuntu does not start; the staging command refuses without it | **not plugged in** (the firmware lists it only while plugged in) |
| Phone recording video | The GB10 lines are on screen for a moment only, before the screen is redrawn | |
| About 10 minutes | The 2026-09-24 boot took 75 s from Ubuntu stopping to Ubuntu starting again | receipt: `evidence/m2_first_boot_2026-09-24.md` |

Other facts checked read-only on 2026-10-20 (`~/handoffs/L197/machine-readonly.txt`):
Secure Boot is **off** (as decided 2026-10-01; leave it off and do not open
the firmware setup screen); Ubuntu is first in the boot order (`0001,0003`);
no one-time boot is pending; the boot partition has 291 MB free; the GB10
is at `000f:01:00.0` (`10de:2e12`). The boot folder already holds an older
handoff image from 2026-09-23 and the parked Allen image from 2026-10-18;
the staging command replaces the older handoff image and leaves Allen alone.

## The steps

### 1. Plug in the stick and the keyboard

With Ubuntu running, plug the AIENOSRECOV stick and a wired USB keyboard
into the Spark. Then, in a terminal on the Spark, type:

    efibootmgr | grep f227357c

*You should see* one line ending in `HD(1,GPT,f227357c-...)`: that is the
stick, under whatever number the firmware gave it. If you see nothing, the
firmware has not noticed the stick yet; unplug it, plug it in again, and
retry. (This changes nothing; it only reads.)

### 2. Stage the boot: the one command

In the same terminal (as yourself, **not** with `sudo` in front):

    cd ~/workspace/aienos-spark-boot-package && git pull && scripts/stage_one_time_boot.sh --apply

What it does: builds the image from this exact commit, runs the safety
check, saves a "before" snapshot of the boot settings, copies the image into
`\EFI\AIENOS`, adds a boot entry that is *not* in the normal boot order, and
tells the firmware "use it once, next restart". It asks for your password
once. It does **not** restart the machine.

*You should see*, in this order:

1. A pause while it builds, then `AArch64 PE32+ EFI application verified`.
2. The safety check, one line each. Lines starting `PASS` and `INFO` are
   fine. Lines starting `ASK` are questions for you (are you at the machine,
   is this the right boot); they are not errors. The check ends with
   `NATIVE_PREFLIGHT: READY (read-only checks only; the ASK lines are Drake's)`.
3. Eight summary lines (`commit:`, `image:`, `recovery:    Boot000N
   AIENOSRECOV stick listed`, `capture:`, `copy to:`, `boot entry:`,
   `BootOrder:`, `BootNext:`).
4. **The password prompt** (this is where it asks), then
   `Captured state to ...pre_boot_capture.json`.
5. The record of what was staged (`aienos_commit:`, `image_sha256:`, ...,
   `aienos_boot_entry:`), four lines from the firmware that include
   `BootNext: 000N`, then `Staged: the next boot runs Boot000N once.` and the
   three commands for after the boot. Take a photo of this part.

If any line starts with `STOP`, nothing was written. Send the screen text to
the chat and stop there. One STOP you might see is `quiet flag held`: another
job on the Spark is using the hardware; tell the chat and wait.

The chat records the attempt (ledger and evidence file) from your photo and
the step 5 output, so there is nothing else to log by hand.

### 3. Restart

Start the phone video, pointed at the monitor. Then type:

    sudo reboot

Keep your hands off the keyboard from here until Ubuntu is back.

### 4. What the screen must show, in order

1. Ubuntu shuts down; the NVIDIA logo appears (a few seconds).
2. White text on black: the AienOS pre-exit report. Look for these lines:
   - `aienos_commit: <the commit from step 2>`
   - **Either** the GB10 found:
     `gb10_pci: 000f:01:00.0`, `gb10_bar0_phys: 0x...`,
     `gb10_pmc_boot_0: 0x........`, `gb10_pmc_boot_42: 0x........`
   - **or** the GB10 not found, with the reason:
     `gb10: unavailable`, then `gb10_pci_root_bridges: N`, possibly
     `gb10_pci_root_open[i]: refused (...)`, lines
     `gb10_pci_root[i]: segment S devices D bridges B windows W`, and
     possibly `gb10_pci_candidate[i]: command_status ... bar0 ...`.
     This is also a useful result: these lines say why.
   - `exiting firmware boot services`
   - `report file: \EFI\AIENOS\BOOTREPORT.TXT saved` (or `not saved (...)`)
3. The screen is redrawn by AienOS itself (no firmware underneath any more):
   the AienOS report with `boot: native` and `kernel: alive`, then
   `uart_report: ...` and `nvram_report: saved (write 2 of 3)`.
4. `restarting in 30 s`, counting down to 1.
5. The machine restarts: NVIDIA logo, then Ubuntu starts as usual.

QEMU (the emulator on the desk) printed items 2 and 3 on its text console;
that capture is in `docs/SPARK_BOOT_PACKAGE_EVIDENCE.md` section 5. The
`uart_report`, `nvram_report` and `restarting in 30 s` lines go to the
screen only, so they come from the code (`crates/aienos-boot/src/handoff.rs`),
not from a capture.

### 5. After Ubuntu is back: three commands

In a terminal on the Spark, from `~/workspace/aienos-spark-boot-package`,
run the three commands step 2 printed at its end. They are, in order:

    scripts/collect_boot_report.sh
    sudo scripts/verify_native_rollback.sh --capture-post <evidence folder>/post_boot_capture.json
    scripts/verify_native_rollback.sh --verify --pre <evidence folder>/pre_boot_capture.json --post <evidence folder>/post_boot_capture.json

(`<evidence folder>` is the `evidence_dir:` line step 2 printed, under
`~/workspace/evidence-out/handoff/staging/`.)

*You should see*: the first ends with `M2_GATE: PASS` and shows the GB10
lines from the report the firmware kept; the third ends with
`VERDICT: PASS`. Both only read, except the second, which writes one file in
the evidence folder.

Keep the stick plugged in until these three are done: if the stick is
pulled, the firmware drops it from the boot order and the check reports a
changed boot order.

## If it hangs

- **The screen stops changing for 2 minutes** (well past the 30 s
  countdown): hold the power button until the Spark turns off, wait ten
  seconds, press it again. The firmware already used up the one-time choice,
  so Ubuntu starts. Then do step 5 anyway.
- **Ubuntu does not start after that**: power on and press the firmware's
  boot-menu key while the NVIDIA logo shows (which key is not recorded for
  this machine; the logo screen names it). Pick the USB stick entry
  (`UEFI: USB USB Hard Drive, Partition 1`). The stick starts a small
  recovery system that checks the disks read-only
  (`docs/RECOVERY_MEDIA_MACHINE1.md`). Then tell the chat; do not retry.
- **`Secure Boot Violation` on screen**: Secure Boot got switched on. The
  firmware falls back to Ubuntu by itself. Tell the chat; do not change the
  firmware setting.

After any restart, the TPM-locked storage may fail to open on the first try
and succeed on the second (a known Ubuntu-side quirk since 2026-09-24, not
caused by AienOS).

## Evidence to copy afterwards

1. The phone video (keep it off GitHub: phone files carry location data).
2. The output of the three step-5 commands (copy the terminal text).
3. The evidence folder `~/workspace/evidence-out/handoff/staging/<time>/`
   (`pre_boot_capture.json`, `post_boot_capture.json`, `STAGED.TXT`).
4. Say "booted" in the chat. The chat writes
   `evidence/spark_handoff_boot_2026-10-<day>.md` from these, with the GB10
   lines quoted, and undoes nothing else (the boot entry stays, outside the
   boot order, for the next boot).

## Not part of this boot

- The C kernel images (Aienux). Built and checked in QEMU on this branch
  (`docs/SPARK_BOOT_PACKAGE_EVIDENCE.md`), but the full and B7b images need
  owner key files that do not exist yet, and the inference image needs the
  model file locked in place first. A second boot with the inference image
  (`scripts/stage_one_time_boot_ck.sh --apply`) is possible in the same
  sitting only if this boot comes back clean.
- Loading GPU firmware or using the GPU. Not in this image at all.
