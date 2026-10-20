#!/usr/bin/env bash
# tpm_crb_swtpm_test.sh -- gate AIENOS_TPM_CRB_SWTPM (L200): the C kernel's TPM 2.0 CRB
# driver (native/kernel/dev/tpm_crb.c) against a REAL TPM 2.0 implementation, swtpm.
#
# The driver runs on the host over a register-level simulator of the PTP CRB window
# (native/kernel/tests/test_tpm_crb.c, "swtpm" mode); every command the driver writes into
# the simulated CRB buffer and starts is forwarded to swtpm's unix socket, and swtpm's
# answer comes back through the simulated response buffer. Two runs, each with a fresh swtpm:
#   fresh    nothing started the TPM: TPM2_Startup(CLEAR) must return success (rc=0x0).
#   started  the firmware already started the TPM (what OVMF and the Spark's firmware do), a
#            PCR is extended and an NV index written with tpm2-tools: the driver's Startup
#            must return TPM_RC_INITIALIZE (0x100), PCR_Read and NV_Read must return exactly
#            what tpm2-tools reads independently.
# Output is diffed against native/kernel/tests/fixtures/tpm_crb_swtpm_{fresh,started}_expected.txt.
# Nothing here touches a real TPM or the Spark's /dev/tpm*. Final line:
#     AIENOS_TPM_CRB_SWTPM: PASS|FAIL|NOT_RUN [(...)]     exit 0 / 1 / 2 (tools missing)
set -uo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_TPM_CRB_SWTPM"
for t in swtpm tpm2_startup tpm2_nvdefine tpm2_nvwrite tpm2_nvread tpm2_pcrextend tpm2_pcrread xxd sha256sum; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
out="${TPM_CRB_OUT:-${repo_root}/target/native-kernel-tpm-crb}"
fix="${repo_root}/native/kernel/tests/fixtures"
make -s -C native/kernel OUT="${out}" test-tpm-crb >"${out}.build.log" 2>&1 || {
    echo "${tag}: FAIL (build; see ${out}.build.log)"; tail -5 "${out}.build.log"; exit 1; }
bin="${out}/host/test_tpm_crb"
work="$(mktemp -d)"
spid=""
cleanup() { [[ -z "${spid}" ]] || kill "${spid}" 2>/dev/null || true; rm -rf "${work}"; }
trap cleanup EXIT
fails=0

start_swtpm() { # name
    mkdir -p "${work}/$1-state"
    swtpm socket --tpm2 --tpmstate dir="${work}/$1-state" \
        --server type=unixio,path="${work}/$1.sock" --ctrl type=unixio,path="${work}/$1.sock.ctrl" \
        --flags not-need-init --log file="${work}/$1.log",level=1 &
    spid=$!
    for _ in $(seq 1 50); do [[ -S "${work}/$1.sock" ]] && return 0; sleep 0.1; done
    echo "FAIL  swtpm $1 did not come up"; fails=$((fails + 1)); return 1
}
stop_swtpm() { kill "${spid}" 2>/dev/null || true; wait "${spid}" 2>/dev/null || true; spid=""; }
chk() { # description, condition result
    if [[ "$2" == ok ]]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}

# ---- run 1: fresh TPM ----
start_swtpm fresh && {
    "${bin}" swtpm "${work}/fresh.sock" fresh >"${work}/fresh.out" 2>&1
    stop_swtpm
    if diff -u "${fix}/tpm_crb_swtpm_fresh_expected.txt" "${work}/fresh.out" >"${work}/fresh.diff"; then
        chk "fresh swtpm output equals fixture" ok
    else
        chk "fresh swtpm output equals fixture" bad; head -20 "${work}/fresh.diff"
    fi
}

# ---- run 2: firmware already started the TPM ----
start_swtpm started && {
    export TPM2TOOLS_TCTI="swtpm:path=${work}/started.sock"
    nv_hex="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    ext_hex="1111111111111111111111111111111111111111111111111111111111111111"
    echo "${nv_hex}" | xxd -r -p >"${work}/nv.bin"
    tpm2_startup -c >/dev/null 2>&1
    tpm2_nvdefine 0x01500016 -C o -s 32 -a "ownerwrite|ownerread|authread|authwrite" >/dev/null 2>&1
    tpm2_nvwrite 0x01500016 -C o -i "${work}/nv.bin" >/dev/null 2>&1
    tpm2_pcrextend "16:sha256=${ext_hex}" >/dev/null 2>&1
    want_pcr16="$( (printf '%064d' 0 | xxd -r -p; echo "${ext_hex}" | xxd -r -p) | sha256sum | cut -d' ' -f1)"
    tool_pcr16="$(tpm2_pcrread sha256:16 2>/dev/null | sed -nE 's/^ *16 *: *0x([0-9A-Fa-f]{64}).*/\1/p' | tr 'A-F' 'a-f')"
    tool_nv="$(tpm2_nvread 0x01500016 -C o -s 32 2>/dev/null | xxd -p -c 64)"
    chk "tpm2-tools PCR16 equals sha256(zeros||extend) computed here" "$([[ "${tool_pcr16}" == "${want_pcr16}" ]] && echo ok)"
    chk "tpm2-tools NV read equals what was written" "$([[ "${tool_nv}" == "${nv_hex}" ]] && echo ok)"
    unset TPM2TOOLS_TCTI
    "${bin}" swtpm "${work}/started.sock" started >"${work}/started.out" 2>&1
    stop_swtpm
    if diff -u "${fix}/tpm_crb_swtpm_started_expected.txt" "${work}/started.out" >"${work}/started.diff"; then
        chk "started swtpm output equals fixture" ok
    else
        chk "started swtpm output equals fixture" bad; head -20 "${work}/started.diff"
    fi
    chk "driver PCR16 equals the independently computed value" "$(grep -q "^pcr16: rc=0 ${want_pcr16}$" "${work}/started.out" && echo ok)"
    chk "driver NV read equals what tpm2-tools wrote" "$(grep -q "auth=index): rc=0x0 ${nv_hex}$" "${work}/started.out" && echo ok)"
}

if [[ "${fails}" -eq 0 ]]; then echo "${tag}: PASS"; exit 0; fi
echo "${tag}: FAIL (${fails} checks)"
exit 1
