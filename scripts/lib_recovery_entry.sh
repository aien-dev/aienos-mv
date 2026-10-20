# shellcheck shell=bash
# Find the AIENOSRECOV recovery stick in the firmware boot list.
#
# The firmware names and numbers the stick's entry itself when the stick is
# plugged in, and drops the entry when it is not: on 2026-09-24 it was
# `Boot0004* UEFI: USB USB Hard Drive, Partition 1 ... HD(1,GPT,f227357c-...)`
# (evidence/gate1_machine1_selftest_report_2026-09-24.txt line 39). The label
# never carries the volume name AIENOSRECOV, and the number is the firmware's
# choice, so the stick is found by its GPT partition GUID (PARTUUID of the
# AIENOSRECOV vfat partition, evidence/gate1_machine1_selftest_2026-09-24.md).
#
# Only the firmware's own entry counts. The firmware lists the stick at
# power-on, not when it is plugged into a running Ubuntu. On 2026-10-20 the
# stick went in after Ubuntu started; a hand-made `efibootmgr -C` entry
# (Boot0002, path HD(...)/File(...) with no USB node) satisfied the old check,
# the AIENOS entry was created as Boot0004, and at power-on the firmware gave
# Boot0004 to the stick, replacing the AIENOS entry BootNext named
# (evidence/spark_native_boot1_2026-10-20/fixer_readonly_capture_L318.txt).
# The firmware writes the full device path, so its entry has a USB(...) node
# before the partition; efibootmgr -C writes the short HD(...) form.
#
# Source it, then: recov_entry="$(recovery_entry "$(efibootmgr)")"
# efibootmgr 18 prints each entry's device path without -v.

AIENOS_RECOV_PARTUUID="${AIENOS_RECOV_PARTUUID:-f227357c-1e80-4fd0-a55e-2efbef1dd25c}"

# recovery_entry <efibootmgr output>: the 4-hex number of the first boot entry
# the firmware made for that GPT partition on a USB device; empty when the
# firmware does not list the stick (unplugged, or plugged in after power-on).
recovery_entry() {
    grep -iE "^Boot[0-9A-F]{4}\*? .*/USB\([^)]*\)/.*HD\([0-9]+,GPT,${AIENOS_RECOV_PARTUUID}," <<<"$1" \
        | head -n 1 | sed -E 's/^Boot([0-9A-Fa-f]{4}).*/\1/' || true
}

# recovery_stick_present: the stick's partition is visible to Linux right now.
# A firmware entry proves only that the stick was in at the last power-on.
recovery_stick_present() {
    [[ -e "${AIENOS_RECOV_DEV_DIR:-/dev/disk/by-partuuid}/${AIENOS_RECOV_PARTUUID}" ]]
}
