#!/usr/bin/env bash
# ktap_summary.sh: read a serial console log, find the in-kernel KTAP block
# (native/kernel/core/ktap.h; format: docs.kernel.org/dev-tools/ktap.html,
# idea borrowed from Linux KUnit, no code copied) and turn it into a
# pass/fail summary with an exit code. One reader for every machine that
# prints the block: QEMU, the iMac and the Spark.
#
# Usage: bash scripts/ktap_summary.sh LOG          one line per test, then the verdict
#        bash scripts/ktap_summary.sh --block LOG  the KTAP block alone, console noise removed
#        bash scripts/ktap_summary.sh --self-test  negative controls on built-in logs
# Last line (summary mode): KTAP_SUMMARY: PASS|FAIL total=N pass=P fail=F skip=S [(reason)]
# Exit: 0 PASS, 1 FAIL (a failed test, or a block that is missing, truncated,
# out of order or inconsistent), 64 usage error.
#
# Rules (the KTAP v1 spec, read strictly where a crash could hide):
#  - the block starts at the first line that ends in "KTAP version 1"; whatever
#    precedes that text on its line (a timestamp, a log prefix) is a prefix of
#    that many characters, stripped from every later line, as kunit_tool does;
#  - nesting is two spaces per level; lines that are not version, plan,
#    result or "#" diagnostic lines at the expected depth are console noise
#    and are ignored (KTAP allows unknown lines);
#  - every block needs its plan "1..N" and exactly N results numbered 1..N;
#    a block that never closes or ends short is FAIL (the kernel crashed or
#    the log was cut), never PASS;
#  - a parent "ok" over a failed child is FAIL (inconsistent);
#  - directives: SKIP counts as skip, TODO and XFAIL do not fail, TIMEOUT and
#    ERROR fail, an unknown directive falls back to ok / not ok;
#  - only the first complete top-level document is read.
set -uo pipefail

ktap_parse() { # mode(summary|block) file
    local mode="$1" file="$2"
    local -a plan seen bad sub
    local top=-1 started=0 done=0 plen=0 err="" total=0 pass=0 fail=0 skip=0
    local raw line c lvl sp n rest desc dir word path i res
    while IFS= read -r raw || [[ -n "${raw}" ]]; do
        raw="${raw%$'\r'}"
        if [[ "${started}" == 0 ]]; then
            [[ "${raw}" =~ ^(.*)KTAP\ version\ 1$ ]] || continue
            plen=${#BASH_REMATCH[1]}
            started=1
        fi
        line="${raw:${plen}}"
        [[ "${#raw}" -ge "${plen}" ]] || continue
        c="${line#"${line%%[! ]*}"}"
        sp=$(( ${#line} - ${#c} ))
        (( sp % 2 == 0 )) || continue
        lvl=$(( sp / 2 ))
        if [[ "${c}" == "KTAP version 1" ]]; then
            if (( lvl == top + 1 )); then
                top=${lvl}; plan[top]=-1; seen[top]=0; bad[top]=0; sub[top]=""
                [[ "${mode}" == block ]] && echo "${line}"
            fi
            continue
        fi
        (( top >= 0 )) || continue
        if [[ "${c}" =~ ^1\.\.([0-9]+)$ ]] && (( lvl == top )) && [[ "${plan[top]}" == -1 ]]; then
            plan[top]=${BASH_REMATCH[1]}
            [[ "${mode}" == block ]] && echo "${line}"
        elif [[ "${c}" =~ ^#\ Subtest:\ (.*)$ ]] && (( lvl == top )); then
            sub[top]="${BASH_REMATCH[1]}"
            [[ "${mode}" == block ]] && echo "${line}"
        elif [[ "${c}" == "#"* ]] && (( lvl == top )); then
            [[ "${mode}" == block ]] && echo "${line}"
        elif [[ "${c}" =~ ^(ok|not\ ok)\ ([0-9]+)(\ (.*))?$ ]] && (( lvl == top || lvl == top - 1 )); then
            res="${BASH_REMATCH[1]}"; n="${BASH_REMATCH[2]}"; rest="${BASH_REMATCH[4]}"
            desc="${rest%%#*}"; desc="${desc%"${desc##*[! ]}"}"
            dir=""
            [[ "${rest}" == *"#"* ]] && dir="${rest#*#}" && dir="${dir# }"
            word="${dir%% *}"; word="${word^^}"
            local closed=0 child_bad=0
            if (( lvl == top - 1 )); then
                # This result closes the child block at depth top.
                closed=1; child_bad=${bad[top]}
                if [[ "${plan[top]}" == -1 ]]; then
                    err="${err:-block '${sub[top]:-${desc}}' has no plan}"; child_bad=1
                elif [[ "${seen[top]}" != "${plan[top]}" ]]; then
                    err="${err:-block '${sub[top]:-${desc}}' planned ${plan[top]} results, saw ${seen[top]}}"; child_bad=1
                fi
                top=$(( top - 1 ))
            fi
            [[ "${mode}" == block ]] && echo "${line}"
            if [[ "${plan[top]}" == -1 ]]; then
                err="${err:-a result before the plan at depth ${top}}"
            fi
            if [[ "${n}" != $(( seen[top] + 1 )) ]]; then
                err="${err:-result ${n} '${desc}' out of order at depth ${top} (expected $(( seen[top] + 1 )))}"
            fi
            seen[top]=$(( seen[top] + 1 ))
            local verdict
            case "${word}" in
                SKIP) verdict=SKIP ;;
                TODO|XFAIL) verdict=PASS ;;
                TIMEOUT|ERROR) verdict=FAIL ;;
                *) [[ "${res}" == ok ]] && verdict=PASS || verdict=FAIL ;;
            esac
            if [[ "${closed}" == 1 && "${child_bad}" != 0 && "${verdict}" != FAIL ]]; then
                err="${err:-'${desc}' says ${res} over a failed or incomplete block}"
                verdict=FAIL
            fi
            [[ "${verdict}" == FAIL ]] && bad[top]=1
            if [[ "${closed}" == 0 ]]; then
                # A leaf: a test with no subtests.
                path=""
                for (( i = 1; i <= top; i++ )); do path+="${sub[i]:-level${i}}."; done
                total=$(( total + 1 ))
                case "${verdict}" in
                    PASS) pass=$(( pass + 1 )) ;;
                    FAIL) fail=$(( fail + 1 )) ;;
                    SKIP) skip=$(( skip + 1 )) ;;
                esac
                if [[ "${mode}" == summary ]]; then
                    echo "${verdict}  ${path}${desc}${dir:+ # ${dir}}"
                fi
            fi
            if (( top == 0 )) && [[ "${seen[0]}" == "${plan[0]}" ]]; then
                done=1
                break
            fi
        fi
    done <"${file}"
    local reason=""
    if [[ "${started}" == 0 ]]; then
        reason="no KTAP block in the log"
    elif [[ "${done}" == 0 ]]; then
        if (( top > 0 )); then
            reason="block '${sub[top]:-level${top}}' never closed (crash or cut log)"
        elif [[ "${plan[0]:--1}" == -1 ]]; then
            reason="top-level block has no plan"
        else
            reason="top-level plan ${plan[0]}, saw ${seen[0]} results (crash or cut log)"
        fi
    elif [[ -n "${err}" ]]; then
        reason="${err}"
    fi
    [[ "${mode}" == block ]] && { [[ -z "${reason}" ]]; return; }
    local v=PASS
    [[ -z "${reason}" && "${fail}" == 0 && "${bad[0]:-0}" == 0 ]] || v=FAIL
    echo "KTAP_SUMMARY: ${v} total=${total} pass=${pass} fail=${fail} skip=${skip}${reason:+ (${reason})}"
    [[ "${v}" == PASS ]]
}

ktap_self_test() {
    local d rc=0
    d="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf '${d}'" RETURN
    expect() { # name want_exit want_last_line_pattern file
        local out got
        out="$(ktap_parse summary "$4")"; got=$?
        if [[ "${got}" == "$2" && "$(tail -1 <<<"${out}")" =~ $3 ]]; then
            echo "PASS  self-test: $1"
        else
            echo "FAIL  self-test: $1 (exit ${got}, want $2): $(tail -1 <<<"${out}")"
            rc=1
        fi
    }
    # Positive: the kernel's shape with console noise, CRs and an odd-width
    # noise line between results.
    printf '%s\r\n' 'boot noise' 'KTAP version 1' '1..2' '  KTAP version 1' '  # Subtest: mm' '  1..2' \
        '  ok 1 mmu_enabled' 'gic: v3 (noise from another printer)' '   odd indent noise' '  ok 2 guard_page' \
        'ok 1 mm' '  KTAP version 1' '  # Subtest: stages' '  1..1' '  ok 1 devices # SKIP not linked' \
        'ok 2 stages' 'trailing noise' 'KTAP version 1' '1..1' 'not ok 1 second_document_is_ignored' >"${d}/good"
    expect "clean pass with noise" 0 '^KTAP_SUMMARY: PASS total=3 pass=2 fail=0 skip=1$' "${d}/good"
    # Positive: a timestamp prefix on every line (Linux dmesg shape).
    sed 's/^/[    1.000000] /' "${d}/good" >"${d}/prefixed"
    expect "timestamp prefix stripped" 0 '^KTAP_SUMMARY: PASS total=3' "${d}/prefixed"
    # Negative: one failed case.
    sed 's/^  ok 2 guard_page/  not ok 2 guard_page/; s/^ok 1 mm/not ok 1 mm/' "${d}/good" >"${d}/fail"
    expect "failed case" 1 '^KTAP_SUMMARY: FAIL total=3 pass=1 fail=1 skip=1$' "${d}/fail"
    # Negative: parent says ok over a failed child.
    sed 's/^  ok 2 guard_page/  not ok 2 guard_page/' "${d}/good" >"${d}/liar"
    expect "parent ok over failed child" 1 'FAIL .*says ok over a failed' "${d}/liar"
    # Negative: the kernel died inside a suite (log cut mid block).
    head -8 "${d}/good" >"${d}/cut"
    expect "log cut inside a block" 1 "FAIL .*block 'mm' never closed" "${d}/cut"
    # Negative: the log ends after the first suite of two.
    head -11 "${d}/good" >"${d}/short"
    expect "top-level plan not met" 1 'FAIL .*plan 2, saw 1 results' "${d}/short"
    # Negative: no block at all (a crash before the final report).
    printf 'report_kind: panic\n' >"${d}/none"
    expect "no block" 1 'FAIL .*no KTAP block' "${d}/none"
    # Negative: a missing result inside a suite.
    grep -v '^  ok 2 guard_page' "${d}/good" >"${d}/gap"
    expect "missing result" 1 'FAIL .*planned 2 results, saw 1' "${d}/gap"
    # Negative: out-of-order numbering.
    sed 's/^  ok 2 guard_page/  ok 3 guard_page/' "${d}/good" >"${d}/order"
    expect "out of order" 1 'FAIL .*out of order' "${d}/order"
    # Directives: XFAIL does not fail, TIMEOUT fails even on an ok line.
    printf '%s\n' 'KTAP version 1' '1..2' 'not ok 1 known_bad # XFAIL tracked' 'ok 2 slow # TIMEOUT 5s' >"${d}/dir"
    expect "XFAIL passes, TIMEOUT fails" 1 '^KTAP_SUMMARY: FAIL total=2 pass=1 fail=1 skip=0$' "${d}/dir"
    # --block drops the noise and keeps only the first document.
    if [[ "$(ktap_parse block "${d}/good" | wc -l)" == 13 ]] && ! ktap_parse block "${d}/good" | grep -q noise; then
        echo "PASS  self-test: --block keeps the 13 KTAP lines and drops noise"
    else
        echo "FAIL  self-test: --block output"; rc=1
    fi
    [[ "${rc}" == 0 ]] && echo "KTAP_SELF_TEST: PASS" || echo "KTAP_SELF_TEST: FAIL"
    return "${rc}"
}

case "${1:-}" in
    --self-test) ktap_self_test; exit $? ;;
    --block) [[ -r "${2:-}" ]] || { echo "usage: $0 --block LOG" >&2; exit 64; }; ktap_parse block "$2"; exit $? ;;
    -*|"") echo "usage: $0 LOG | --block LOG | --self-test" >&2; exit 64 ;;
    *) [[ -r "$1" ]] || { echo "cannot read $1" >&2; exit 64; }; ktap_parse summary "$1"; exit $? ;;
esac
