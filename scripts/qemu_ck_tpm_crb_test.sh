#!/usr/bin/env bash
# qemu_ck_tpm_crb_test.sh -- gate AIENOS_TPM_CRB_QEMU (L200): the TPM 2.0 CRB driver inside a
# booted C kernel (the x86_64 kernel, native/kernel/x86/tpm_x86.c) talking to QEMU's
# tpm-crb device with a real swtpm behind it. QEMU's aarch64 virt machine has only a
# tpm-tis device (no CRB), so the CRB driver can only be booted on q35; the same
# native/kernel/dev/tpm_crb.c is compiled into the aarch64 kernel build too (gate
# AIENOS_TPM_CRB_A64_BUILD in scripts/tpm_crb_a64_build_check.sh). QEMU qualifies nothing
# physical: a PASS says nothing about the Spark's TPM (NTC0702).
#
# Steps: swtpm is first started alone and tpm2-tools define NV index 0x01500016 (32 known
# bytes), then swtpm is restarted for QEMU (NV persists, the firmware's TPM2_Startup then
# runs as it does on the Spark); the kernel boots with OVMF, finds the ACPI TPM2 table
# (start method 7), opens the CRB, and prints the "tpm:" lines checked below against
# native/kernel/tests/fixtures/tpm_crb_qemu_expected.txt (hex values that depend on the
# firmware's measurements are normalized, then checked for shape). A second boot with no
# TPM device must print "tpm: none (no ACPI TPM2 table)" (negative control).
# Final line: AIENOS_TPM_CRB_QEMU: PASS|FAIL|NOT_RUN [(...)]   exit 0 / 1 / 2 (tools) / 3 (NOT_RUN)
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_TPM_CRB_QEMU"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 swtpm tpm2_nvdefine tpm2_nvwrite tpm2_startup mformat mmd mcopy xxd "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }
out="${IMACKERN_OUT:-${repo_root}/target/native-kernel-imac-kernel}"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" imac-kernel >/dev/null

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_tpm_crb_test "${AIENOS_GATE_MINUTES:-30}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
work="$(mktemp -d)"
qpid=""; spid=""
cleanup() {
    [[ -z "${qpid}" ]] || kill "${qpid}" 2>/dev/null || true
    [[ -z "${spid}" ]] || kill "${spid}" 2>/dev/null || true
    rm -rf "${work}"
    gh_lock_release
}
trap cleanup EXIT

img="${work}/esp.img"
truncate -s 64M "${img}"
mformat -i "${img}" -F ::
mmd -i "${img}" ::/EFI ::/EFI/BOOT
mcopy -i "${img}" "${out}/IMACKERN.EFI" ::/EFI/BOOT/BOOTX64.EFI

nv_hex="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
echo "${nv_hex}" | xxd -r -p >"${work}/nv.bin"
mkdir -p "${work}/state"
start_swtpm() { # extra swtpm flags...
    rm -f "${work}/data.sock" "${work}/data.sock.ctrl"
    swtpm socket --tpm2 --tpmstate dir="${work}/state" --ctrl type=unixio,path="${work}/data.sock.ctrl" \
        --log file="${work}/swtpm.log",level=1 "$@" &
    spid=$!
    for _ in $(seq 1 50); do [[ -S "${work}/data.sock.ctrl" ]] && return 0; sleep 0.1; done
    echo "FAIL  swtpm did not come up"; exit 1
}
# Prepare the NV index through a swtpm of our own, then stop it (state stays in ${work}/state).
start_swtpm --server type=unixio,path="${work}/data.sock" --flags not-need-init
for _ in $(seq 1 50); do [[ -S "${work}/data.sock" ]] && break; sleep 0.1; done
export TPM2TOOLS_TCTI="swtpm:path=${work}/data.sock"
tpm2_startup -c >/dev/null
tpm2_nvdefine 0x01500016 -C o -s 32 -a "ownerwrite|ownerread|authread|authwrite" >/dev/null
tpm2_nvwrite 0x01500016 -C o -i "${work}/nv.bin" >/dev/null
unset TPM2TOOLS_TCTI
kill "${spid}"; wait "${spid}" 2>/dev/null || true; spid=""

fails=0
chk() { # description, condition (ok|bad)
    if [[ "$2" == ok ]]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}
boot() { # name, extra qemu args...
    local name="$1"; shift
    cp "${vars_fd}" "${work}/vars.fd"
    qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 4 -m 1024 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/vars.fd" \
        -drive if=none,id=esp,format=raw,file="${img}" -device ide-hd,drive=esp,bus=ide.0 \
        "$@" -display none -nic none -serial file:"${work}/${name}.log" -no-reboot &
    qpid=$!
    local self_exit=0
    for _ in $(seq 1 "${AIENOS_TPM_CRB_WAIT:-300}"); do
        if ! kill -0 "${qpid}" 2>/dev/null; then self_exit=1; break; fi
        sleep 1
    done
    kill "${qpid}" 2>/dev/null || true
    wait "${qpid}" 2>/dev/null || true
    qpid=""
    tr -d '\r' <"${work}/${name}.log" >"${work}/${name}.txt" 2>/dev/null || : >"${work}/${name}.txt"
    chk "${name}: QEMU powered off by itself" "$([[ ${self_exit} == 1 ]] && echo ok)"
    chk "${name}: kernel alive, no panic" "$(grep -aq '^kernel: alive$' "${work}/${name}.txt" && ! grep -aqE '^panic' "${work}/${name}.txt" && echo ok)"
}

# ---- boot 1: TPM present (tpm-crb + swtpm) ----
start_swtpm
boot tpm -chardev socket,id=chrtpm,path="${work}/data.sock.ctrl" -tpmdev emulator,id=tpm0,chardev=chrtpm -device tpm-crb,tpmdev=tpm0
kill "${spid}" 2>/dev/null || true; wait "${spid}" 2>/dev/null || true; spid=""
# keep only the driver's lines; replace values that depend on the firmware's measurements
grep -a '^tpm: ' "${work}/tpm.txt" \
    | sed -E 's/(sha256=)[0-9a-f]{64}$/\1<sha256>/' >"${work}/tpm.norm" || true
if diff -u native/kernel/tests/fixtures/tpm_crb_qemu_expected.txt "${work}/tpm.norm" >"${work}/tpm.diff"; then
    chk "tpm: driver output equals fixture" ok
else
    chk "tpm: driver output equals fixture" bad; head -30 "${work}/tpm.diff"
fi
p0="$(sed -nE 's/^tpm: pcr0 rc=0 sha256=([0-9a-f]{64})$/\1/p' "${work}/tpm.txt")"
p7="$(sed -nE 's/^tpm: pcr7 rc=0 sha256=([0-9a-f]{64})$/\1/p' "${work}/tpm.txt")"
zero="$(printf '%064d' 0)"
chk "tpm: PCR0 is a 64-hex digest, not all zero (the firmware measured into it)" "$([[ -n "${p0}" && "${p0}" != "${zero}" ]] && echo ok)"
chk "tpm: PCR7 is a 64-hex digest and differs from PCR0" "$([[ -n "${p7}" && "${p7}" != "${p0}" ]] && echo ok)"
chk "tpm: NV read returned exactly the bytes tpm2-tools wrote" "$(grep -aqx "tpm: nv_read 0x1500016 rc=0x0 data=${nv_hex}" "${work}/tpm.txt" && echo ok)"

# ---- boot 2: no TPM device (negative control: the same kernel must say none and touch nothing) ----
boot notpm
chk "notpm: kernel reports no TPM" "$(grep -aqx 'tpm: none (no ACPI TPM2 table)' "${work}/notpm.txt" && echo ok)"
chk "notpm: no other tpm line" "$([[ "$(grep -ac '^tpm: ' "${work}/notpm.txt")" == 1 ]] && echo ok)"

echo "---- tpm serial (tpm: lines) ----"; cat "${work}/tpm.norm"; echo "----"
if [[ ${fails} == 0 ]]; then echo "${tag}: PASS"; exit 0; fi
echo "${tag}: FAIL (${fails} checks)"; exit 1
