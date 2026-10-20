#!/usr/bin/env bash
# qemu_ck_imac_swap_test.sh -- QEMU test for the AIENOS live kernel swap (disk-staged path).
# QEMU is an emulator: a PASS here qualifies nothing about a real iMac. The UDP receiver is
# not built (swap_x86.c: the UDP path is a stub); this test covers the verify, quiesce and
# jump path with the slot object staged on a disk.
#
# Builds lab kernel A (commit=<sha>-A, with the disk-staged swap receiver) and kernel B
# (commit=<sha>-B), both with the RFC 8032 TEST owner key (CK_TEST_SLOT_KEY=1: QEMU only,
# never for hardware). B is wrapped as a signed slot object by ck_slot_tool and written raw at
# LBA 2048 of a third disk. Boot A: A teaches Allen one fact (saves Allen's memory), verifies B
# with ck_slot_verify (the NETLDR verifier), quiesces and jumps into B in the same QEMU process.
# A second run stages a bad-signature object and must refuse it.
#
# Checks:
#   1   kernel A boots (commit -A) and saves Allen's memory
#   1b  A saves Allen's memory before the swap
#   2   A reads the staged slot object
#   3   the swap verifies with ck_slot_verify ("swap: verified ok") and jumps
#   4   B boots (commit -B) and reaches DONE, after the jump
#   5   no reset: the QEMU process that started A is still running with B up (-no-reboot
#       would have ended it on a guest reset)
#   6   Allen's memory is intact: B loads the generation A saved, with the name A taught
#   7   a bad signature is refused: "REJECTED (signature does not verify)", no jump, A keeps running
#   8   the verify path is the netboot one: swap_x86.c calls ck_slot_verify, net_loader.c calls
#       it too, and swap_x86.c has no direct aienos_ed25519_verify (no copy of the check)
#   9   the default image (CK_X86_REPORT=0) links no swap receiver
# Final line: AIENOS_IMAC_SWAP: PASS|FAIL|NOT_RUN   exit 0/1/2/3
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_IMAC_SWAP"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy truncate od dd "${cross}gcc" "${cross}ld" "${cross}nm"; do
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
outdef="${work}/outdef"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
# mkimg OUTDIR COMMIT-SUFFIX [make vars...]: builds imac-kernel into OUTDIR
mkimg() {
    local od="$1" suffix="$2"; shift 2
    make -s -C native/kernel X86_CROSS="${cross}" OUT="${od}" AIENOS_COMMIT="${commit}${suffix}" "$@" imac-kernel >/dev/null
}
A_WAIT="${AIENOS_SWAP_A_WAIT:-240}"
B_WAIT="${AIENOS_SWAP_B_WAIT:-300}"
fails=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }

# ---- builds ----
mkimg "${out}" -A CK_X86_REPORT=1 CK_TEST_SLOT_KEY=1 CK_X86_SWAP_TEST_DISK=1
cp "${out}/IMACKERN-LAB.EFI" "${work}/KERNA.EFI"
mkimg "${out}" -B CK_X86_REPORT=1 CK_TEST_SLOT_KEY=1
cp "${out}/IMACKERN-LAB.EFI" "${work}/KERNB.EFI"
mkimg "${outdef}" -D
make -s -C native/kernel OUT="${out}" slot-tool >/dev/null
SLOT="${out}/host/ck_slot_tool"

# ---- the slot object for B: manifest unit signed with the TEST key, then the kernel ----
"${SLOT}" make-manifest --image "${work}/KERNB.EFI" --generation 2 --policy-generation 2 \
    --out "${work}/M.bin" >/dev/null
"${SLOT}" sign-test "${work}/M.bin" --out "${work}/MS.bin" >/dev/null
"${SLOT}" verify "${work}/MS.bin" --image "${work}/KERNB.EFI" --test-key >/dev/null
cat "${work}/MS.bin" "${work}/KERNB.EFI" > "${work}/SLOT_B.bin"
# a copy with one signature byte flipped (the signature is manifest bytes 448..511; byte 458 is inside)
cp "${work}/SLOT_B.bin" "${work}/SLOT_BAD.bin"
b=$(od -An -tu1 -j458 -N1 "${work}/SLOT_BAD.bin" | tr -d ' ')
printf "\\$(printf '%03o' $((b ^ 1)))" | dd of="${work}/SLOT_BAD.bin" bs=1 seek=458 conv=notrunc 2>/dev/null

# disks: ESP (kernel A as BOOTX64.EFI), data (Allen's memory), swap (slot object raw at LBA 2048)
mkesp() {
    local img="$1"
    truncate -s 128M "${img}"
    mformat -i "${img}" -F ::
    mmd -i "${img}" ::/EFI ::/EFI/BOOT
    mcopy -i "${img}" "${work}/KERNA.EFI" ::/EFI/BOOT/BOOTX64.EFI
}
mkswap() { # image slot-object
    truncate -s 16M "$1"
    dd if="$2" of="$1" bs=512 seek=2048 conv=notrunc 2>/dev/null
}
esp="${work}/esp.img"; mkesp "${esp}"
data="${work}/data.img"; truncate -s 64M "${data}"
swp="${work}/swap.img"; mkswap "${swp}" "${work}/SLOT_B.bin"

# boot ESP DATA SWAP SERIAL-LOG: starts QEMU in the background, sets qpid
boot() {
    local v="${work}/vars-$RANDOM$RANDOM.fd"
    cp "${vars_fd}" "${v}"
    qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 4 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${v}" \
        -drive if=none,id=esp,format=raw,file="$1" -device ide-hd,drive=esp,bus=ide.0 \
        -drive if=none,id=data,format=raw,file="$2" -device ide-hd,drive=data,bus=ide.1 \
        -drive if=none,id=swap,format=raw,file="$3" -device ide-hd,drive=swap,bus=ide.2 \
        -nographic -serial file:"$4" -no-reboot 2>/dev/null &
    qpid=$!
}
# wait_for REGEX FILE SECONDS: 0 when REGEX shows up in FILE; stops early if QEMU has exited
wait_for() {
    local i
    for ((i = 0; i < $3; i++)); do
        grep -aqE -- "$1" "$2" 2>/dev/null && return 0
        kill -0 "${qpid}" 2>/dev/null || { grep -aqE -- "$1" "$2" 2>/dev/null && return 0; return 1; }
        sleep 1
    done
    return 1
}
stop_qemu() { kill "${qpid}" 2>/dev/null || true; wait "${qpid}" 2>/dev/null || true; qpid=""; }

# ---- run 1: the good swap ----
logA="${work}/serialA.log"
boot "${esp}" "${data}" "${swp}" "${logA}"
wait_for "AIENOS-KERNEL-0 DONE" "${logA}" "${A_WAIT}" || true
if grep -aq -- "${commit}-A" "${logA}" && grep -aq "AIENOS-KERNEL-0 DONE" "${logA}"; then
    pass "1  kernel A boots (commit -A)"
else
    fail "1  kernel A boots (commit -A): DONE not seen in ${A_WAIT} s"
fi
if grep -aq "allen_memory: saved generation" "${logA}"; then
    pass "1b A saves Allen's memory before the swap"
else
    fail "1b A saves Allen's memory before the swap"
fi
if grep -aq "swap: inbox at LBA 2048" "${logA}"; then pass "2  A reads the staged slot object"; else fail "2  A reads the staged slot object"; fi
wait_for "swap: jumping to new kernel|swap: REJECTED" "${logA}" "${B_WAIT}" || true
if grep -aq "swap: verified ok" "${logA}"; then pass "3  swap verifies with ck_slot_verify"; else fail "3  swap verifies"; fi
if grep -aq "swap: jumping to new kernel" "${logA}"; then pass "3b swap jumps to B"; else fail "3b swap jumps to B"; fi

jump_line="$(grep -an "swap: jumping to new kernel" "${logA}" | head -1 | cut -d: -f1 || true)"
GA="$(grep -a "allen_memory: saved generation" "${logA}" | tail -1 | sed -n 's/.*saved generation \([0-9][0-9]*\).*/\1/p' || true)"
after="${work}/after_jump.log"
if [[ -n "${jump_line}" ]]; then tail -n +"$((jump_line + 1))" "${logA}" > "${after}"; else : > "${after}"; fi

wait_for "AIENOS-KERNEL-0 DONE" "${after}" "${B_WAIT}" || true
sleep 2
if grep -aq -- "${commit}-B" "${after}" && grep -aq "AIENOS-KERNEL-0 DONE" "${after}"; then
    pass "4  kernel B boots (commit -B) and reaches DONE after the jump"
else
    fail "4  kernel B boots (commit -B) and reaches DONE after the jump"
fi
if kill -0 "${qpid}" 2>/dev/null; then
    pass "5  no reset: the QEMU process that booted A is still running with B up"
else
    fail "5  no reset: QEMU ended (guest reset or crash)"
fi
stop_qemu

if [[ -n "${GA}" ]] && grep -aq "allen_memory: loaded generation ${GA} " "${after}" && grep -aq "name Ada" "${after}"; then
    pass "6  Allen's memory intact: B loads generation ${GA} with the name A taught"
else
    fail "6  Allen's memory intact: A saved generation '${GA:-none}', B's load line: $(grep -a 'loaded generation' "${after}" | head -1 || echo none)"
fi

# ---- run 2: the bad signature must be refused ----
badesp="${work}/esp_bad.img"; mkesp "${badesp}"
badswp="${work}/swap_bad.img"; mkswap "${badswp}" "${work}/SLOT_BAD.bin"
badata="${work}/data_bad.img"; truncate -s 64M "${badata}"
logR="${work}/serialR.log"
boot "${badesp}" "${badata}" "${badswp}" "${logR}"
wait_for "swap: REJECTED" "${logR}" "${B_WAIT}" || true
sleep 5
if grep -aq "swap: REJECTED (signature does not verify)" "${logR}"; then
    pass "7a bad signature refused: REJECTED (signature does not verify)"
else
    fail "7a bad signature refused: $(grep -a 'swap:' "${logR}" | tail -2 | tr '\n' ' ' || echo 'no swap line')"
fi
if grep -aq "swap: jumping to new kernel" "${logR}"; then
    fail "7b bad signature: the swap jumped anyway"
else
    pass "7b bad signature: no jump"
fi
if grep -aq "AIENOS-KERNEL-0 DONE" "${logR}" && kill -0 "${qpid}" 2>/dev/null; then
    pass "7c A keeps running after the refusal"
else
    fail "7c A keeps running after the refusal"
fi
stop_qemu

# ---- source and image checks ----
if grep -q "ck_slot_verify(" native/kernel/x86/swap_x86.c \
   && ! grep -q "aienos_ed25519_verify" native/kernel/x86/swap_x86.c \
   && grep -q "ck_slot_verify(m, obj" native/boot/net_loader.c; then
    pass "8  swap verifies through ck_slot_verify, the netboot function (swap_x86.c; net_loader.c:578)"
else
    fail "8  swap verify path is not the netboot function"
fi
if [[ -f "${outdef}/imackern/imackern.elf" ]] \
   && ! "${cross}nm" "${outdef}/imackern/imackern.elf" \
      | grep -qE ' [TtDdBbRr] (ck_slot_verify|aienos_ed25519_verify|ck_x86_swap_tramp|ck_x86_swap_read_inbox)$'; then
    pass "9  the default image (CK_X86_REPORT=0) links no swap receiver"
else
    fail "9  the default image links a swap receiver symbol"
fi

[[ ${fails} -eq 0 ]] && echo "${tag}: PASS" && exit 0
echo "${tag}: FAIL (${fails} failures)"
exit 1
