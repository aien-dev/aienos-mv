#!/usr/bin/env bash
# qemu_ck_pc_edition_test.sh -- boot the AienOS PC edition release files in QEMU q35
# with an AMD CPU model (-cpu EPYC-Genoa, the closest QEMU 8.2 has to the Ryzen 5
# 7600X: Zen 4) and UEFI (OVMF), and check each reaches the console and powers off
# the way the aienos-run contract says (ledger L261). Host only, TCG on the Spark:
# slow, and QEMU qualifies nothing physical.
#
# Usage: bash scripts/qemu_ck_pc_edition_test.sh [DIST]   (default dist/, from
#        scripts/pc_edition_build.sh). Runs:
#   live-cd    the live ISO as a DVD (AHCI CD drive), no virtio, std VGA, 4 cores
#   live-usb   the live ISO written to a USB stick (xHCI usb-storage), byte for byte
#   vm-qcow2   the VM disk as aienos-run starts it: virtio-blk snapshot=on, a blank
#              qcow2 store disk as vdb, virtio-net user net with UDP 47030 forwarded,
#              virtio-keyboard, virtio-vga, x2APIC on, 4 cores; stopped by QMP
#              system_powerdown (the ACPI power button), must exit by itself
#   vm-vhdx    the Hyper-V disk read by QEMU's vhdx driver, virtio-blk
# Each run: serial must show "aienos: console ready" and the prompt, the commands
# "help" and "status" are typed on COM1, then "poweroff" (or the power button), and
# QEMU must exit by itself (ACPI S5). A screendump is taken at the prompt and must
# hold drawn pixels (the console is on the VM window, not only on serial).
# NOT_RUN (exit 2) when qemu, OVMF or socat is missing; never a silent pass.
# Final line: QEMU_CK_PC_EDITION: PASS (4 runs) or QEMU_CK_PC_EDITION: FAIL <runs>.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dist="${1:-${repo_root}/dist}"
tag=QEMU_CK_PC_EDITION
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
boot_s="${PC_BOOT_TIMEOUT_S:-600}"
notrun() { echo "${tag}: NOT_RUN ($*)"; exit 2; }
for t in qemu-system-x86_64 qemu-img socat; do command -v "$t" >/dev/null || notrun "$t missing"; done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || notrun "no OVMF"
iso="${dist}/aienos-pc-live-x86_64.iso" qcow="${dist}/aienos-pc-vm-x86_64.qcow2" vhdx="${dist}/aienos-pc-vm-x86_64.vhdx"
for f in "${iso}" "${qcow}" "${vhdx}"; do [[ -r "$f" ]] || notrun "missing $f (run scripts/pc_edition_build.sh)"; done
(cd "${dist}" && sha256sum -c --quiet SHA256SUMS) || { echo "${tag}: FAIL SHA256SUMS does not match ${dist}"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
qmp() { printf '{"execute":"qmp_capabilities"}\n%s\n' "$2" | socat -t3 - "UNIX-CONNECT:$1" 2>/dev/null || true; }

# wait_for DIR PATTERN SECONDS: until the serial log has PATTERN or QEMU is gone
wait_for() {
    local d="$1" pat="$2" n=0
    while (( n < $3 )); do
        grep -aq -- "${pat}" "${d}/serial.txt" 2>/dev/null && return 0
        kill -0 "$(cat "${d}/pid")" 2>/dev/null || return 1
        sleep 1; n=$((n + 1))
    done
    return 1
}
type_line() { printf '%s\r' "$2" > "$1/serial.in"; sleep 2; }

fails=()
run() { # NAME STOP(poweroff|button) APIC(x2apic|xapic) QEMU-ARGS...
    local name="$1" stop="$2" apic="$3"; shift 3
    local d="${work}/${name}" ok=1 cpu=EPYC-Genoa
    [[ "${apic}" == x2apic ]] && cpu="EPYC-Genoa,+x2apic"
    mkdir -p "${d}"
    cp "${vars_fd}" "${d}/vars.fd"
    mkfifo "${d}/serial.in" "${d}/serial.out" # QEMU's pipe chardev: PATH.in is read, PATH.out written
    echo "== ${name} (-cpu ${cpu}) =="
    qemu-system-x86_64 -M q35 -accel tcg -cpu "${cpu}" -smp 4 -m 1024 -display none -no-reboot \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" -drive if=pflash,format=raw,file="${d}/vars.fd" \
        -chardev pipe,id=s0,path="${d}/serial" -serial chardev:s0 \
        -qmp unix:"${d}/qmp.sock",server=on,wait=off "$@" > "${d}/qemu.txt" 2>&1 &
    echo $! > "${d}/pid"
    cat "${d}/serial.out" > "${d}/serial.txt" &
    local catpid=$! t0=$SECONDS
    check() { grep -aq -- "$2" "${d}/serial.txt" && echo "  ok: $1" || { echo "  FAIL: $1 (no '$2')"; ok=0; }; }
    if wait_for "${d}" "aienos: console ready" "${boot_s}" && wait_for "${d}" "aienos> " 30; then
        echo "  console ready after $((SECONDS - t0)) s (TCG, not a speed figure)"
        sleep 2
        qmp "${d}/qmp.sock" "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"${d}/screen.ppm\"}}" > /dev/null
        type_line "${d}" help
        wait_for "${d}" "commands: help, status" 60 || true
        type_line "${d}" status
        wait_for "${d}" "status: free_mib=" 60 || true
        check "help answered" "commands: help, status, poweroff, reboot"
        check "status answered" "status: kernel=AIENUX-PC-0"
        check "Secure Boot state recorded" "secure_boot: SecureBoot="
        check "APIC mode ${apic}" "apic: mode=${apic}"
        check "4 cores" "summary: cores=4/4"
        check "no disk written" "writes=none"
        local px
        px=$(tail -c +16 "${d}/screen.ppm" 2>/dev/null | tr -d '\000' | wc -c)
        if (( px > 2000 )); then echo "  ok: screen drawn (${px} non-zero bytes in the screendump)"; else echo "  FAIL: screen blank (${px})"; ok=0; fi
        if [[ "${stop}" == button ]]; then
            qmp "${d}/qmp.sock" '{"execute":"system_powerdown"}' > /dev/null
        else
            type_line "${d}" poweroff
        fi
        local n=0
        while kill -0 "$(cat "${d}/pid")" 2>/dev/null && (( n < 180 )); do sleep 1; n=$((n + 1)); done
        if kill -0 "$(cat "${d}/pid")" 2>/dev/null; then
            echo "  FAIL: still running 180 s after ${stop}"; ok=0
        else
            echo "  ok: QEMU exited by itself ${n} s after ${stop} (ACPI S5)"
        fi
        [[ "${stop}" == button ]] && check "power button seen" "power: power button pressed"
    else
        echo "  FAIL: no console ready within ${boot_s} s"; ok=0
    fi
    kill "$(cat "${d}/pid")" 2>/dev/null || true
    kill "${catpid}" 2>/dev/null || true
    wait 2>/dev/null || true
    grep -aE '^(secure_boot|apic|smp|timebase|virtio_blk_gpt|virtio_net|virtio_input|summary|power):' "${d}/serial.txt" | sed 's/^/  | /' || true
    if [[ -n "${AIENOS_LOG_DIR:-}" ]]; then
        mkdir -p "${AIENOS_LOG_DIR}"
        cp "${d}/serial.txt" "${AIENOS_LOG_DIR}/pc_edition_${name}_serial.txt"
        cp "${d}/screen.ppm" "${AIENOS_LOG_DIR}/pc_edition_${name}_screen.ppm" 2>/dev/null || true
    fi
    [[ ${ok} == 1 ]] && echo "${name}: PASS" || { echo "${name}: FAIL"; fails+=("${name}"); }
}

store="${work}/store.qcow2"
qemu-img create -q -f qcow2 "${store}" 1G
vm_dev=(-netdev user,id=n0,hostfwd=udp:127.0.0.1:$((40000 + RANDOM % 20000))-10.0.2.15:47030 -device virtio-net-pci,netdev=n0
        -device virtio-keyboard-pci -device virtio-vga)

run live-cd poweroff xapic -drive if=none,id=cd,media=cdrom,readonly=on,file="${iso}" -device ide-cd,drive=cd,bus=ide.2 \
    -vga std -nic none
run live-usb poweroff xapic -device qemu-xhci,id=xhci -drive if=none,id=stick,format=raw,readonly=on,file="${iso}" \
    -device usb-storage,bus=xhci.0,drive=stick,removable=on -vga std -nic none
run vm-qcow2 button x2apic -drive if=none,id=vda,format=qcow2,file="${qcow}",snapshot=on -device virtio-blk-pci,drive=vda \
    -drive if=none,id=vdb,format=qcow2,file="${store}" -device virtio-blk-pci,drive=vdb "${vm_dev[@]}" -vga none
run vm-vhdx poweroff xapic -drive if=none,id=vda,format=vhdx,readonly=on,file="${vhdx}" -device virtio-blk-pci,drive=vda \
    "${vm_dev[@]}" -vga none

if (( ${#fails[@]} )); then echo "${tag}: FAIL ${fails[*]}"; exit 1; fi
echo "${tag}: PASS (4 runs: live-cd live-usb vm-qcow2 vm-vhdx; -cpu EPYC-Genoa; QEMU qualifies nothing physical)"
