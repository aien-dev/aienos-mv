#!/usr/bin/env bash
# Stage the AIENOS first-boot evidence image for exactly one boot.
#
# Dry run by default (no flag, or --dry-run): builds the image, runs the
# read-only checks and prints what --apply would do; it changes nothing on the
# machine. With --apply (run it as yourself, not under sudo: it calls sudo
# itself) it STOPs before any write unless scripts/native_boot_preflight.sh
# says READY for this exact image (Linux first in BootOrder, no BootNext
# pending, the AIENOSRECOV stick listed by the firmware and plugged in, clean
# tree, no quiet flag), then saves the pre-boot capture
# (scripts/verify_native_rollback.sh --capture-pre) in the evidence folder,
# copies the image to the EFI system partition, removes the previous boot's
# BOOTREPORT.TXT and AienosBootReportV1 variable (copies kept in the evidence
# folder), creates boot entry Boot0A1E (a number the firmware does not hand
# out) WITHOUT adding it to BootOrder, checks the stick is still listed, writes
# \EFI\AIENOS\STAGED.TXT (commit, image digest, prior boot state) and sets
# BootNext.
# The firmware consumes BootNext on that boot, so the following boot returns to
# the normal BootOrder (Linux) whether AIENOS succeeds, hangs, or is powered off.
# Run it under `aien-proof hold --resource machine-1` so the attempt is recorded.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

apply=0
case "${1:-}" in
    "" | --dry-run) ;;
    --apply) apply=1 ;;
    *) echo "usage: $0 [--dry-run | --apply]" >&2; exit 2 ;;
esac
[[ "${EUID}" -ne 0 ]] || { echo "STOP: run as yourself, not under sudo (the script calls sudo for the writes)"; exit 1; }
# shellcheck source=scripts/lib_recovery_entry.sh
source "${repo_root}/scripts/lib_recovery_entry.sh"

image="target/aarch64-unknown-uefi/release/aienos-handoff.efi"
esp="/boot/efi"
loader='\EFI\AIENOS\aienos-handoff.efi'
label="AIENOS handoff (one-time)"
entry="0A1E"
report_file="${esp}/EFI/AIENOS/BOOTREPORT.TXT"
report_var="/sys/firmware/efi/efivars/AienosBootReportV1-a1e05b0e-7c3d-4f51-9b6a-2d8e4c1f0a37"
secure_boot_var="/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c"

if [[ -n "$(git status --porcelain)" ]]; then
    echo "STOP: the checkout has uncommitted changes; the boot must map to one exact commit."
    exit 1
fi
commit="$(git rev-parse HEAD)"

AIENOS_COMMIT="${commit}" cargo build --quiet --release -p aienos-boot \
    --target aarch64-unknown-uefi --features handoff,hardware-staging --bin aienos-handoff
cargo run --quiet --release -p aienos-evidence -- verify-efi "${image}"

if [[ -r "${secure_boot_var}" ]] && [[ "$(od -An -t u1 -j 4 -N 1 "${secure_boot_var}" | tr -d ' ')" == "1" ]]; then
    echo "STOP: Secure Boot is enabled. Firmware will refuse the unsigned AIENOS image."
    echo "Turn Secure Boot off in the firmware setup screen, then rerun."
    exit 1
fi

source_dev="$(findmnt -no SOURCE "${esp}")"
disk="/dev/$(lsblk -no PKNAME "${source_dev}")"
part="$(cat "/sys/class/block/$(basename "${source_dev}")/partition")"
boot_state="$(efibootmgr)"
order_before="$(sed -n 's/^BootOrder: //p' <<<"${boot_state}")"
current_before="$(sed -n 's/^BootCurrent: //p' <<<"${boot_state}")"
recov_entry="$(recovery_entry "${boot_state}")"
digest="$(sha256sum "${image}" | cut -d' ' -f1)"
staged_by="${AIEN_AGENT_ID:-${USER:-unknown}}"
evidence_dir="${AIENOS_STAGE_OUT:-${HOME}/workspace/evidence-out/handoff/staging}/$(date -u +%Y%m%dT%H%M%SZ)"

# Read-only preflight on this exact image; --apply writes nothing unless READY.
preflight_rc=0
bash scripts/native_boot_preflight.sh --image "${image}" --expect-sha256 "${digest}" || preflight_rc=$?

record="$(mktemp)"
trap 'rm -f "${record}"' EXIT
cat >"${record}" <<EOF
aienos_commit: ${commit}
image_sha256: ${digest}
staged_by: ${staged_by}
staged_at_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)
boot_current_before: ${current_before}
boot_order_before: ${order_before}
recovery_entry: Boot${recov_entry:-none} (AIENOSRECOV, GPT partition ${AIENOS_RECOV_PARTUUID})
evidence_dir: ${evidence_dir}
linux_kernel_before: $(uname -r)
EOF

echo "commit:      ${commit}"
echo "image:       ${image} (sha256 ${digest})"
echo "recovery:    ${recov_entry:+Boot${recov_entry} AIENOSRECOV stick listed}${recov_entry:-AIENOSRECOV stick NOT listed}"
echo "capture:     ${evidence_dir}/pre_boot_capture.json (sudo scripts/verify_native_rollback.sh --capture-pre)"
echo "copy to:     ${esp}/EFI/AIENOS/aienos-handoff.efi and STAGED.TXT"
echo "clear:       previous ${report_file} and the AienosBootReportV1 variable (copies kept in the evidence folder)"
echo "boot entry:  Boot${entry} '${label}' on ${disk} partition ${part}, loader ${loader}"
echo "BootOrder:   ${order_before} (left unchanged)"
echo "BootNext:    set to the AIENOS entry for one boot only"

if [[ "${apply}" -ne 1 ]]; then
    if [[ "${preflight_rc}" -eq 0 ]]; then
        echo "Dry run: nothing touched. Preflight READY; --apply would stage the boot above."
    else
        echo "Dry run: nothing touched. Preflight NOT_READY; --apply would STOP before any write."
    fi
    exit 0
fi
[[ "${preflight_rc}" -eq 0 ]] || { echo "STOP: preflight NOT_READY (lines above); nothing was written."; exit 1; }

mkdir -p "${evidence_dir}"
sudo scripts/verify_native_rollback.sh --capture-pre "${evidence_dir}/pre_boot_capture.json"
sudo chown "$(id -u):$(id -g)" "${evidence_dir}/pre_boot_capture.json"
sudo install -D -m 0644 "${image}" "${esp}/EFI/AIENOS/aienos-handoff.efi"

# Clear the previous boot's reports so any report found afterwards was written
# by this boot. The variable is non-volatile: on 2026-10-20 it still held the
# 2026-09-24 report after a boot in which the staged image never ran. Copies go
# to the evidence folder first. efivarfs marks vendor variables immutable.
cleared=""
if sudo test -e "${report_file}"; then
    sudo cat "${report_file}" >"${evidence_dir}/previous_BOOTREPORT.TXT"
    sudo rm -f "${report_file}"
    ! sudo test -e "${report_file}" || { echo "STOP: could not remove ${report_file}; BootNext NOT set"; exit 1; }
    cleared="BOOTREPORT.TXT ($(wc -c <"${evidence_dir}/previous_BOOTREPORT.TXT") bytes, commit $(sed -n 's/^aienos_commit: //p' "${evidence_dir}/previous_BOOTREPORT.TXT" | head -1))"
fi
if sudo test -e "${report_var}"; then
    sudo cat "${report_var}" | tail -c +5 >"${evidence_dir}/previous_AienosBootReportV1.txt"
    sudo chattr -i "${report_var}"
    sudo rm -f "${report_var}"
    ! sudo test -e "${report_var}" || { echo "STOP: could not delete the old AienosBootReportV1 variable; BootNext NOT set"; exit 1; }
    cleared="${cleared:+${cleared}; }AienosBootReportV1 (commit $(sed -n 's/^aienos_commit: //p' "${evidence_dir}/previous_AienosBootReportV1.txt" | head -1))"
fi
echo "cleared_before_boot: ${cleared:-nothing}" >>"${record}"

# The entry gets a fixed number far above the ones the firmware hands out (it
# numbered its own entries 0000-0004 on this machine), so a device the
# firmware finds at power-on cannot take it; on 2026-10-20 the stick took
# Boot0004 from the AIENOS entry. efibootmgr refuses a number already in use.
entry_line="$(efibootmgr | grep -E "^Boot${entry}\*? " | head -1)"
if [[ -z "${entry_line}" ]]; then
    sudo efibootmgr --quiet --create-only --bootnum "${entry}" --disk "${disk}" --part "${part}" \
        --label "${label}" --loader "${loader}"
    entry_line="$(efibootmgr | grep -E "^Boot${entry}\*? " | head -1)"
fi
grep -qiF "File(${loader})" <<<"${entry_line}" \
    || { echo "STOP: Boot${entry} is '${entry_line:-missing}', not the AIENOS loader; BootNext NOT set"; exit 1; }

order_after="$(efibootmgr | sed -n 's/^BootOrder: //p')"
if [[ "${order_after}" != "${order_before}" ]]; then
    sudo efibootmgr --quiet --bootorder "${order_before}"
fi
[[ "${entry}" != "${recov_entry}" ]] || { echo "STOP: the new entry took the recovery entry's number; BootNext NOT set"; exit 1; }
[[ "$(recovery_entry "$(efibootmgr)")" == "${recov_entry}" ]] \
    || { echo "STOP: the AIENOSRECOV stick is no longer listed as Boot${recov_entry}; BootNext NOT set"; exit 1; }
echo "aienos_boot_entry: ${entry}" >>"${record}"
cp "${record}" "${evidence_dir}/STAGED.TXT"
sudo install -m 0644 "${record}" "${esp}/EFI/AIENOS/STAGED.TXT"
sudo efibootmgr --quiet --bootnext "${entry}"

cat "${record}"
efibootmgr | sed -n '1,4p'
echo "Staged: the next boot runs Boot${entry} once. Reboot when you are at the machine."
echo "After it returns to Linux: scripts/collect_boot_report.sh, then"
echo "  sudo scripts/verify_native_rollback.sh --capture-post \"${evidence_dir}/post_boot_capture.json\""
echo "  scripts/verify_native_rollback.sh --verify --pre \"${evidence_dir}/pre_boot_capture.json\" --post \"${evidence_dir}/post_boot_capture.json\""
