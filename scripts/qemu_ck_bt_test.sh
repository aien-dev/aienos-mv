#!/usr/bin/env bash
# Bluetooth-over-USB transfer gate for the AIENOS C kernel (L207): proves the
# generic xHCI pipes (native/kernel/dev/xhci_pipe.c) that the Bluetooth
# transport (dev/usb_bt.c) rides on. This QEMU (8.2.2) has no usb-bt-dongle, so
# no HCI byte is exchanged here; the HCI state machine is proven separately by
# the host controller simulator (make -f stage.mk bt-test). QEMU is not
# hardware: a PASS here qualifies nothing on the DGX Spark's MediaTek adapter.
#
# Boots the TEST image (make full CK_BT_TEST=1) in QEMU AArch64 with UEFI, an
# NVMe disk, one qemu-xhci with a usb-kbd and a usb-storage stick (first 4 KiB
# a known pattern, snapshot=on so the stick image is never changed):
#   row B1  the pipes start ("bt: xhci pipes running ports=N")
#   row B2  bulk: Bulk-Only INQUIRY, CBW out, 36 bytes in, CSW in, vendor QEMU
#   row B3  bulk IN of 4096 bytes (many packets) equals the pattern
#   row B4  bulk OUT data stage: WRITE(10) then READ(10) of 512 bytes round trip
#   row B5  interrupt IN: a key typed through the QEMU monitor arrives as an
#           8-byte HID report with usage 0x04 in byte 2
#   row B6  "bt: no Bluetooth controller" (none is attached)
#   row B7  release: xHCI halted, bus master revoked, COMMAND read back off,
#           stream returned to abort, final report, QEMU exit 0, M1 checks
#   row B8  default image carries no Bluetooth or transfer-proof code and
#           prints no "bt" line
# --mutation (self-test): the pipes are broken on purpose and the named row
# must FAIL while the boot still reaches the final report:
#   bulk-out-short -> B2   bulk-in-offset -> B3   intr-drop -> B5
# Exit 0 PASS, 1 FAIL, 2 missing tools, 3 NOT_RUN (quiet flag or QEMU gate lock).
# Final line: "AIENOS_CK_BT: PASS|FAIL|NOT_RUN" ("AIENOS_CK_BT_MUTATION" for --mutation).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

mutation_run=0
case "${1:-}" in
    "") ;;
    --mutation) mutation_run=1 ;;
    *) echo "usage: $0 [--mutation]"; exit 2 ;;
esac
final_tag="AIENOS_CK_BT"
[[ "${mutation_run}" == 0 ]] || final_tag="AIENOS_CK_BT_MUTATION"

code_fd="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
vars_fd="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 not installed"; echo "${final_tag}: NOT_RUN (qemu-system-aarch64 missing)"; exit 2; }
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "AAVMF firmware not found"; echo "${final_tag}: NOT_RUN (AAVMF missing)"; exit 2; }

cross=""
if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
out="${repo_root}/target/native-kernel"
mk() { make -s -C native/kernel CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" "$@"; }
mk full CK_BT_TEST=1 >/dev/null
test_efi="${out}/full-bt-test/BOOTAA64.EFI"
mk full >/dev/null
default_efi="${out}/full/BOOTAA64.EFI"
mutations=(bulk-out-short bulk-in-offset intr-drop)
if [[ "${mutation_run}" == 1 ]]; then
    for m in "${mutations[@]}"; do mk full CK_BT_TEST=1 CK_TEST_BT_MUTATION="${m}" >/dev/null; done
fi

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib_gate_hold.sh"
if ! gh_quiet_check; then
    echo "NOT_RUN  ${gh_why}"
    echo "${final_tag}: NOT_RUN (quiet flag held)"
    exit 3
fi
if ! gh_lock_take qemu_ck_bt_test "${AIENOS_GATE_MINUTES:-60}"; then
    echo "NOT_RUN  ${gh_why}"
    echo "${final_tag}: NOT_RUN (QEMU gate lock held)"
    exit 3
fi
top="$(mktemp -d)"
qemu_pid=""
cleanup() {
    [[ -z "${qemu_pid}" ]] || kill "${qemu_pid}" 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    rm -rf "${top}"
    gh_lock_release
}
trap cleanup EXIT

# shellcheck source=scripts/lib_ck_m1_checks.sh
source "${repo_root}/scripts/lib_ck_m1_checks.sh"
image="${top}/nvme.img"
truncate -s 67108864 "${image}"
# The stick: first 4096 bytes are "AIENOS-L207-BULK-" repeated (17 bytes, not a divisor of 512 on purpose).
stick="${top}/stick.img"
truncate -s 1048576 "${stick}"
yes AIENOS-L207-BULK- | tr -d '\n' | head -c 4096 | dd of="${stick}" conv=notrunc status=none
m1_fail=0
boot_timeout="${AIENOS_QEMU_TIMEOUT:-300}"

serial_has() { tr -d '\r' 2>/dev/null <"${work}/serial.log" | grep -qE -- "$1"; }
qemu_running() { [[ -n "${qemu_pid}" ]] && kill -0 "${qemu_pid}" 2>/dev/null; }
wait_for() {
    local deadline=$(( $(date +%s) + $1 )) p; shift
    while (( $(date +%s) < deadline )) && qemu_running; do
        for p in "$@"; do serial_has "${p}" && return 0; done
        sleep 0.2
    done
    for p in "$@"; do serial_has "${p}" && return 0; done
    return 1
}
send_keys() { local k; for k in "$@"; do echo "sendkey ${k}" >&3; sleep 0.12; done; }

# boot <name> <efi> typed: one QEMU boot. typed=yes: end the keyboard shell with
# "exit" and type 'a' once the interrupt transfer is armed (through the monitor,
# outside the guest).
boot() {
    work="${top}/$1"
    local typed="$3"
    mkdir -p "${work}/esp/EFI/BOOT" "${work}/esp/EFI/AIENOS"
    touch "${work}/esp/EFI/AIENOS/BOOTREPORT.TXT"
    cp "$2" "${work}/esp/EFI/BOOT/BOOTAA64.EFI"
    cp "${vars_fd}" "${work}/vars.fd"
    rm -f "${work}/mon.in" "${work}/mon.out"
    mkfifo "${work}/mon.in" "${work}/mon.out"
    : >"${work}/serial.log"
    timeout "${boot_timeout}" qemu-system-aarch64 \
        -M "virt,virtualization=on,gic-version=3,iommu=smmuv3" -accel tcg,thread=single -cpu max -smp 4 -m 2048 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/vars.fd" \
        -drive if=none,id=esp,format=raw,file=fat:rw:"${work}/esp" \
        -device virtio-blk-pci,drive=esp \
        -drive if=none,id=nvme0,format=raw,file="${image}" \
        -device nvme,drive=nvme0,serial=aienos-bt-test \
        -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
        -drive if=none,id=stick,format=raw,file="${stick}",snapshot=on \
        -device usb-storage,bus=xhci.0,drive=stick \
        -device ramfb -display none -nic none \
        -chardev pipe,id=mon,path="${work}/mon" -mon chardev=mon,mode=readline \
        -serial file:"${work}/serial.log" -no-reboot 2>"${work}/qemu.err" &
    qemu_pid=$!
    cat "${work}/mon.out" >/dev/null &
    exec 3>"${work}/mon.in"
    if [[ "${typed}" == yes ]]; then
        # The keyboard phase comes first and waits for the recovery key; end its shell quickly.
        if wait_for "${boot_timeout}" '^keyboard: shell ready' '^keyboard: unavailable' '^report_kind: (panic|fault|final)'; then
            if serial_has '^keyboard: shell ready'; then send_keys e x i t ret; fi
        fi
        if wait_for "${boot_timeout}" '^bt_xfer: interrupt in armed' '^report_kind: (panic|fault|final)'; then
            if serial_has '^bt_xfer: interrupt in armed'; then
                for _ in 1 2 3 4 5 6; do
                    serial_has '^bt_xfer: interrupt (report|saw no report)' && break
                    send_keys a
                    sleep 1
                done
            fi
        fi
    fi
    set +e
    wait "${qemu_pid}"
    qemu_status=$?
    set -e
    qemu_pid=""
    exec 3>&- 2>/dev/null || true
    tr -d '\r' <"${work}/serial.log" >"${work}/serial.txt"
    [[ -z "${AIENOS_LOG_DIR:-}" ]] || cp "${work}/serial.txt" "${AIENOS_LOG_DIR}/qemu_ck_bt_$1.log"
    echo "== boot $1: qemu exit ${qemu_status}"
}

all_rows=(B1 B2 B3 B4 B5 B6 B7 B8)
declare -A rowfail rowmodes
reset_rows() { local r; for r in "${all_rows[@]}"; do rowfail[$r]=0; rowmodes[$r]=""; done; }
reset_rows
mode=""
note_mode() { [[ " ${rowmodes[$1]} " == *" ${mode} "* ]] || rowmodes[$1]="${rowmodes[$1]:+${rowmodes[$1]} }${mode}"; }
rpass() { note_mode "$1"; echo "ROW $1 PASS  ${mode}: $2"; }
rfail() { note_mode "$1"; rowfail[$1]=1; echo "ROW $1 FAIL  ${mode}: $2"; }
rcheck() { if grep -qE -- "$3" "${work}/serial.txt"; then rpass "$1" "$2"; else rfail "$1" "$2"; fi; }
rabsent() { if grep -qE -- "$3" "${work}/serial.txt"; then rfail "$1" "$2"; else rpass "$1" "$2"; fi; }
line_of() { { grep -nE -m1 -- "$1" "${work}/serial.txt" || true; } | cut -d: -f1; }
before() {
    local a b
    a="$(line_of "$3")"; b="$(line_of "$4")"
    if [[ -n "${a}" && -n "${b}" && "${a}" -lt "${b}" ]]; then rpass "$1" "$2 (lines ${a} < ${b})"
    else rfail "$1" "$2 (lines ${a:-none} ${b:-none})"; fi
}

evaluate() {
    mode=test
    rcheck B1 "xHCI pipes running" '^bt: xhci pipes running ports=[0-9]+ context_bytes=(32|64)$'
    rcheck B2 "bulk INQUIRY over CBW out, data in, CSW in" "^bt_xfer: bulk INQUIRY ok \\(36 bytes in, max packet in=[0-9]+ out=[0-9]+\\) vendor='QEMU    ' product='QEMU HARDDISK   '$"
    rcheck B3 "4096-byte bulk IN equals the pattern" '^bt_xfer: bulk READ\(10\) 4096 bytes in, pattern ok \(0 bad bytes\)$'
    rcheck B4 "bulk OUT data stage round trip" '^bt_xfer: bulk WRITE\(10\)\+READ\(10\) 512 bytes round trip ok \(rc=0, 0 differing bytes\)$'
    rcheck B5 "interrupt IN armed" '^bt_xfer: interrupt in armed \(endpoint 0x81, max packet 8\), waiting for a key$'
    rcheck B5 "interrupt report carries the typed key" '^bt_xfer: interrupt report 8 bytes: [0-9a-f]{2} [0-9a-f]{2} 04 '
    rcheck B6 "no controller reported" '^bt: no Bluetooth controller$'
    rcheck B7 "controller halted before the revoke" '^xhci: halt before revoke usbcmd=0x[0-9a-f]{8} usbsts=0x[0-9a-f]{8} halted=yes$'
    rcheck B7 "bus master revoked" '^dma_gate: xhci bus master revoked$'
    rcheck B7 "COMMAND read back: bus master off" '^xhci_pci: after phase command=0x[0-9a-f]{4} bus_master=off$'
    rcheck B7 "stream returned to abort" '^smmu: xhci stream 0x[0-9a-f]+ returned to abort \(rc=0\)$'
    before B7 "order: Bluetooth step < revoke" '^bt: xhci pipes running' '^dma_gate: xhci bus master revoked$'
    before B7 "order: revoke < read back" '^dma_gate: xhci bus master revoked$' '^xhci_pci: after phase command='
    rcheck B7 "final report reached" '^report_kind: final$'
    rabsent B7 "no panic or fault" '^report_kind: (panic|fault)'
    if [[ "${qemu_status}" == 0 ]]; then rpass B7 "QEMU exit 0"; else rfail B7 "QEMU exit ${qemu_status}"; fi
    failed=0
    ck_m1_checks
    [[ "${failed}" == 0 ]] || m1_fail=1
    if [[ "${failed}" == 0 ]]; then rpass B7 "M1 checks"; else rfail B7 "an M1 check failed"; fi
    rcheck B7 "image is this commit" "aienos_commit: ${commit}"
}

if [[ "${mutation_run}" == 0 ]]; then
    gate_fail=0
    boot bt-test "${test_efi}" yes
    evaluate
    rabsent B7 "no pipe-mutation banner" 'TEST-ONLY USB PIPE MUTATION'
    boot bt-default "${default_efi}" no
    mode=default
    rabsent B8 "default image prints no bt line" '^(bt|bt_xfer): '
    rcheck B8 "default image still boots to the final report" '^report_kind: final$'
    if grep -aqF "bt_xfer" "${default_efi}"; then rfail B8 "default image carries the transfer-proof code"; else rpass B8 "default image carries no transfer-proof code"; fi
    if grep -aqF "hci bring-up" "${default_efi}"; then rfail B8 "default image carries the live HCI bring-up"; else rpass B8 "default image carries no live HCI bring-up"; fi
    echo "== per-row results (QEMU only; no Bluetooth controller exists here; hardware NOT_RUN)"
    for r in "${all_rows[@]}"; do
        if [[ -z "${rowmodes[$r]}" ]]; then echo "BT_ROW ${r}: NOT_RUN"; gate_fail=1
        elif [[ "${rowfail[$r]}" == 0 ]]; then echo "BT_ROW ${r}: PASS (${rowmodes[$r]})"
        else echo "BT_ROW ${r}: FAIL (${rowmodes[$r]})"; gate_fail=1; fi
    done
    if [[ "${gate_fail}" == 0 ]]; then
        echo "${final_tag}: PASS (rows B1-B8; QEMU only, no Bluetooth controller, hardware NOT_RUN)"
        exit 0
    fi
    echo "${final_tag}: FAIL (QEMU only)"
    exit 1
fi

# ---- --mutation: every mutation must make its row FAIL ----
mut_fail=0
mut_ok() { echo "MUTATION PASS  $1"; }
mut_bad() { echo "MUTATION FAIL  $1"; mut_fail=1; }
if grep -aqF "TEST-ONLY USB PIPE MUTATION" "${test_efi}"; then mut_bad "test image carries the mutation banner"; else mut_ok "test image carries no mutation banner"; fi
for combo in "CK_HARDWARE_STAGING=1" "CK_QEMU_UNSAFE_DMA=1"; do
    for flag in CK_BT_TEST=1 "CK_BT_TEST=1 CK_TEST_BT_MUTATION=bulk-out-short" CK_BT=1; do
        set +e
        # shellcheck disable=SC2086
        msg="$(make -s -n -C native/kernel OUT="${top}/refuse" full ${flag} "${combo}" 2>&1)"
        st=$?
        set -e
        if [[ "${flag}" == CK_BT=1 && "${combo}" == CK_QEMU_UNSAFE_DMA=1 ]]; then continue; fi
        if [[ "${st}" != 0 ]] && grep -qE "CK_BT(_TEST)?[^ ]* .*cannot be combined with ${combo%%=*}" <<<"${msg}"; then
            mut_ok "build refuses ${flag} with ${combo}"
        else
            mut_bad "build did not refuse ${flag} with ${combo} (status ${st}): $(head -c 200 <<<"${msg}")"
        fi
    done
done
declare -A mut_row=([bulk-out-short]=B2 [bulk-in-offset]=B3 [intr-drop]=B5)
for m in "${mutations[@]}"; do
    efi="${out}/full-bt-test-${m}/BOOTAA64.EFI"
    boot "mut-${m}" "${efi}" yes
    reset_rows
    evaluate
    r="${mut_row[$m]}"
    if ! grep -qF "WARNING: TEST-ONLY USB PIPE MUTATION BUILD (CK_TEST_BT_MUTATION=" "${work}/serial.txt"; then
        mut_bad "${m}: image did not announce the mutation"
    elif ! grep -q "report_kind: final" "${work}/serial.txt" || [[ "${rowfail[B1]}" != 0 ]]; then
        mut_bad "${m}: boot did not reach the final report with the pipes running (a dead boot proves nothing)"
    elif [[ "${rowfail[$r]}" == 1 ]]; then
        mut_ok "${m}: row ${r} FAILs as it must"
    else
        mut_bad "${m}: row ${r} still PASSES: the check does not catch this mutation"
    fi
done
if [[ "${mut_fail}" != 0 || -n "${AIENOS_QEMU_VERBOSE:-}" ]]; then
    for s in "${top}"/*/serial.txt; do
        echo "---- serial console $(basename "$(dirname "${s}")") (device lines) ----"
        grep -E '^(WARNING|bt|keyboard|xhci|dma_gate|smmu|devices:|report_kind:)' "${s}" | head -60 || true
    done
fi
if [[ "${mut_fail}" == 0 ]]; then echo "${final_tag}: PASS (bulk-out-short->B2 bulk-in-offset->B3 intr-drop->B5)"; exit 0; fi
echo "${final_tag}: FAIL"
exit 1
