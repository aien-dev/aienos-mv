#!/usr/bin/env bash
# Collect and judge the evidence from an AIENOS first-boot attempt. Read-only.
# Run under `aien-proof hold --resource machine-1` so the full output becomes
# an audit event in the hardware-test ledger.
#
# Gate (M2 first native boot): Machine 1 left firmware, entered native AIENOS
# code with no Linux underneath, left independently recoverable evidence, and
# returned to the existing system without damaging it.
set -uo pipefail

esp="${AIENOS_ESP_DIR:-/boot/efi/EFI/AIENOS}"
staged_file="${esp}/STAGED.TXT"
report_file="${esp}/BOOTREPORT.TXT"
image="${esp}/aienos-handoff.efi"
loader='\EFI\AIENOS\aienos-handoff.efi'
report_var="${AIENOS_REPORT_VAR:-/sys/firmware/efi/efivars/AienosBootReportV1-a1e05b0e-7c3d-4f51-9b6a-2d8e4c1f0a37}"

field() { sed -n "s/^$1: //p" <<<"$2" | head -1; }
check() { # name, pass(0/1), detail
    if [[ "$2" == 1 ]]; then echo "PASS  $1: $3"; else echo "FAIL  $1: $3"; failed=1; fi
}
failed=0

echo "== staging record (${staged_file})"
staged="$(cat "${staged_file}" 2>/dev/null)"
if [[ -z "${staged}" ]]; then
    echo "missing: nothing was staged, or the file is unreadable"
    echo "M2_GATE: UNPROVEN"
    exit 1
fi
printf '%s\n' "${staged}"
commit="$(field aienos_commit "${staged}")"

echo "== staged image"
digest_now="$(sha256sum "${image}" 2>/dev/null | cut -d' ' -f1)"
echo "sha256: ${digest_now:-missing}"

echo "== pre-exit report (${report_file})"
pre="$(cat "${report_file}" 2>/dev/null)"
printf '%s\n' "${pre:-missing}"

echo "== native firmware-variable report"
native=""
if [[ -r "${report_var}" ]]; then
    # efivarfs prefixes the value with 4 bytes of attributes.
    # efivarfs files cannot seek, so stream the value instead of `tail` on the path.
    native="$(cat "${report_var}" 2>/dev/null | tail -c +5)"
fi
printf '%s\n' "${native:-missing}"

echo "== current boot state"
boot_state="$(efibootmgr)"
printf '%s\n' "${boot_state}" | sed -n '1,4p'
echo "kernel: $(uname -r)"
echo "root: $(findmnt -no OPTIONS / | cut -d, -f1)"

echo "== verdict"
check "image_unchanged" "$([[ -n "${digest_now}" && "${digest_now}" == "$(field image_sha256 "${staged}")" ]] && echo 1 || echo 0)" \
    "staged ${commit:0:12}, image ${digest_now:0:16}"
# A report counts only when it names the staged commit. The report variable is
# non-volatile: on 2026-10-20 it still held the 2026-09-24 report (commit
# 52109bcb) after a boot in which the staged image never ran, and an empty
# BOOTREPORT.TXT is no report either.
origin() { # report text -> where it came from
    local c; c="$(field aienos_commit "$1")"
    if [[ -z "$1" ]]; then echo "missing or empty"
    elif [[ "${c}" == "${commit}" ]]; then echo "from the staged image"
    else echo "left from an earlier boot (commit ${c:-none}${c:+, not the staged ${commit:0:12}})"; fi
}
pre_fresh=0; [[ -n "${pre}" && "$(field aienos_commit "${pre}")" == "${commit}" ]] && pre_fresh=1
native_fresh=0; [[ -n "${native}" && "$(field aienos_commit "${native}")" == "${commit}" ]] && native_fresh=1
# Either report proves the firmware ran this image; the variable is written only
# after ExitBootServices, so it alone also proves firmware exit.
check "left_firmware_into_aienos" "$([[ "${pre_fresh}" == 1 || "${native_fresh}" == 1 ]] && echo 1 || echo 0)" \
    "pre-exit file: $(origin "${pre}"), native report: $(origin "${native}")"
check "native_code_after_firmware_exit" "${native_fresh}" \
    "firmware variable written by the staged image after ExitBootServices: $([[ "${native_fresh}" == 1 ]] && echo "yes (kind $(field report_kind "${native}"))" || echo "no, $(origin "${native}")")"
check "kernel_alive" "$([[ "${native_fresh}" == 1 ]] && grep -qx "kernel: alive" <<<"${native}" && echo 1 || echo 0)" \
    "last stage $([[ "${native_fresh}" == 1 ]] && field last_stage "${native}" || echo "unknown: no report from the staged image")"
check "boot_next_consumed" "$(grep -q "^BootNext:" <<<"${boot_state}" && echo 0 || echo 1)" \
    "no pending one-time boot"
# Entries are judged by what they load, not by number: the firmware chooses
# numbers for the removable media it finds at power-on and on 2026-10-20 gave
# the stick Boot0004 while Boot0004 was the AIENOS entry BootNext named.
entry_line() { grep -E "^Boot$1\\*? " <<<"${boot_state}" | head -1; }
entry_label() { entry_line "$1" | sed -E 's/^Boot[0-9A-Fa-f]{4}\*? //; s/\t.*//'; }
loads_aienos() { entry_line "$1" | grep -qiF 'File(\EFI\AIENOS\'; }
# Firmware may append entries for removable media it finds (a USB stick). The
# gate is that the original entries keep their order and AIENOS never joined.
order_before="$(field boot_order_before "${staged}")"
order_now="$(field BootOrder "${boot_state}")"
kept="$(tr ',' '\n' <<<"${order_now}" | grep -Fx -f <(tr ',' '\n' <<<"${order_before}") | paste -sd,)"
added="" aienos_in_order=""
for n in $(tr ',' '\n' <<<"${order_now}" | grep -Fvx -f <(tr ',' '\n' <<<"${order_before}")); do
    added="${added:+${added}; }${n} = $(entry_label "${n}")"
done
for n in $(tr ',' ' ' <<<"${order_now}"); do
    loads_aienos "${n}" && aienos_in_order="${aienos_in_order:+${aienos_in_order},}${n}"
done
check "boot_order_preserved" "$([[ "${kept}" == "${order_before}" && -z "${aienos_in_order}" ]] && echo 1 || echo 0)" \
    "before ${order_before}, now ${order_now}${added:+ (added: ${added})}${aienos_in_order:+; AIENOS loader in BootOrder: ${aienos_in_order}}"
aienos_entry="$(field aienos_boot_entry "${staged}")"
check "aienos_entry_kept" "$([[ -n "${aienos_entry}" ]] && entry_line "${aienos_entry}" | grep -qiF "File(${loader})" && echo 1 || echo 0)" \
    "Boot${aienos_entry:-????} $(if [[ -z "$(entry_line "${aienos_entry:-none}")" ]]; then echo "is gone";
        elif entry_line "${aienos_entry}" | grep -qiF "File(${loader})"; then echo "still loads ${loader}";
        else echo "now holds '$(entry_label "${aienos_entry}")', not ${loader}: the firmware replaced it before the boot, so BootNext most likely started that instead"; fi)"
check "linux_entry_booted" "$([[ "$(field BootCurrent "${boot_state}")" == "$(field boot_current_before "${staged}")" ]] && echo 1 || echo 0)" \
    "BootCurrent $(field BootCurrent "${boot_state}")"
check "linux_kernel_unchanged" "$([[ "$(uname -r)" == "$(field linux_kernel_before "${staged}")" ]] && echo 1 || echo 0)" \
    "$(uname -r)"
check "root_filesystem_writable" "$([[ "$(findmnt -no OPTIONS / | cut -d, -f1)" == "rw" ]] && echo 1 || echo 0)" "/"

if [[ "${failed}" == 0 ]]; then
    echo "M2_GATE: PASS"
else
    echo "M2_GATE: FAIL"
    exit 1
fi
