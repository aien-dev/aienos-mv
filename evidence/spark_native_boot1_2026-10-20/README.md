# Spark native boot 1, 2026-10-20 (attended): what happened

Verdict: **the staged AienOS image (commit 26e929f3) left no trace of running.**
The firmware gave the AienOS boot entry's number to the recovery stick at
power-on, so the one-time boot most likely started the stick instead. Ubuntu,
its disks, the boot order and Secure Boot came back unchanged
(`rollback_verify.txt`: PASS).

The commit that added this folder says "kernel alive at EL2". That is wrong:
the native report `collect_boot_report.sh` printed is the one left over from
the 2026-09-24 boot (see below), not a report from this boot.

## Files

| File | What it is |
|---|---|
| `STAGED.TXT` | what the stager recorded at 22:55:59Z (commit, image digest, Boot0004 as the AienOS entry, BootNext) |
| `pre_boot_capture.json`, `post_boot_capture.json`, `rollback_verify.txt` | boot settings before and after; rollback check PASS |
| `collect_boot_report.txt` | the M2 gate as first run (old checker): FAIL x3 |
| `fixer_readonly_capture_L318.txt` | read-only capture by the L318 fixer: full boot list, ESP folder, the staging commands from the system log, the stick's boot menu |

## The evidence, claim by claim

1. **The native report is stale.** [OBSERVED] Its lines are identical to the
   2026-09-24 report in `evidence/m2_first_boot_2026-09-24.md` (lines 73-98),
   including `aienos_commit: 52109bcb...`; the staged commit is `26e929f3...`.
   The report variable is non-volatile, so it survives until replaced.
2. **The pre-exit file was not written.** [OBSERVED] `\EFI\AIENOS\BOOTREPORT.TXT`
   is 0 bytes with a 2026-10-18 04:44 local timestamp, older than the boot.
3. **The stick entry was hand-made.** [OBSERVED] System log, 22:55:51Z:
   `efibootmgr -C -d /dev/sda -p 1 -L AIENOSRECOV -l \EFI\BOOT\BOOTAA64.EFI`
   (now `Boot0002`, short `HD(...)/File(...)` path). The stick had been plugged
   into the running Ubuntu, so the firmware had not listed it yet; the stager's
   check accepted this entry.
4. **The AienOS entry was Boot0004 and the firmware replaced it.** [OBSERVED]
   22:55:59Z the stager created `AIENOS handoff (one-time)` and recorded it as
   `0004`; 22:56:00Z `efibootmgr --bootnext 0004`. After the restart, `Boot0004`
   is `UEFI: USB USB Hard Drive, Partition 1` with a firmware-style
   `VenHw(...)/USB(1,0)/USB(0,0)/HD(...f227357c...)` path, it was appended to
   BootOrder, and no `AIENOS handoff (one-time)` entry exists. The UEFI 2.10
   specification (section 3.1.2) says the firmware "should not, under normal
   operation, automatically remove any correctly formed Boot#### variable
   currently referenced by the BootOrder or BootNext variables"; this firmware
   (American Megatrends, BIOS 5.36_0ACUM018) did.
5. **What started instead.** [INFERRED] BootNext pointed at Boot0004, which by
   then loaded the stick's `\EFI\BOOT\BOOTAA64.EFI`: a menu offering the
   "AIENOS Standalone Recovery System" and "Reboot to Incumbent Linux". What
   the screen showed is UNKNOWN (no photo or note in the evidence). Whether the
   firmware remembered 0004 for the stick from September, or picked it fresh,
   is not observable from Linux.
6. **Why September worked.** [OBSERVED] On 2026-09-24 the stick was already
   listed by the firmware (`Boot0004`) before staging, so the AienOS entry got
   `Boot0000` and nothing new appeared at power-on.

## What the three FAILs were

- `left_firmware_into_aienos`, `native_code_after_firmware_exit`: correct
  verdicts with misleading wording ("native report: present"). Both refused
  the stale report because its commit is not the staged one.
- `boot_order_preserved`: wrong check. It judged the AienOS entry by number,
  and the number had become the stick's. The stick being appended by the
  firmware is allowed.
- Not flagged before, also wrong: `kernel_alive` PASSed on the stale report's
  `kernel: alive` line.

## Fixes (branch fix/boot1-gate, L318)

- `scripts/collect_boot_report.sh`: a report counts only if it names the staged
  commit, and the verdict says which boot it came from; `kernel_alive` needs
  such a report; entries are judged by what they load, not by number; new check
  `aienos_entry_kept` (the AienOS entry still loads the AienOS image; on
  2026-09-24 it did).
- `scripts/lib_recovery_entry.sh` (used by every stager and the preflight): the
  stick counts only when the firmware itself lists it (a `USB(` node in the
  path) and its partition is plugged in now.
- `scripts/stage_one_time_boot.sh`: removes the previous BOOTREPORT.TXT and
  report variable before setting BootNext (copies kept in the evidence folder),
  and creates the AienOS entry as `Boot0A1E`, far above the numbers the firmware
  uses (0000-0004 here).
- `scripts/collect_boot_report_test.sh` replays this boot and the September boot
  through the checker. On the old checker it fails (stale `kernel_alive` PASS,
  AienOS loader in BootOrder missed); on the new one this boot reads:

```text
FAIL  left_firmware_into_aienos: pre-exit file: missing or empty, native report: left from an earlier boot (commit 52109bcb334a95bdd33410b4d22c59dee8c917f4, not the staged 26e929f3b6b4)
FAIL  native_code_after_firmware_exit: firmware variable written by the staged image after ExitBootServices: no, left from an earlier boot (...)
FAIL  kernel_alive: last stage unknown: no report from the staged image
PASS  boot_order_preserved: before 0001,0003, now 0001,0003,0004 (added: 0004 = UEFI: USB USB Hard Drive, Partition 1)
FAIL  aienos_entry_kept: Boot0004 now holds 'UEFI: USB USB Hard Drive, Partition 1', not \EFI\AIENOS\aienos-handoff.efi: the firmware replaced it before the boot, so BootNext most likely started that instead
M2_GATE: FAIL
```

Left on the machine, untouched (the next attended boot is Drake's): the
hand-made `Boot0002 AIENOSRECOV` entry (not in BootOrder, harmless), the stale
report variable and the empty BOOTREPORT.TXT (the next staging run clears both).
