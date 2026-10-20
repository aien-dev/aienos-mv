#!/usr/bin/env bash
# qemu_ck_imac_swap_test.sh -- QEMU test for the AIENOS live kernel swap.
# QEMU is an emulator: a PASS here qualifies nothing about a 2017 iMac.
#
# Builds kernel A (commit=...-A) and kernel B (commit=...-B), wraps kernel B
# in a test-signed slot object placed in the ESP FAT image, boots kernel A
# in QEMU with CK_X86_SWAP_TEST_DISK=1 (reads the slot from the ESP rather
# than waiting for UDP), and confirms:
#   1. Kernel A's commit appears.
#   2. swap: verified ok appears.
#   3. Kernel B's commit appears after kernel A's DONE line.
#   4. Only one "AIENOS-KERNEL-0" firmware entry (no reset).
#   5. Bad-signature slot rejected.
# Final line: AIENOS_IMAC_SWAP: PASS|FAIL|NOT_RUN   exit 0/1/2/3
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_IMAC_SWAP"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy python3 "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_imac_swap_test "${AIENOS_GATE_MINUTES:-60}"; then
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

# Build kernels A and B
mk AIENOS_COMMIT="${commit}-A" CK_X86_REPORT=1 CK_X86_SWAP=1 CK_X86_BENCH=1 \
   CK_X86_SWAP_TEST_DISK=1 imac-kernel
cp "${out}/IMACKERN-LAB.EFI" "${work}/KERNA.EFI"

mk AIENOS_COMMIT="${commit}-B" CK_X86_REPORT=1 CK_X86_SWAP=1 CK_X86_BENCH=1 imac-kernel
cp "${out}/IMACKERN-LAB.EFI" "${work}/KERNB.EFI"

# Wrap kernel B in a test-signed slot manifest (python, no external tool)
python3 - "${work}/KERNB.EFI" "${work}/KERNB_SLOT.bin" << 'PYEOF'
import sys, struct, hashlib
efi_path, out_path = sys.argv[1], sys.argv[2]
with open(efi_path, 'rb') as f:
    efi = f.read()
manifest = bytearray(4096)
manifest[0:8] = b'AIENSLT1'
manifest[8] = 1
struct.pack_into('<I', manifest, 16, len(efi))
manifest[20] = 1
struct.pack_into('<I', manifest, 192, 1)
struct.pack_into('<I', manifest, 196, 1)
with open(out_path, 'wb') as f:
    f.write(bytes(manifest))
    f.write(efi)
print(f"slot object {len(manifest)+len(efi)} bytes")
PYEOF

# Bad-signature copy: flip one byte in the signature region
python3 -c "
with open('${work}/KERNB_SLOT.bin','rb') as f: d=bytearray(f.read())
d[128]^=0x01
with open('${work}/KERNB_BAD.bin','wb') as f: f.write(d)
"

# ESP with kernel A as boot loader and swap inbox
img="${work}/esp.img"
truncate -s 128M "${img}"
mformat -i "${img}" -F ::
mmd -i "${img}" ::/EFI ::/EFI/BOOT ::/EFI/AIENOS
mcopy -i "${img}" "${work}/KERNA.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "${img}" "${work}/KERNB_SLOT.bin" ::/EFI/AIENOS/SWAP_INBOX.BIN

data="${work}/data.img"
truncate -s 64M "${data}"

fails=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }
chk() { grep -aqE -- "$2" "$3" && pass "$1" || fail "$1 (pattern: $2)"; }
log="${work}/serial.log"
txt="${work}/serial.txt"
snap() { tr -d '\r' <"${log}" >"${txt}" 2>/dev/null || : >"${txt}"; }
wait_for() {
    for _ in $(seq 1 "$3"); do
        snap
        grep -aqE -- "$1" "$2" && return 0
        kill -0 "${qpid}" 2>/dev/null || return 1
        sleep 1
    done; return 1
}

cp "${vars_fd}" "${work}/vars.fd"
qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 4 -m 1024 \
    -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
    -drive if=pflash,format=raw,file="${work}/vars.fd" \
    -drive if=none,id=esp,format=raw,file="${img}" -device ide-hd,drive=esp,bus=ide.0 \
    -drive if=none,id=data,format=raw,file="${data}" -device ide-hd,drive=data,bus=ide.1 \
    -nographic -serial file:"${log}" -no-reboot 2>/dev/null &
qpid=$!

wait_for "AIENOS-KERNEL-0 DONE" "${txt}" 90 ||
    fail "kernel A DONE not seen in 90 s"
wait_for "${commit}-B.*AIENOS-KERNEL-0 DONE|AIENOS-KERNEL-0 DONE.*${commit}-B" \
         "${txt}" 120 || fail "kernel B DONE not seen"
kill "${qpid}" 2>/dev/null || true; wait "${qpid}" 2>/dev/null || true; qpid=""
snap

chk "kernel-A commit" "${commit}-A"     "${txt}"
chk "kernel-B commit" "${commit}-B"     "${txt}"
chk "swap verified"   "swap: verified"  "${txt}"
# Count firmware entries: only 1 allowed (no reset)
cnt=$(grep -cE "^AIENOS-KERNEL-0$" "${txt}" 2>/dev/null || echo 0)
[[ "${cnt}" -le 1 ]] && pass "single firmware entry" || fail "multiple firmware entries (${cnt})"

# Bad signature test
img2="${work}/esp2.img"; cp "${img}" "${img2}"
mcopy -i "${img2}" -o "${work}/KERNB_BAD.bin" ::/EFI/AIENOS/SWAP_INBOX.BIN
log2="${work}/serial2.log"; txt2="${work}/serial2.txt"
snap2() { tr -d '\r' <"${log2}" >"${txt2}" 2>/dev/null || : >"${txt2}"; }
cp "${vars_fd}" "${work}/vars2.fd"
qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 4 -m 1024 \
    -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
    -drive if=pflash,format=raw,file="${work}/vars2.fd" \
    -drive if=none,id=esp,format=raw,file="${img2}" -device ide-hd,drive=esp,bus=ide.0 \
    -nographic -serial file:"${log2}" -no-reboot 2>/dev/null &
qpid=$!
for _ in $(seq 1 90); do
    snap2; grep -aqE "REJECTED|swap:.*REJECTED" "${txt2}" && break
    kill -0 "${qpid}" 2>/dev/null || break; sleep 1
done
kill "${qpid}" 2>/dev/null || true; wait "${qpid}" 2>/dev/null || true; qpid=""
snap2
chk "bad-sig rejected" "REJECTED\|swap: Ed25519 signature REJECTED" "${txt2}"

[[ $fails -eq 0 ]] && echo "${tag}: PASS" && exit 0
echo "${tag}: FAIL (${fails} failures)"
exit 1
