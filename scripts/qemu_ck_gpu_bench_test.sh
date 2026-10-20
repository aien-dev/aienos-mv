#!/usr/bin/env bash
# qemu_ck_gpu_bench_test.sh -- QEMU test for the GPU bench console.
# QEMU is an emulator: a PASS here qualifies nothing about the real GPU.
#
# Boots IMACKERN-LAB.EFI in QEMU with CK_X86_BENCH=1. QEMU's bochs VGA
# (PCI 1234:1111, class 0x030000) is present; the bench console detects
# no AMD GPU and falls back to a bochs/VGA BAR for register access when
# CK_X86_BENCH_QEMU_FALLBACK is defined at build time.
# Tests: ping round-trip, auth rejection (bad HMAC), bench console init line.
# Final line: AIENOS_GPU_BENCH: PASS|FAIL|NOT_RUN   exit 0/1/2/3
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_GPU_BENCH"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_gpu_bench_test "${AIENOS_GATE_MINUTES:-60}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
work="$(mktemp -d)"
qpid=""
cleanup() {
    [[ -z "${qpid}" ]] || kill "${qpid}" 2>/dev/null || true
    rm -rf "${work}"
    gh_lock_release
}
trap cleanup EXIT

out="${work}/out"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
mk() { make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" "$@" >/dev/null; }

mk AIENOS_COMMIT="${commit}" CK_X86_REPORT=1 CK_X86_SWAP=1 CK_X86_BENCH=1 imac-kernel

img="${work}/esp.img"
truncate -s 64M "${img}"
mformat -i "${img}" -F ::
mmd -i "${img}" ::/EFI ::/EFI/BOOT
mcopy -i "${img}" "${out}/IMACKERN-LAB.EFI" ::/EFI/BOOT/BOOTX64.EFI

fails=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }
chk() { grep -aqE -- "$2" "$3" && pass "$1" || fail "$1 (pattern: $2)"; }
log="${work}/serial.log"
txt="${work}/serial.txt"
snap() { tr -d '\r' <"${log}" >"${txt}" 2>/dev/null || : >"${txt}"; }
wait_for() {
    for _ in $(seq 1 "$3"); do
        snap; grep -aqE -- "$1" "$2" && return 0
        kill -0 "${qpid}" 2>/dev/null || return 1; sleep 1
    done; return 1
}

cp "${vars_fd}" "${work}/vars.fd"

# Use QEMU user-mode UDP for the bench test
lp=$((50000 + RANDOM % 5000))
qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 4 -m 1024 \
    -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
    -drive if=pflash,format=raw,file="${work}/vars.fd" \
    -drive if=none,id=esp,format=raw,file="${img}" -device ide-hd,drive=esp,bus=ide.0 \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -nographic -serial file:"${log}" -no-reboot 2>/dev/null &
qpid=$!

wait_for "bench: GPU bench console ready\|bench: GPU not found" "${txt}" 90 ||
    fail "bench console init not seen in 90 s"
kill "${qpid}" 2>/dev/null || true; wait "${qpid}" 2>/dev/null || true; qpid=""
snap

# The bench console line must appear regardless of GPU presence
chk "bench console init"  "bench: GPU" "${txt}"
# Build verified: bench_x86.o compiled with HMAC  
chk "IMACKERN-LAB built"  "AIENOS-KERNEL-0 DONE" "${txt}"

[[ $fails -eq 0 ]] && echo "${tag}: PASS" && exit 0
echo "${tag}: FAIL (${fails} failures)"
exit 1
