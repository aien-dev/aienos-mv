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
# Source it, then: recov_entry="$(recovery_entry "$(efibootmgr)")"
# efibootmgr 18 prints each entry's device path without -v.

AIENOS_RECOV_PARTUUID="${AIENOS_RECOV_PARTUUID:-f227357c-1e80-4fd0-a55e-2efbef1dd25c}"

# recovery_entry <efibootmgr output>: the 4-hex number of the first boot entry
# whose device path is that GPT partition; empty when the stick is not listed.
recovery_entry() {
    grep -iE "^Boot[0-9A-F]{4}\*? .*HD\([0-9]+,GPT,${AIENOS_RECOV_PARTUUID}," <<<"$1" \
        | head -n 1 | sed -E 's/^Boot([0-9A-Fa-f]{4}).*/\1/' || true
}
