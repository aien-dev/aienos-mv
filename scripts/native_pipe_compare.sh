#!/usr/bin/env bash
# native_pipe_compare.sh: native AIENOS pipelines next to Linux and Omega-on-Linux on the same workload (aienos-mv#432).
# It reports measured numbers only: no ratio, no "faster" claim, and every row says where it ran.
#
# Workload (core/pipe_bench.h pb_flow, the same on every path): gen [| xform key 0x5A] | sink, TOTAL bytes produced
# 4096 per step (every fifth byte NUL), each stage reading up to 4096 bytes per step, the sink returning the count and
# FNV-1a of what it received; the bytes and the hash are checked on every run of every path. Sizes 163840 and 1048576
# with 2 and 3 stages, plus a one-byte run (end-to-end latency). Paths:
#   aienos_qemu      native AIENOS: admitted Omega stages (tgen, txform, tsink) as EL0 steps over native pipes and tasks,
#                    one emulated CPU, QEMU TCG on this machine. From the pipe_bench lines of a qemu_ck_task_test.sh
#                    serial log (--qemu-log FILE), or from a fresh run of that script. Wall time only (in-guest counter).
#   aienos_host      the same pipe and task code (core/pipe.c, core/task.c) with C stand-in stages on this machine's CPU,
#                    no kernel, no EL0 (make -C native/kernel test_task, test_task --bench). Wall time only.
#   linux_c          Linux processes joined by pipe(2), C stages with the same stage ABI and semantics
#                    (native/kernel/tests/compare/np_linux.c, mode c), pinned to one CPU like the AIENOS path.
#   linux_omega      the same, but every stage is the .osc source of the AIENOS fixture compiled by Omega (oscc front
#                    end and code generator of --omega DIR) and called as native code; compiled before the clock starts.
# Matched: workload, bytes, chunk sizes, stage semantics, one CPU; the Omega paths run the same .osc sources.
# Not matched: the hardware. aienos_qemu runs on an emulated CPU; the other paths on the real CPU of this machine. The row
# that would match it (Linux inside the same QEMU) is reported NOT_RUN: this machine has no readable AArch64 Linux kernel
# image to boot. Energy is NOT_MEASURED on every path (no power meter is read). Nothing here is a hardware result.
#
# Usage: bash scripts/native_pipe_compare.sh [--omega DIR] [--qemu-log FILE] [--runs N] [--out FILE]
#   --omega DIR     an aien-dev/omega checkout (src/compiler); without it linux_omega is NOT_RUN
#   --qemu-log FILE use this serial log instead of booting QEMU (it must hold pipe_bench lines)
#   --runs N        repeats per Linux and host scenario (default 5, at most 9)
#   --out FILE      also write the table as tab-separated values
# Exit: 0 every path that ran was correct, 1 a path gave wrong bytes or failed, 2 usage error.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
kdir="${repo_root}/native/kernel"
omega="" qlog="" runs=5 out=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --omega) omega="${2:?}"; shift ;;
        --qemu-log) qlog="${2:?}"; shift ;;
        --runs) runs="${2:?}"; shift ;;
        --out) out="${2:?}"; shift ;;
        -h|--help) sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "native_pipe_compare: unknown argument $1" >&2; exit 2 ;;
    esac
    shift
done
[[ "${runs}" =~ ^[1-9]$ ]] || { echo "native_pipe_compare: --runs must be 1..9" >&2; exit 2; }
work="$(mktemp -d "${TMPDIR:-/tmp}/np-compare.XXXXXX")" || exit 2
trap 'rm -rf --one-file-system "${work}"' EXIT
bad=0
rows=()
row() { rows+=("$(printf '%s\t' "$@")"); }
field() { sed -nE "s/.* $1=([^ ;]+).*/\\1/p" <<<"$2"; }

# CPU pinning: one CPU, like the single-CPU AIENOS path. The last online CPU, to stay off CPU 0.
pin=()
if command -v taskset >/dev/null; then cpu=$(( $(nproc) - 1 )); pin=(taskset -c "${cpu}"); pin_note="pinned to CPU ${cpu}"
else pin_note="NOT pinned (taskset missing)"; fi

echo "native pipeline comparison at $(git -C "${repo_root}" rev-parse --short=12 HEAD) on $(uname -srm); measured numbers only"
echo "cc: $(cc --version | head -1); qemu: $(qemu-system-aarch64 --version 2>/dev/null | head -1 || echo 'not installed')"

# ---- linux_c and linux_omega
src="${kdir}/tests/compare/np_linux.c"
if cc -std=gnu11 -O2 -Wall -Wextra -Werror -o "${work}/np_c" "${src}" 2>"${work}/cc_c.log"; then have_c=1; else have_c=0; cat "${work}/cc_c.log" >&2; bad=1; fi
have_o=0 o_why="no --omega checkout given"
if [[ -n "${omega}" ]]; then
    if [[ -d "${omega}/src/compiler" ]]; then
        mapfile -t olib < <(ls "${omega}"/src/compiler/*.c | grep -v '/oscc_main\.c$')
        if cc -std=gnu11 -O2 -DNP_OMEGA -I"${omega}/src/compiler" -I"${omega}/src" -o "${work}/np_o" "${src}" "${olib[@]}" \
            "${omega}/src/sha256.c" 2>"${work}/cc_o.log"; then
            have_o=1; o_rev="$(git -C "${omega}" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
        else o_why="Omega build failed (see ${work}/cc_o.log)"; fi
    else o_why="${omega} has no src/compiler"; fi
fi
fix="${kdir}/tests/fixtures/task"
for nst in 2 3; do
    for total in 163840 1048576 1; do
        [[ ${total} == 1 && ${nst} == 3 ]] && continue
        w="stages=${nst} bytes=${total}"
        if [[ ${have_c} == 1 ]]; then
            l="$("${pin[@]}" "${work}/np_c" c "${nst}" "${total}" "${runs}")" || bad=1
            row "${w}" linux_c "$(field wall_us_median "${l}")" "$(field bytes_per_s_median "${l}")" "$(field cpu_us_median "${l}")" NOT_MEASURED \
                "Linux $(uname -r), C stages, ${pin_note}, bad_runs=$(field bad_runs "${l}")"
        fi
        if [[ ${have_o} == 1 ]]; then
            l="$("${pin[@]}" "${work}/np_o" omega "${nst}" "${total}" "${runs}" "${fix}")" || bad=1
            row "${w}" linux_omega "$(field wall_us_median "${l}")" "$(field bytes_per_s_median "${l}")" "$(field cpu_us_median "${l}")" NOT_MEASURED \
                "Linux $(uname -r), Omega ${o_rev} oscc native code of the fixture .osc, ${pin_note}, bad_runs=$(field bad_runs "${l}")"
        else
            row "${w}" linux_omega NOT_RUN NOT_RUN NOT_RUN NOT_MEASURED "${o_why}"
        fi
    done
done

# ---- aienos_host: test_task --bench (same pipe and task code, C stand-in stages)
if nice make -s -C "${kdir}" OUT="${work}/kout" "${work}/kout/host/test_task" >"${work}/make.log" 2>&1; then
    "${pin[@]}" "${work}/kout/host/test_task" --bench >"${work}/host.txt" 2>&1 || bad=1
else bad=1; echo "native_pipe_compare: test_task build failed" >&2; fi
# ---- aienos_qemu: the pipe_bench lines of the QEMU task gate
if [[ -z "${qlog}" ]]; then
    echo "--- booting QEMU: scripts/qemu_ck_task_test.sh (takes the QEMU gate lock)" >&2
    mkdir -p "${work}/qlogs"
    AIENOS_LOG_DIR="${work}/qlogs" AIENOS_CK_OUT_TASK="${work}/q_task" AIENOS_CK_OUT_PROD="${work}/q_prod" \
        bash "${repo_root}/scripts/qemu_ck_task_test.sh" >"${work}/qemu.txt" 2>&1 </dev/null
    echo "--- $(tail -1 "${work}/qemu.txt")" >&2
    qlog="${work}/qlogs/qemu_ck_task_serial.log"
fi
q_why="no pipe_bench lines in the QEMU log (QEMU missing, gate lock held or run failed)"
for nst in 2 3; do
    for total in 163840 1048576 1; do
        [[ ${total} == 1 && ${nst} == 3 ]] && continue
        w="stages=${nst} bytes=${total}"
        if [[ ${total} == 1 ]]; then c=tiny_one_byte; else c="$([[ ${nst} == 2 ]] && echo two || echo three)_stage_${total}"; fi
        l="$(grep -E "^pipe_bench: case=${c} " "${work}/host.txt" 2>/dev/null)"
        if [[ -n "${l}" ]]; then
            [[ "${l}" == *"result=PASS"* ]] || bad=1
            row "${w}" aienos_host "$(field wall_us_median "${l}")" "$(field bytes_per_s_median "${l}")" NOT_MEASURED NOT_MEASURED \
                "this CPU, AIENOS pipe and task code, C stand-in stages, no kernel, -O1, ${pin_note}"
        else row "${w}" aienos_host NOT_RUN NOT_RUN NOT_MEASURED NOT_MEASURED "test_task --bench gave no line"; fi
        l="$(grep -E "^pipe_bench: case=${c} " "${qlog}" 2>/dev/null)"
        if [[ -n "${l}" ]]; then
            [[ "${l}" == *"result=PASS"* ]] || bad=1
            row "${w}" aienos_qemu "$(field wall_us_median "${l}")" "$(field bytes_per_s_median "${l}")" NOT_MEASURED NOT_MEASURED \
                "QEMU TCG emulated CPU (1 vCPU) on this machine, admitted Omega stages, EL0 per step; emulator numbers, runs=$(field runs "${l}")"
        else row "${w}" aienos_qemu NOT_RUN NOT_RUN NOT_MEASURED NOT_MEASURED "${q_why}"; fi
    done
done
row "all" linux_in_qemu NOT_RUN NOT_RUN NOT_RUN NOT_MEASURED \
    "the hardware-matched Linux row (Linux in the same QEMU) is not built: no readable AArch64 Linux kernel image here"

echo
printf '%-22s %-13s %16s %16s %14s %13s  %s\n' workload path wall_us_median bytes_per_s cpu_us energy "where and how"
for r in "${rows[@]}"; do
    IFS=$'\t' read -r a b c d e f g <<<"${r}"
    printf '%-22s %-13s %16s %16s %14s %13s  %s\n' "${a}" "${b}" "${c}" "${d}" "${e}" "${f}" "${g}"
done
if [[ -n "${out}" ]]; then
    { printf 'workload\tpath\twall_us_median\tbytes_per_s_median\tcpu_us_median\tenergy\tlabel\n'; printf '%s\n' "${rows[@]}" | sed 's/\t$//'; } >"${out}"
fi
echo
echo "Not a speed claim: aienos_qemu runs on an emulated CPU, the other rows on the real CPU; compare rows only within the same hardware."
if [[ ${bad} == 0 ]]; then echo "NATIVE_PIPE_COMPARE: DONE (every path that ran gave exact bytes)"; exit 0; fi
echo "NATIVE_PIPE_COMPARE: FAIL (a path gave wrong bytes or did not build)"; exit 1
