#!/usr/bin/env bash
# Allen on AArch64 AIENOS in QEMU (make full CK_CONSOLE_SESSION=1 CK_ALLEN=1).
#
# Boots the Allen image under UEFI (AAVMF) with an NVMe disk, a qemu-xhci USB
# keyboard and a ramfb screen, waits for the session, types on the USB
# keyboard through the QEMU monitor ("sendkey"), and checks:
#   A1 no empty answer (welcome only when Allen remembers someone; NULL prints nothing)
#   A2 the prompt is Allen's                         ("you> ")
#   A3 "hello allen" gets Allen's greeting answer    ("allen: Hello Humanity.")
#   A4 a second line gets an answer                  (two or more answers after typing)
#   A5 no machine named on screen                    (no "imac", "spark", "dgx", "gb10" in Allen's lines)
#   A6 "goodbye" ends the session and the boot ends   ("console_session: end (exit)", report_kind: final)
# QEMU only: a PASS says nothing about hardware.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
final_tag="AIENOS_CK_ALLEN_ARM64"
code_fd="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
vars_fd="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
command -v qemu-system-aarch64 >/dev/null || { echo "${final_tag}: NOT_RUN (qemu-system-aarch64 missing)"; exit 2; }
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${final_tag}: NOT_RUN (AAVMF missing)"; exit 2; }

cross=""
if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
out="${repo_root}/target/native-kernel"
make -s -C native/kernel CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" full CK_CONSOLE_SESSION=1 CK_ALLEN=1 >/dev/null
efi="${AIENOS_ALLEN_EFI:-${out}/full-allen/BOOTAA64.EFI}" # AIENOS_ALLEN_EFI: run a prebuilt image (e.g. the hardware one) instead

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib_gate_hold.sh"
if ! gh_quiet_check; then echo "${final_tag}: NOT_RUN (quiet flag held: ${gh_why})"; exit 3; fi
if ! gh_lock_take qemu_ck_allen_arm64_test "${AIENOS_GATE_MINUTES:-60}"; then echo "${final_tag}: NOT_RUN (QEMU gate lock held: ${gh_why})"; exit 3; fi

work="$(mktemp -d)"
qemu_pid=""
cleanup() {
    [[ -z "${qemu_pid}" ]] || kill "${qemu_pid}" 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    [[ -z "${AIENOS_KEEP_WORK:-}" ]] && rm -rf "${work}" || echo "work kept: ${work}"
    gh_lock_release
}
trap cleanup EXIT

serial_has() { tr -d '\r' 2>/dev/null <"${work}/serial.log" | grep -qE -- "$1"; }
qemu_running() { [[ -n "${qemu_pid}" ]] && kill -0 "${qemu_pid}" 2>/dev/null; }
wait_for() { # seconds ERE: 0 once the serial log matches, 1 when QEMU ended or the wait ran out
    local deadline=$(( $(date +%s) + $1 )); shift
    while (( $(date +%s) < deadline )) && qemu_running; do serial_has "$1" && return 0; sleep 0.3; done
    serial_has "$1"
}
key() { echo "sendkey $1" >&3; sleep 0.12; }
usb_say() { local s="$1" i c; for ((i = 0; i < ${#s}; i++)); do c="${s:i:1}"; [[ "$c" == " " ]] && c=spc; key "$c"; done; key ret; sleep 1; }

kbd_dev="-device usb-kbd,bus=xhci.0,id=kbd0"
[[ -z "${AIENOS_NO_KBD:-}" ]] || kbd_dev=""
mkdir -p "${work}/esp/EFI/BOOT" "${work}/esp/EFI/AIENOS"
touch "${work}/esp/EFI/AIENOS/BOOTREPORT.TXT"
cp "${efi}" "${work}/esp/EFI/BOOT/BOOTAA64.EFI"
cp "${vars_fd}" "${work}/vars.fd"
truncate -s 67108864 "${work}/nvme.img"
mkfifo "${work}/mon.in" "${work}/mon.out" "${work}/ser.in" "${work}/ser.out"
: >"${work}/serial.log"
qemu-system-aarch64 \
    -M virt,virtualization=on,gic-version=3,iommu=smmuv3 -accel tcg,thread=single -cpu max -smp 4 -m 2048 \
    -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
    -drive if=pflash,format=raw,file="${work}/vars.fd" \
    -drive if=none,id=esp,format=raw,file=fat:rw:"${work}/esp" -device virtio-blk-pci,drive=esp \
    -drive if=none,id=nvme0,format=raw,file="${work}/nvme.img" -device nvme,drive=nvme0,serial=aienos-allen-test \
    -device qemu-xhci,id=xhci ${kbd_dev} \
    -device ramfb -display none -nic none \
    -chardev pipe,id=mon,path="${work}/mon" -mon chardev=mon,mode=readline \
    -chardev pipe,id=ser,path="${work}/ser" -serial chardev:ser -no-reboot 2>"${work}/qemu.err" &
qemu_pid=$!
cat "${work}/mon.out" >/dev/null &
cat "${work}/ser.out" >>"${work}/serial.log" &
exec 3>"${work}/mon.in"

fails=0
row() { if [[ "$2" == 1 ]]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
if [[ -n "${AIENOS_NO_KBD:-}" ]]; then # hardware Allen image without a keyboard: say so, end, reset
    wait_for "${AIENOS_QEMU_WAIT:-900}" "^report_kind: (final|panic|fault)" || true
    sleep 3
    log="$(tr -d "\\r" <"${work}/serial.log")"
    row "B1 Allen says it cannot hear the keyboard" $(grep -q "allen: I can.t hear the keyboard" <<<"${log}" && echo 1 || echo 0)
    row "B2 the boot ends (report_kind: final) instead of waiting" $(grep -q "report_kind: final" <<<"${log}" && echo 1 || echo 0)
    hold="$(tr -d "\\r" <"${work}/serial.log" | sed -n 's/^screen: holding \([0-9]*\) s before reset$/\1/p' | head -1)"
    hold_end=$(( $(date +%s) + ${hold:-0} + 30 )) # the hardware image holds its last screen before reset
    while qemu_running && (( $(date +%s) < hold_end )); do sleep 1; done
    row "B3 QEMU exited on its own (reset, after a ${hold:-0} s screen hold)" $(qemu_running && echo 0 || echo 1)
    if [[ "${fails}" == 0 ]]; then echo "${final_tag}_NOKBD: PASS (QEMU only)"; exit 0; else echo "${final_tag}_NOKBD: FAIL"; exit 1; fi
fi
if ! wait_for "${AIENOS_QEMU_WAIT:-900}" '^you> |^console_session: ready'; then
    echo "session never opened; last lines:"; tail -n 15 "${work}/serial.log" | tr -d '\r'
    echo "${final_tag}: FAIL (no session)"; exit 1
fi
sleep 2
usb_say "hello allen"
wait_for 60 'allen: Hello Humanity' || true
usb_say "how are you"
sleep 3
usb_say "goodbye"
wait_for 300 '^report_kind: (final|panic|fault)' || true

log="$(tr -d '\r' <"${work}/serial.log")"
allen_lines="$(grep -E '^allen: |allen: ' <<<"${log}" || true)"
first_allen=$(grep -nE 'allen: ' <<<"${log}" | head -1 | cut -d: -f1)
first_prompt=$(grep -nE 'you> ' <<<"${log}" | head -1 | cut -d: -f1)
row "A1 no empty answer (no \"(null)\" line, welcome only when remembered)" $(grep -qE "allen: \(null\)|allen: *$" <<<"${log}" && echo 0 || echo 1)
row "A2 the prompt is Allen's (you> )" $(grep -q 'you> ' <<<"${log}" && ! grep -q 'console> ' <<<"${log}" && echo 1 || echo 0)
row "A3 hello allen -> Hello Humanity." $(grep -q 'allen: Hello Humanity\.' <<<"${log}" && echo 1 || echo 0)
row "A4 every typed line answered (3 lines, 3 answers)" $([[ $(wc -l <<<"${allen_lines}") -ge 3 ]] && echo 1 || echo 0)
row "A5 no machine named on screen" $(grep -qiE 'imac|spark|dgx|gb10|apple|nvidia' <<<"${allen_lines}" && echo 0 || echo 1)
row "A6 goodbye ends the session and the boot" $(grep -q 'console_session: end (exit)' <<<"${log}" && grep -q 'report_kind: final' <<<"${log}" && echo 1 || echo 0)
echo "--- Allen said:"; sed 's/^.*allen: /  /' <<<"${allen_lines}"
if [[ "${fails}" == 0 ]]; then echo "${final_tag}: PASS (QEMU only; hardware NOT_RUN)"; else echo "${final_tag}: FAIL (${fails} rows)"; exit 1; fi
