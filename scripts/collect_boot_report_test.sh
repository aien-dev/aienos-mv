#!/usr/bin/env bash
# Host test of scripts/collect_boot_report.sh: replays real Machine 1 evidence
# and a synthetic good boot through the M2 gate, with efibootmgr stubbed on
# PATH and the ESP folder and report variable pointed at temporary files.
# Reads no machine state and writes nothing outside a temporary directory.
#
# Cases:
#   1. 2026-09-24 first boot (evidence/m2_first_boot_2026-09-24.md): PASS.
#   2. 2026-10-20 attended boot 1 (evidence/spark_native_boot1_2026-10-20/):
#      the AIENOS image never ran (the variable still held the September
#      report, BOOTREPORT.TXT was empty) and the firmware gave the AIENOS
#      entry's number, Boot0004, to the USB stick it found at power-on. The
#      gate must FAIL for those reasons, and must not call the stick's
#      firmware entry an AIENOS entry in BootOrder.
#   3. A good boot of a new commit with the stick appended by firmware: PASS.
#   4. The AIENOS loader joined BootOrder under a new number: FAIL.
# Ends with "COLLECT_BOOT_REPORT_TEST: PASS" or "... FAIL".
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
collector="${repo_root}/scripts/collect_boot_report.sh"
boot1="${repo_root}/evidence/spark_native_boot1_2026-10-20"
t="$(mktemp -d)"
trap '[[ -n "${KEEP:-}" ]] || rm -rf "${t}"' EXIT
st=0

# Text between two "== " headers of a saved collector or capture output.
section() { awk -v h="$2" '$0 == h {on = 1; next} on && /^== / {exit} on' "$1"; }

stick=$'Boot0004* UEFI: USB USB Hard Drive, Partition 1\tVenHw(0d51905b-b77e-452a-a2c0-eca0cc8d514a,0000b91d0000000000)/USB(1,0)/USB(0,0)/HD(1,GPT,f227357c-1e80-4fd0-a55e-2efbef1dd25c,0x800,0x1df000)0000424f'
ubuntu=$'Boot0001* ubuntu\tHD(1,GPT,7c2b24e9-0ceb-4ad7-a401-9bf3c92c26ee,0x800,0x95000)/File(\\EFI\\ubuntu\\shimaa64.efi) File(.)'
pxe=$'Boot0003* UEFI: PXE IPv4 Realtek PCIe 10 GBE Family Controller\tPcieRoot(0x7)/Pci(0x0,0x0)/Pci(0x0,0x0)/MAC(4cbb472bb87b,0)/IPv4(0.0.0.00.0.0.0,0,0)0000424f'
handoff() { printf 'Boot%s* AIENOS handoff (one-time)\tHD(1,GPT,7c2b24e9-0ceb-4ad7-a401-9bf3c92c26ee,0x800,0x95000)/File(\\EFI\\AIENOS\\aienos-handoff.efi)' "$1"; }

# setup <case> <staged text> <pre-exit file text> <variable text> <efibootmgr text>
setup() {
    local d="${t}/$1"
    mkdir -p "${d}/esp" "${d}/bin"
    printf 'image %s\n' "$1" >"${d}/esp/aienos-handoff.efi"
    local digest; digest="$(sha256sum "${d}/esp/aienos-handoff.efi" | cut -d' ' -f1)"
    # Same image digest and Linux kernel as staged, so only the cases' own differences count.
    sed -e "s/^image_sha256: .*/image_sha256: ${digest}/" -e "s/^linux_kernel_before: .*/linux_kernel_before: $(uname -r)/" \
        <<<"$2" >"${d}/esp/STAGED.TXT"
    printf '%s' "$3" >"${d}/esp/BOOTREPORT.TXT"
    if [[ -n "$4" ]]; then { printf '\x07\x00\x00\x00'; printf '%s\n' "$4"; } >"${d}/var"; fi
    printf '%s\n' "$5" >"${d}/efibootmgr.txt"
    printf '#!/bin/sh\ncat "%s"\n' "${d}/efibootmgr.txt" >"${d}/bin/efibootmgr"
    chmod +x "${d}/bin/efibootmgr"
}
run() { # case -> output in ${t}/<case>/out, exit code in ${t}/<case>/rc
    local d="${t}/$1" rc=0
    PATH="${d}/bin:${PATH}" AIENOS_ESP_DIR="${d}/esp" AIENOS_REPORT_VAR="${d}/var" \
        bash "${collector}" >"${d}/out" 2>&1 || rc=$?
    echo "${rc}" >"${d}/rc"
}
expect() { # case pattern(extended regex, whole line) description
    if grep -qE "^$2\$" "${t}/$1/out"; then echo "PASS  $1: $3"; else
        echo "FAIL  $1: $3 (no line matching: $2)"; st=1; fi
}
reject() { # case pattern description
    if grep -qE "^$2\$" "${t}/$1/out"; then echo "FAIL  $1: $3 (line present: $2)"; st=1; else
        echo "PASS  $1: $3"; fi
}

# 1. September first boot, from the ledger payload in the M2 evidence page.
sept_md="${repo_root}/evidence/m2_first_boot_2026-09-24.md"
sept_payload="$(awk '/^```text$/ {on = 1; next} on && /^```$/ {exit} on' "${sept_md}")"
sept_staged="$(section <(printf '%s\n' "${sept_payload}") "== staging record (/boot/efi/EFI/AIENOS/STAGED.TXT)")"
sept_native="$(section <(printf '%s\n' "${sept_payload}") "== native firmware-variable report")"
sept_boot="$(printf 'BootCurrent: 0001\nTimeout: 1 seconds\nBootOrder: 0001,0003,0004\n%s\n%s\n%s\n%s' \
    "$(handoff 0000)" "${ubuntu}" "${pxe}" "${stick}")"
setup sept "${sept_staged}" "" "${sept_native}" "${sept_boot}"
run sept
expect sept "PASS  kernel_alive: .*" "kernel alive in a report from the staged commit"
expect sept "PASS  boot_order_preserved: .*" "firmware-added stick is not an AIENOS entry"
expect sept "PASS  aienos_entry_kept: .*" "Boot0000 still loads the AIENOS image"
expect sept "M2_GATE: PASS" "gate passes"

# 2. Attended boot 1 on 2026-10-20, from the saved collector output and the L318 read-only capture.
b1_staged="$(section "${boot1}/collect_boot_report.txt" "== staging record (/boot/efi/EFI/AIENOS/STAGED.TXT)")"
b1_native="$(section "${boot1}/collect_boot_report.txt" "== native firmware-variable report")"
b1_boot="$(section "${boot1}/fixer_readonly_capture_L318.txt" "== efibootmgr (plain; the collector's input)")"
setup boot1 "${b1_staged}" "" "${b1_native}" "${b1_boot}"
run boot1
expect boot1 "FAIL  left_firmware_into_aienos: .*earlier boot.*52109bcb.*" "native report named as left over from commit 52109bcb"
expect boot1 "FAIL  native_code_after_firmware_exit: .*" "no native report from this boot"
expect boot1 "FAIL  kernel_alive: .*" "stale 'kernel: alive' does not count"
expect boot1 "PASS  boot_order_preserved: .*0004 = UEFI: USB USB Hard Drive, Partition 1.*" "stick entry named, not taken for AIENOS"
expect boot1 "FAIL  aienos_entry_kept: Boot0004 .*UEFI: USB USB Hard Drive, Partition 1.*" "firmware replacing Boot0004 is reported"
expect boot1 "M2_GATE: FAIL" "gate fails"

# 3. Good boot of a new commit: both reports from it, stick appended by firmware.
commit="1111111111111111111111111111111111111111"
good_staged="$(sed -e "s/^aienos_commit: .*/aienos_commit: ${commit}/" -e "s/^aienos_boot_entry: .*/aienos_boot_entry: 0005/" <<<"${b1_staged}")"
good_native="$(sed "s/^aienos_commit: .*/aienos_commit: ${commit}/" <<<"${b1_native}")"
good_pre="$(printf 'report_version: 1\naienos_commit: %s\nreport_kind: pre_exit\nlast_stage: pre_exit_report_saved\n' "${commit}")"
good_boot="$(printf 'BootCurrent: 0001\nTimeout: 1 seconds\nBootOrder: 0001,0003,0004\n%s\n%s\n%s\n%s' \
    "${ubuntu}" "${pxe}" "${stick}" "$(handoff 0005)")"
setup good "${good_staged}" "${good_pre}" "${good_native}" "${good_boot}"
run good
expect good "PASS  left_firmware_into_aienos: pre-exit file: from the staged image, native report: from the staged image" "both reports from this commit"
expect good "PASS  kernel_alive: .*" "kernel alive"
expect good "M2_GATE: PASS" "gate passes"

# 4. The AIENOS loader joined BootOrder under another number.
joined_boot="${good_boot/BootOrder: 0001,0003,0004/BootOrder: 0001,0003,0004,0006}"$'\n'"$(handoff 0006)"
setup joined "${good_staged}" "${good_pre}" "${good_native}" "${joined_boot}"
run joined
expect joined "FAIL  boot_order_preserved: .*" "AIENOS loader in BootOrder fails"
reject joined "M2_GATE: PASS" "gate does not pass"

if [[ "${st}" == 0 ]]; then echo "COLLECT_BOOT_REPORT_TEST: PASS"; exit 0; fi
for c in sept boot1 good joined; do echo "--- ${c} output"; cat "${t}/${c}/out"; done
echo "COLLECT_BOOT_REPORT_TEST: FAIL"
exit 1
