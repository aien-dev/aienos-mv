#!/usr/bin/env bash
# aienos_run_measure.sh -- boot an image N times through aienos-run and report the receipt
# timings (ledger L261: startup and boot-to-console per backend, the faster one wins).
#
#     scripts/aienos_run_measure.sh RUNNER N OUTDIR -- <aienos-run args>
#
# Each run is `RUNNER --headless --receipt --exit-on-ready <args>` with its own
# AIENOS_RUN_HOME under OUTDIR (so every run starts from a fresh cache, Store disk and
# firmware variables after the first image copy). Per run it records the exit code and
# the receipt lines vmm_started / first_output / console_ready (milliseconds from the
# moment aienos-run started the backend). Last lines:
#     AIENOS_RUN_MEASURE: <field> n=<ok runs> min=<ms> median=<ms> max=<ms>
#     AIENOS_RUN_MEASURE: PASS runs=<N> | FAIL (...)
set -euo pipefail
tag="AIENOS_RUN_MEASURE"
runner="${1:?usage: $0 RUNNER N OUTDIR -- args}"
n="${2:?}"
out="${3:?}"
shift 3
[[ "${1:-}" == "--" ]] && shift
mkdir -p "${out}"
out="$(cd "${out}" && pwd)"
: >"${out}/runs.tsv"
fails=0
for i in $(seq 1 "${n}"); do
    home="${out}/home-${i}"
    rm -rf "${home}"
    set +e
    AIENOS_RUN_HOME="${home}" "${runner}" --headless --receipt --exit-on-ready "$@" \
        >"${out}/run-${i}.out" 2>"${out}/run-${i}.err"
    rc=$?
    set -e
    get() { sed -nE "s/^aienos-run: receipt $1=([0-9.]+) ms.*/\1/p" "${out}/run-${i}.err" | head -1; }
    v="$(get vmm_started)"; f="$(get first_output)"; r="$(get console_ready)"
    printf '%s\t%s\t%s\t%s\t%s\n' "${i}" "${rc}" "${v:-NA}" "${f:-NA}" "${r:-NA}" >>"${out}/runs.tsv"
    echo "run ${i}: exit=${rc} vmm_started=${v:-NA} first_output=${f:-NA} console_ready=${r:-NA}"
    if [[ ${rc} != 0 || -z "${r}" ]]; then fails=$((fails + 1)); fi
done
stats() { # column number, name
    local vals
    vals="$(awk -F'\t' -v c="$1" '$2==0 && $c!="NA" {print $c}' "${out}/runs.tsv" | sort -g)"
    local k
    k="$(grep -c . <<<"${vals}" || true)"
    if [[ -z "${vals}" ]]; then echo "${tag}: $2 n=0"; return; fi
    local mid=$(((k + 1) / 2))
    echo "${tag}: $2 n=${k} min=$(head -1 <<<"${vals}") median=$(sed -n "${mid}p" <<<"${vals}") max=$(tail -1 <<<"${vals}")"
}
stats 3 vmm_started_ms
stats 4 first_output_ms
stats 5 console_ready_ms
if [[ ${fails} == 0 ]]; then echo "${tag}: PASS runs=${n}"; else echo "${tag}: FAIL (${fails} of ${n} runs failed or never reached the console)"; exit 1; fi
