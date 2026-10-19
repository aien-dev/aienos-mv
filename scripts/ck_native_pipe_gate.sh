#!/usr/bin/env bash
# ck_native_pipe_gate.sh: the native pipeline acceptance gate (aienos-mv#432, subordinate to aien-architecture-mv#158).
# One verdict for "native pipelines work" built from evidence that already exists, plus a readiness matrix per platform.
# It adds no second CI authority: scripts/ck_gates.sh runs it as the child `native_pipe` (gates PIPE, TASK and
# NATIVE_PIPE come from its log) and the Spark judge runs ck_gates.sh as before.
#
# Categories, each PASS | FAIL | NOT_RUN, never inferred from one another:
#   host_unit         make -C native/kernel native-pipe-host: test_pipe, test_task, test_osh_op (with the journal), the
#                     host baselines (test_task --bench) and every mutant of those tests (each must FAIL).
#   qemu_integration  scripts/qemu_ck_pipe_test.sh, scripts/qemu_ck_task_test.sh (with the QEMU baselines) and
#                     scripts/qemu_ck_osh_op_test.sh (three boots of one disk: journal, power cut, recovery), each its
#                     own verdict, then checks on their serial logs that the pipelines ran real admitted stages
#                     concurrently (no fake statuses, every EL0 slot torn down) and that no unknown outcome was replayed.
#   pi5_physical, imac_physical, gb10_physical
#                     always NOT_RUN here: this runner has host and QEMU only and takes no on-device evidence. A physical
#                     column changes only with an independent on-device run; nothing in this script can set it.
# Mutation classes (from the host_unit log), each KILLED only if every listed mutant made its test FAIL:
#   capability_validation  pipe write right skipped, stage ends with full rights, admission skipped, OSH permission
#                          check skipped, location capability skipped
#   stage_teardown         session keeps an inner write end, partial launch not reaped, a non-starting command keeps
#                          its pipe ends, cancel grace outcome order-dependent
#   byte_ordering          pipe swaps bytes: caught by the pipe, the task and the OSH tests
#   intent_journaling      effect without its intent, non-durable journal accepted, failed append not poisoning
#
# Baselines: every "pipe_bench:" line of the host run and every "BASELINE pipe_bench:" line of the QEMU task run is
# printed as "BASELINE <category> ..." with its own label. QEMU numbers are EMULATOR numbers (TCG on the Spark), host
# numbers are C stand-in stages on the Spark CPU; neither is a hardware number and none has a pass threshold.
#
# Spec read: aien-protocols-mv specs/osh-platform/OSH_PLATFORM_ABI.md at fb5fca98e667 (sections 8.4, 8.5, 10, 12, 15;
# unchanged in main 2007ee6121be) and STREAM_TASK.md on feat/osh-stream-abi-43 at 181855c9ff3e (ST6.3, ST6.5, ST9,
# ST10.2, ST10.3). The first output line carries the ST10.3 pins (impl, corpus, toolchain, target).
# UNVERIFIED (not pinned by the spec): the category names and the PASS/FAIL/NOT_RUN matrix, the four mutation classes
# and their mutant lists, the "real work" log checks, the line prefix NATIVE_PIPE_RUN (this gate runs no corpus
# vectors, so it does not claim the OSHSV1_RUN keyword) and target=host+qemu-virt for a run that covers both targets.
#
# Usage: bash scripts/ck_native_pipe_gate.sh            run the gate (QEMU children take the gate lock themselves)
#        bash scripts/ck_native_pipe_gate.sh --self-test  check the parsing with canned logs (no build, no QEMU)
# Env:   AIENOS_LOG_DIR   if set, the child serial logs, host.log and native_pipe_matrix.json are copied there
#        AIENOS_NP_WORK   work folder (default: a new temp folder outside the tree)
# Exit:  0 PASS, 1 FAIL, 3 NOT_RUN (a QEMU child could not run: gate lock, quiet flag, no QEMU), 2 usage error.
# Last line: AIENOS_CK_NATIVE_PIPE: PASS|FAIL|NOT_RUN (reasons).
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PHYS_WHY="no on-device evidence; this runner is host and QEMU only"
SPEC_CORPUS=181855c9ff3ec9bcc635d2043d5d7d213cee6021   # STREAM_TASK.md revision 2 (feat/osh-stream-abi-43)
QEMU_KIDS=(pipe task osh_op)
declare -A KID_SCRIPT=([pipe]=qemu_ck_pipe_test.sh [task]=qemu_ck_task_test.sh [osh_op]=qemu_ck_osh_op_test.sh)
declare -A KID_MARK=([pipe]=PIPE [task]=TASK [osh_op]=OSH_OP)

# The mutation table: class | exact line prefix printed by make native-pipe-host for that mutant.
MUTATION_TABLE='
capability_validation|test_pipe mutant (write right not checked): FAILS as required
capability_validation|test_task mutant FULL_RIGHTS: FAILS as required
capability_validation|test_task mutant SKIP_ADMIT: FAILS as required
capability_validation|test_osh_op mutant SKIP_PERM: FAILS as required
capability_validation|test_osh_op mutant LOC_NO_PERM: FAILS as required
stage_teardown|test_task mutant KEEP_SESSION_END: FAILS as required
stage_teardown|test_task mutant NO_PARTIAL_REAP: FAILS as required
stage_teardown|test_osh_op mutant KEEP_GAP_ENDS: FAILS as required
stage_teardown|test_task mutant GRACE_BROKEN_PIPE: FAILS as required
byte_ordering|test_pipe pipe mutant REORDER: FAILS as required
byte_ordering|test_task pipe mutant REORDER: FAILS as required
byte_ordering|test_osh_op pipe mutant REORDER: FAILS as required
intent_journaling|test_osh_op mutant SKIP_INTENT: FAILS as required
intent_journaling|test_osh_op mutant VOLATILE_OK: FAILS as required
intent_journaling|test_osh_op mutant NO_POISON: FAILS as required
'
MUTATION_CLASSES=(capability_validation stage_teardown byte_ordering intent_journaling)

sha_of() { sha256sum | cut -d' ' -f1; }
file_sha() { [[ -r "$1" ]] && sha_of <"$1" || echo "not readable"; }
json_str() {
    local s="$1"
    s="${s//\\/\\\\}"; s="${s//\"/\\\"}"; s="${s//$'\t'/\\t}"; s="${s//$'\r'/}"; s="${s//$'\n'/\\n}"
    printf '"%s"' "$(printf '%s' "${s}" | tr -d '\000-\010\013\014\016-\037')"
}

# host_verdict LOG RC -> "VERDICT|reason": PASS needs exit 0, the NATIVE_PIPE_HOST: PASS line, the three test PASS lines,
# no mutant that passed and the host baselines' own PASS line.
host_verdict() {
    local log="$1" rc="$2" t
    [[ -r "${log}" ]] || { echo "FAIL|no host log"; return; }
    grep -qE 'PASSED, test does not catch it|did not end with a verdict' "${log}" && { echo "FAIL|a mutant survived or gave no verdict"; return; }
    for t in test_pipe test_task test_osh_op; do
        grep -qE "^${t}: PASS \\([0-9]+ checks\\)$" "${log}" || { echo "FAIL|${t} did not PASS"; return; }
    done
    grep -qxF 'test_task_bench: PASS' "${log}" || { echo "FAIL|host baselines did not PASS"; return; }
    grep -qE '^pipe_bench: .*result=FAIL' "${log}" && { echo "FAIL|a host baseline scenario failed"; return; }
    grep -qxF 'NATIVE_PIPE_HOST: PASS' "${log}" || { echo "FAIL|no NATIVE_PIPE_HOST: PASS line"; return; }
    [[ "${rc}" == 0 ]] || { echo "FAIL|make exited ${rc}"; return; }
    echo "PASS|3 host tests, $(grep -c 'FAILS as required' "${log}") mutants killed, host baselines"
}

# mutation_class LOG CLASS -> "KILLED|n of n" or "SURVIVED|missing: ..."
mutation_class() {
    local log="$1" cls="$2" c line n=0 k=0 miss=""
    while IFS='|' read -r c line; do
        [[ "${c}" == "${cls}" ]] || continue
        n=$((n + 1))
        if [[ -r "${log}" ]] && grep -qF -- "${line}" "${log}"; then k=$((k + 1)); else miss+="${line%%:*}; "; fi
    done <<<"${MUTATION_TABLE}"
    if [[ ${n} -gt 0 && ${k} == "${n}" ]]; then echo "KILLED|${k} of ${n} mutants fail their test"
    else echo "SURVIVED|${k} of ${n}; not shown failing: ${miss%; }"; fi
}

# kid_verdict LOG MARK RC -> PASS|FAIL|NOT_RUN, the rules of ck_gates.sh classify_gate: one whole verdict line, and a
# PASS with a nonzero exit is FAIL.
kid_verdict() {
    local log="$1" mark="$2" rc="$3" m
    [[ -r "${log}" ]] || { echo FAIL; return; }
    m="$(sed -nE "s/^AIENOS_CK_${mark}: (PASS|FAIL|NOT_RUN)( .*)?\$/\\1/p" "${log}" | sort -u)"
    case "${m}" in
        PASS) [[ "${rc}" == 0 ]] && echo PASS || echo FAIL ;;
        NOT_RUN) echo NOT_RUN ;;
        *) echo FAIL ;;
    esac
}

# real_work LOGDIR -> lines "ok|what" or "bad|what": the pipelines ran real admitted stages concurrently, nothing fake,
# nothing left over, no unknown outcome replayed. Reads the serial logs the QEMU children copy to AIENOS_LOG_DIR.
real_work() {
    local d="$1" s="$1/qemu_ck_osh_op_serial.log" t="$1/qemu_ck_task_serial.log" rec="$1/qemu_ck_osh_op_serial_recover.log"
    local f n
    chk() { if eval "$2"; then echo "ok|$1"; else echo "bad|$1"; fi; }
    chk "task: two admitted stages alive at once with bytes in flight" \
        "grep -qE '^task_check: name=two_stage_mid_run_both_alive result=PASS .*in_flight=[1-9]' '${t}' 2>/dev/null"
    chk "task: every EL0 step torn down, nothing left over" \
        "grep -qE '^task_test: done checks=[0-9]+ failed=0 launches=[1-9][0-9]* not_torn_down=0 ' '${t}' 2>/dev/null"
    chk "osh_op: gen | xform | sink through the shell, three stages alive at once, sink hash matches" \
        "grep -qE '^osh_op_stages: case=stream3 n=1 launch=OK started=3 .*sink_count=163840 sink_hash_ok=1 .*mid_alive=3 ' '${s}' 2>/dev/null"
    chk "osh_op: every native case ended with no task, no pipe and every EL0 slot torn down" \
        "[[ -r '${s}' ]] && ! grep -E '^osh_op_native_end: ' '${s}' | grep -qvE 'tasks_empty=1 pipes_empty=1 stage_steps=[0-9]+ not_torn_down=0 '"
    n=0
    [[ -r "${s}" ]] && n="$(grep -cE '^osh_op_native_end: .*stage_steps=[1-9]' "${s}")"
    chk "osh_op: native cases ran real admitted stage steps (${n} cases with steps)" "[[ ${n} -ge 1 ]]"
    f=0
    for x in "${d}"/qemu_ck_osh_op_serial*.log "${t}"; do [[ -r "${x}" ]] && grep -qE 'fake_status|event fake' "${x}" && f=1; done
    chk "no fake pipeline status anywhere in the native pipeline logs" "[[ ${f} == 0 && -r '${s}' ]]"
    chk "restart: unknown outcomes recorded as OUTCOME_UNKNOWN and never replayed" \
        "grep -qE '^osh_op_record: .*outcome=OUTCOME_UNKNOWN .*replayed=0 category=UNKNOWN_STARTED' '${rec}' 2>/dev/null && ! grep -qE 'replayed=[1-9]' '${rec}'"
}

# baseline_lines HOSTLOG TASKLOG RECLOG: the measurements, each with its category and its own label.
baseline_lines() {
    [[ -r "$1" ]] && grep -E '^pipe_bench: ' "$1" | sed 's/^/BASELINE host_unit /'
    [[ -r "$2" ]] && grep -E '^pipe_bench: ' "$2" | sed 's/^/BASELINE qemu_integration /'
    if [[ -r "$3" ]]; then
        echo "BASELINE qemu_integration restart: unknown_outcomes=$(grep -cE '^osh_op_record: .*outcome=OUTCOME_UNKNOWN' "$3") replayed=$(grep -cE '^osh_op_record: .*replayed=[1-9]' "$3"); QEMU (aarch64 virt), TEST signer, not physical, emulator run"
    fi
    return 0
}

# decide HOSTV QEMUV MUTATION_KILLED(0|1) -> final verdict word
decide() {
    if [[ "$1" == FAIL || "$2" == FAIL || "$3" != 1 ]]; then echo FAIL
    elif [[ "$1" == PASS && "$2" == PASS ]]; then echo PASS
    else echo NOT_RUN; fi
}

# report HOSTLOG HOSTRC LOGDIR KIDLOG_PREFIX: prints every line of the gate; sets final, host_v, qemu_v, mut_ok.
report() {
    local hlog="$1" hrc="$2" ldir="$3" kp="$4" out v r c k bad=0 nr=0
    out="$(host_verdict "${hlog}" "${hrc}")"; host_v="${out%%|*}"; r="${out#*|}"
    echo "NATIVE_PIPE_CATEGORY host_unit: ${host_v} (${r})"
    mut_ok=1; declare -gA mut_v=()
    for c in "${MUTATION_CLASSES[@]}"; do
        out="$(mutation_class "${hlog}" "${c}")"; mut_v[${c}]="${out%%|*}"
        [[ "${mut_v[${c}]}" == KILLED ]] || mut_ok=0
        echo "NATIVE_PIPE_MUTATION ${c}: ${out%%|*} (${out#*|})"
    done
    declare -gA kid_v=()
    for k in "${QEMU_KIDS[@]}"; do
        v="$(kid_verdict "${kp}${k}.log" "${KID_MARK[${k}]}" "${kid_rc[${k}]:-x}")"; kid_v[${k}]="${v}"
        [[ "${v}" == FAIL ]] && bad=1
        [[ "${v}" == NOT_RUN ]] && nr=1
        # PIPE and TASK verdicts pass through whole, so ck_gates.sh reads them from this log; OSH_OP has its own child there.
        if [[ "${k}" == osh_op ]]; then echo "child ${KID_SCRIPT[${k}]}: ${v}"; else echo "AIENOS_CK_${KID_MARK[${k}]}: ${v}"; fi
    done
    qemu_v=PASS
    if [[ ${bad} == 1 ]]; then qemu_v=FAIL; elif [[ ${nr} == 1 ]]; then qemu_v=NOT_RUN; fi
    if [[ "${qemu_v}" == PASS ]]; then
        while IFS='|' read -r v r; do
            echo "REAL_WORK ${v^^}  ${r}"
            [[ "${v}" == ok ]] || qemu_v=FAIL
        done < <(real_work "${ldir}")
    fi
    echo "NATIVE_PIPE_CATEGORY qemu_integration: ${qemu_v} (pipe=${kid_v[pipe]} task=${kid_v[task]} osh_op=${kid_v[osh_op]})"
    for c in pi5 imac gb10; do echo "NATIVE_PIPE_CATEGORY ${c}_physical: NOT_RUN (${PHYS_WHY})"; done
    baseline_lines "${hlog}" "${ldir}/qemu_ck_task_serial.log" "${ldir}/qemu_ck_osh_op_serial_recover.log"
    final="$(decide "${host_v}" "${qemu_v}" "${mut_ok}")"
}

write_matrix() { # FILE
    local c first=1
    {
        echo "{"
        echo "  \"schema\": \"aienos.native_pipe_gate.v1\","
        echo "  \"issue\": \"aienos-mv#432\","
        echo "  \"commit\": $(json_str "$(git -C "${repo_root}" rev-parse HEAD 2>/dev/null || echo unknown)"),"
        echo "  \"tree_clean\": $([[ -z "$(git -C "${repo_root}" status --porcelain 2>/dev/null)" ]] && echo true || echo false),"
        echo "  \"pins\": {\"cc\": $(json_str "$(cc --version 2>/dev/null | head -1)"), \"make\": $(json_str "$(make --version 2>/dev/null | head -1)"),"
        echo "           \"qemu\": $(json_str "$(qemu-system-aarch64 --version 2>/dev/null | head -1 || echo 'not installed')"),"
        echo "           \"aavmf_code_sha256\": $(json_str "$(file_sha "${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}")"),"
        echo "           \"host\": $(json_str "$(uname -srm)"),"
        echo "           \"task_fixture_provenance_sha256\": $(json_str "$(file_sha "${repo_root}/native/kernel/tests/fixtures/task/PROVENANCE.txt")")},"
        echo "  \"categories\": {\"host_unit\": $(json_str "${host_v}"), \"qemu_integration\": $(json_str "${qemu_v}"),"
        echo "                 \"pi5_physical\": \"NOT_RUN\", \"imac_physical\": \"NOT_RUN\", \"gb10_physical\": \"NOT_RUN\"},"
        printf '  "mutation": {'
        for c in "${MUTATION_CLASSES[@]}"; do [[ ${first} == 1 ]] || printf ', '; first=0; printf '%s: %s' "$(json_str "${c}")" "$(json_str "${mut_v[${c}]}")"; done
        echo "},"
        echo "  \"logs_sha256\": {\"host\": $(json_str "$(file_sha "${host_log}")"), \"pipe\": $(json_str "$(file_sha "${work}/kid_pipe.log")"), \"task\": $(json_str "$(file_sha "${work}/kid_task.log")"), \"osh_op\": $(json_str "$(file_sha "${work}/kid_osh_op.log")")},"
        echo "  \"verdict\": $(json_str "${final}"),"
        echo "  \"physical_statement\": $(json_str "Host and QEMU only. Physical columns are NOT_RUN until an independent on-device run exists; nothing here qualifies a Pi 5, iMac or GB10.")"
        echo "}"
    } >"$1"
}

# ----------------------------------------------------------------- self-test
self_test() {
    local tmp st=0
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/np-gate-selftest.XXXXXX")" || exit 2
    ok() { echo "PASS  $*"; }
    no() { echo "FAIL  $*"; st=1; }
    good_host() {
        printf '%s\n' 'test_pipe: PASS (1239 checks)' 'test_task: PASS (221 checks)' 'test_osh_op: PASS (534 checks)'
        local c line
        while IFS='|' read -r c line; do [[ -n "${c}" ]] && echo "${line} (x: FAIL (1 of 2 checks failed))"; done <<<"${MUTATION_TABLE}"
        printf '%s\n' 'pipe_bench: case=two_stage_163840 result=PASS stages=2; host' 'test_task_bench: PASS' 'NATIVE_PIPE_HOST: PASS'
    }
    good_logs() { # DIR
        mkdir -p "$1"
        printf '%s\n' 'task_check: name=two_stage_mid_run_both_alive result=PASS gen_state=2 in_flight=65024 clean=alive' \
            'task_test: done checks=20 failed=0 launches=900 not_torn_down=0 scheduling=cooperative-step; L' \
            'pipe_bench: case=two_stage_163840 result=PASS stages=2; QEMU, emulator numbers' >"$1/qemu_ck_task_serial.log"
        printf '%s\n' 'osh_op_stages: case=stream3 n=1 launch=OK started=3 cmds=3 sink_count=163840 sink_hash_ok=1 max_in_pipes=65536 mid_alive=3 mid_in_flight=1; L' \
            'osh_op_native_end: case=stream3 tasks_empty=1 pipes_empty=1 stage_steps=402 not_torn_down=0 admit_refused=0; L' >"$1/qemu_ck_osh_op_serial.log"
        printf '%s\n' 'osh_op_record: seq=1 outcome=OUTCOME_UNKNOWN request_digest=00 replayed=0 category=UNKNOWN_STARTED effect=task_spawn; L' >"$1/qemu_ck_osh_op_serial_recover.log"
    }
    kids() { # PIPE_LINE PIPE_RC TASK_LINE TASK_RC OSH_LINE OSH_RC
        printf '%s\n' "$1" >"${tmp}/k_pipe.log"; printf '%s\n' "$3" >"${tmp}/k_task.log"; printf '%s\n' "$5" >"${tmp}/k_osh_op.log"
        declare -gA kid_rc=([pipe]="$2" [task]="$4" [osh_op]="$6")
    }
    run() { report "$1" "$2" "$3" "${tmp}/k_" >"${tmp}/out.txt"; }
    expect() { # NAME WANT
        if [[ "${final}" == "$2" ]]; then ok "$1 -> $2"; else no "$1 -> ${final}, expected $2"; sed 's/^/      /' "${tmp}/out.txt" | head -30; fi
    }
    good_host >"${tmp}/host.log"; good_logs "${tmp}/logs"
    kids 'AIENOS_CK_PIPE: PASS' 0 'AIENOS_CK_TASK: PASS' 0 'AIENOS_CK_OSH_OP: PASS' 0
    run "${tmp}/host.log" 0 "${tmp}/logs"; expect "all good" PASS
    grep -qxF 'AIENOS_CK_PIPE: PASS' "${tmp}/out.txt" && grep -qxF 'AIENOS_CK_TASK: PASS' "${tmp}/out.txt" \
        && ok "PIPE and TASK verdicts pass through whole for ck_gates.sh" || no "PIPE/TASK pass-through lines missing"
    grep -q '^AIENOS_CK_OSH_OP' "${tmp}/out.txt" && no "OSH_OP verdict leaked as a bare line" || ok "OSH_OP stays prefixed (its own child in ck_gates.sh)"
    [[ "$(grep -c '_physical: NOT_RUN' "${tmp}/out.txt")" == 3 ]] && ok "three physical columns NOT_RUN when host and QEMU PASS" || no "physical columns not NOT_RUN"
    grep -q '^BASELINE host_unit pipe_bench: ' "${tmp}/out.txt" && grep -q '^BASELINE qemu_integration pipe_bench: ' "${tmp}/out.txt" \
        && ok "baselines carried with their category" || no "baseline lines missing"
    # a log that claims a physical PASS changes nothing
    echo 'NATIVE_PIPE_CATEGORY pi5_physical: PASS' >>"${tmp}/host.log"; echo 'AIENOS_CK_NATIVE_PIPE_PI5: PASS' >>"${tmp}/logs/qemu_ck_task_serial.log"
    run "${tmp}/host.log" 0 "${tmp}/logs"
    grep -qxF "NATIVE_PIPE_CATEGORY pi5_physical: NOT_RUN (${PHYS_WHY})" "${tmp}/out.txt" && ok "a claimed physical PASS in a log stays NOT_RUN" || no "a log set a physical column"
    good_host >"${tmp}/host.log"; good_logs "${tmp}/logs"
    # each mutation class goes SURVIVED (and the gate FAILs) when one of its mutants is not shown failing
    local c line
    for c in "${MUTATION_CLASSES[@]}"; do
        line="$(awk -F'|' -v c="${c}" '$1==c {print $2; exit}' <<<"${MUTATION_TABLE}")"
        good_host | grep -vF -- "${line}" >"${tmp}/h_${c}.log"
        run "${tmp}/h_${c}.log" 0 "${tmp}/logs"
        grep -q "^NATIVE_PIPE_MUTATION ${c}: SURVIVED" "${tmp}/out.txt" && [[ "${final}" == FAIL ]] \
            && ok "mutation ${c}: one mutant not shown failing -> SURVIVED, gate FAIL" || no "mutation ${c} not caught (${final})"
    done
    { good_host; echo 'test_task mutant X: PASSED, test does not catch it'; } >"${tmp}/h_surv.log"
    run "${tmp}/h_surv.log" 0 "${tmp}/logs"; expect "a surviving mutant" FAIL
    good_host | sed 's/^test_osh_op: PASS (534 checks)$/test_osh_op: FAIL (2 of 534 checks failed)/' >"${tmp}/h_red.log"
    run "${tmp}/h_red.log" 0 "${tmp}/logs"; expect "a red host test" FAIL
    run "${tmp}/host.log" 2 "${tmp}/logs"; expect "host PASS lines but make exit 2" FAIL
    good_host | sed 's/^pipe_bench: case=two_stage_163840 result=PASS/pipe_bench: case=two_stage_163840 result=FAIL/' >"${tmp}/h_bf.log"
    run "${tmp}/h_bf.log" 0 "${tmp}/logs"; expect "a failed host baseline scenario" FAIL
    kids 'AIENOS_CK_PIPE: PASS' 0 'AIENOS_CK_TASK: FAIL' 1 'AIENOS_CK_OSH_OP: PASS' 0
    run "${tmp}/host.log" 0 "${tmp}/logs"; expect "TASK child FAIL" FAIL
    kids 'AIENOS_CK_PIPE: PASS' 0 'AIENOS_CK_TASK: PASS' 0 'AIENOS_CK_OSH_OP: NOT_RUN' 3
    run "${tmp}/host.log" 0 "${tmp}/logs"; expect "OSH_OP child NOT_RUN (gate lock)" NOT_RUN
    kids 'AIENOS_CK_PIPE: PASS' 1 'AIENOS_CK_TASK: PASS' 0 'AIENOS_CK_OSH_OP: PASS' 0
    run "${tmp}/host.log" 0 "${tmp}/logs"; expect "PIPE child PASS line with exit 1" FAIL
    kids 'AIENOS_CK_PIPE: PASS' 0 'serial: AIENOS_CK_TASK: PASS' 0 'AIENOS_CK_OSH_OP: PASS' 0
    run "${tmp}/host.log" 0 "${tmp}/logs"; expect "TASK verdict only inside another line" FAIL
    kids 'AIENOS_CK_PIPE: PASS' 0 'AIENOS_CK_TASK: PASS' 0 'AIENOS_CK_OSH_OP: PASS' 0
    # the real-work checks go red on a fake status, a leak, a missing concurrency snapshot and a replay
    echo 'event fake_status_exhausted after=3' >>"${tmp}/logs/qemu_ck_osh_op_serial.log"
    run "${tmp}/host.log" 0 "${tmp}/logs"; expect "a fake pipeline status in the OSH log" FAIL
    good_logs "${tmp}/logs"; echo 'osh_op_native_end: case=x tasks_empty=0 pipes_empty=1 stage_steps=3 not_torn_down=0 admit_refused=0; L' >>"${tmp}/logs/qemu_ck_osh_op_serial.log"
    run "${tmp}/host.log" 0 "${tmp}/logs"; expect "a native case that left a task behind" FAIL
    good_logs "${tmp}/logs"; sed -i 's/mid_alive=3/mid_alive=1/' "${tmp}/logs/qemu_ck_osh_op_serial.log"
    run "${tmp}/host.log" 0 "${tmp}/logs"; expect "three stages never alive at once" FAIL
    good_logs "${tmp}/logs"; sed -i 's/replayed=0/replayed=1/' "${tmp}/logs/qemu_ck_osh_op_serial_recover.log"
    run "${tmp}/host.log" 0 "${tmp}/logs"; expect "an unknown outcome replayed after restart" FAIL
    good_logs "${tmp}/logs"; sed -i 's/not_torn_down=0 scheduling/not_torn_down=1 scheduling/' "${tmp}/logs/qemu_ck_task_serial.log"
    run "${tmp}/host.log" 0 "${tmp}/logs"; expect "an EL0 slot not torn down in the task run" FAIL
    rm -rf "${tmp}/logs"; run "${tmp}/host.log" 0 "${tmp}/logs"; expect "QEMU PASS lines but no serial logs to check" FAIL
    rm -rf --one-file-system "${tmp}"
    if [[ ${st} == 0 ]]; then echo "AIENOS_CK_NATIVE_PIPE_SELF_TEST: PASS"; exit 0; fi
    echo "AIENOS_CK_NATIVE_PIPE_SELF_TEST: FAIL"; exit 1
}

# ---------------------------------------------------------------------- main
main() {
    case "${1:-}" in
        --self-test) self_test ;;
        "") ;;
        -h|--help) sed -n '2,41p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "ck_native_pipe_gate: unknown argument: $1" >&2; exit 2 ;;
    esac
    work="${AIENOS_NP_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/np-gate.XXXXXX")}" || exit 2
    mkdir -p "${work}/logs"
    host_log="${work}/host.log"
    echo "NATIVE_PIPE_RUN impl=aienos-mv@$(git -C "${repo_root}" rev-parse HEAD) corpus=aien-protocols-mv@${SPEC_CORPUS}" \
        "toolchain=$(cc --version 2>/dev/null | head -1 | tr ' ' '_') target=host+qemu-virt"
    echo "native pipeline acceptance gate (aienos-mv#432): host and QEMU only, qualifies nothing physical"
    echo "--- host_unit: make -C native/kernel native-pipe-host" >&2
    nice make -s -j"${AIENOS_NP_JOBS:-8}" -C "${repo_root}/native/kernel" OUT="${work}/kout" native-pipe-host >"${host_log}" 2>&1
    local hrc=$? k
    declare -gA kid_rc=()
    for k in "${QEMU_KIDS[@]}"; do
        echo "--- qemu_integration: ${KID_SCRIPT[${k}]}" >&2
        AIENOS_LOG_DIR="${work}/logs" AIENOS_CK_OUT_PIPE="${work}/q_pipe" AIENOS_CK_OUT_TASK="${work}/q_task" \
            AIENOS_CK_OUT_OSH="${work}/q_osh" AIENOS_CK_OUT_PROD="${work}/q_prod" \
            bash "${repo_root}/scripts/${KID_SCRIPT[${k}]}" >"${work}/kid_${k}.log" 2>&1 </dev/null
        kid_rc[${k}]=$?
        echo "--- ${KID_SCRIPT[${k}]} exit ${kid_rc[${k}]}: $(tail -1 "${work}/kid_${k}.log")" >&2
    done
    report "${host_log}" "${hrc}" "${work}/logs" "${work}/kid_"
    write_matrix "${work}/native_pipe_matrix.json"
    if [[ -n "${AIENOS_LOG_DIR:-}" ]]; then
        mkdir -p "${AIENOS_LOG_DIR}" && cp "${work}"/logs/*.log "${host_log}" "${work}"/kid_*.log "${work}/native_pipe_matrix.json" "${AIENOS_LOG_DIR}/" 2>/dev/null
    fi
    echo "matrix: ${work}/native_pipe_matrix.json"
    local why="host_unit ${host_v}, qemu_integration ${qemu_v}, mutation $([[ ${mut_ok} == 1 ]] && echo '4 of 4 classes KILLED' || echo 'NOT all classes killed'); physical NOT_RUN"
    echo "AIENOS_CK_NATIVE_PIPE: ${final} (${why})"
    case "${final}" in PASS) exit 0 ;; NOT_RUN) exit 3 ;; *) exit 1 ;; esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
