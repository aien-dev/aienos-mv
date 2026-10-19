#!/usr/bin/env bash
# AIENOS_CK_TASK gate (aienos-mv#429, TEST-ONLY): native pipeline task lifecycle. Three distinct admitted Omega stages
# (native/kernel/tests/fixtures/task: tgen, txform, tsink; real oscc output, TEST signer, each with its own checker2
# realization receipt) are written into the sealed C Store of a GPT boot disk image. One boot of the qualification image
# built with CK_SEED0B_TEST_ANCHOR=1 CK_TASK_TEST=1: the kernel admits them and core/task_test.c runs pipelines of them
# over native pipes (core/task.c, core/pipe.c), every step one ck_osc_launch (receipt verified, EL0, torn down).
#
# Shown: two stages alive at once with bytes in flight over one bounded 64 KiB pipe and a slow consumer (the producer
# blocks on the full pipe: backpressure), exact bytes at the end; a third stage; negative controls for a nonzero exit
# (consumer exits early, endless producer stopped by BROKEN_PIPE), a crash (a real OSC BOUNDS trap mid-pipeline),
# cancellation (deadline, interrupt with grace) and a partial spawn (third stage's receipt missing: PARTIAL_LAUNCH, the
# two started stages reaped, nothing ran); spawn refusals before any effect; least-privilege stage ends. Every check
# also requires no leaked task, pipe or capability, no polling of a blocked task and every EL0 slot torn down.
#
# Baselines (aienos-mv#432, core/pipe_bench.h): 2- and 3-stage throughput, one-byte and first-byte latency, cancellation
# time, 64 KiB capacity, revoke of a blocked reader and writer, static footprint. Printed as "BASELINE pipe_bench: ..."
# lines. They are EMULATOR numbers (QEMU TCG on the Spark), never hardware numbers, and have no pass threshold.
#
# Scheduling, stated plainly: one CPU, cooperative at step granularity (one bounded EL0 step of one stage at a time,
# round robin); a step is never preempted and resumed. Not parallel execution on two cores.
#
# Label: QEMU (aarch64 virt), TEST signer, TEST build, not physical. Nothing here ran on a Spark, Pi 5, iMac or any real
# machine. The only keys are THROWAWAY TEST keys. Last line: AIENOS_CK_TASK: PASS|FAIL|NOT_RUN.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
verdict=AIENOS_CK_TASK

code_fd="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
vars_fd="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 not installed"; echo "${verdict}: NOT_RUN"; exit 2; }
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "AAVMF firmware not found"; echo "${verdict}: NOT_RUN"; exit 2; }

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take "qemu_ck_task_test" "${AIENOS_GATE_MINUTES:-60}"; then
    echo "NOT_RUN  ${gh_why}"
    echo "${verdict}: NOT_RUN"
    exit 3
fi
release_flag() { gh_lock_release; }
work="$(mktemp -d)"
cleanup() { rm -rf "${work}"; release_flag; }
trap cleanup EXIT
failed=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; failed=1; }

cross=""
if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
out_prod="${AIENOS_CK_OUT_PROD:-${repo_root}/target/native-kernel}"
out_task="${AIENOS_CK_OUT_TASK:-${repo_root}/target/native-kernel-test-task}"
mk() { nice make -s -j8 -C native/kernel CROSS="${cross}" AIENOS_COMMIT="${commit}" "$@" >/dev/null; }

fix="native/kernel/tests/fixtures/task"
# the committed fixtures are the ones PROVENANCE.txt names
for f in tgen.osc tgen.unit tgen.rcpt txform.osc txform.unit txform.rcpt tsink.osc tsink.unit tsink.rcpt; do
    if grep -qE "^  ${f//./\\.} +sha256=$(sha256sum "${fix}/${f}" | cut -c1-64)" "${fix}/PROVENANCE.txt"; then :
    else fail "${f} matches its sha256 in PROVENANCE.txt"; fi
done
[[ "${failed}" == 0 ]] && pass "the nine committed fixture files match PROVENANCE.txt"

# build refusals: the TEST-only flag never reaches a hardware or service image, needs the TEST anchor, stands alone
refuse() { # description, make arguments...
    local d=$1
    shift
    if make -n -C native/kernel OUT="${work}/refuse" "$@" all >"${work}/refuse.log" 2>&1; then fail "build refused: ${d}"
    elif grep -q "CK_TASK_TEST\|CK_SERVICE cannot be combined with a CK_TEST" "${work}/refuse.log"; then pass "build refused: ${d}"
    else fail "build refused for the stated reason: ${d} ($(grep -m1 '\*\*\*' "${work}/refuse.log"))"; fi
}
refuse "CK_TASK_TEST=1 with CK_HARDWARE_STAGING" CK_TASK_TEST=1 CK_SEED0B_TEST_ANCHOR=1 CK_HARDWARE_STAGING=1
refuse "CK_TASK_TEST=1 with CK_SERVICE" CK_TASK_TEST=1 CK_SEED0B_TEST_ANCHOR=1 CK_SERVICE=1
refuse "CK_TASK_TEST=1 without the TEST anchor" CK_TASK_TEST=1
refuse "CK_TASK_TEST=2" CK_TASK_TEST=2 CK_SEED0B_TEST_ANCHOR=1
refuse "CK_TASK_TEST=1 with CK_PIPE_TEST=1" CK_TASK_TEST=1 CK_SEED0B_TEST_ANCHOR=1 CK_PIPE_TEST=1

mk OUT="${out_task}" CK_SEED0B_TEST_ANCHOR=1 CK_TASK_TEST=1 full
mk OUT="${out_prod}" all store-image gpt-image
if grep -aqF "TEST-ONLY task self-test" "${out_prod}/BOOTAA64.EFI"; then fail "the default image carries the TEST-only task self-test"
else pass "the default image carries no trace of the TEST-only task self-test"; fi
grep -aqF "TEST-ONLY task self-test" "${out_task}/full/BOOTAA64.EFI" && pass "the task image announces itself TEST-ONLY" \
    || fail "the task image announces itself TEST-ONLY"
simg="${out_prod}/host/ck_store_image"

files=("${fix}/tgen.unit" "${fix}/txform.unit" "${fix}/tsink.unit" "${fix}/tgen.rcpt" "${fix}/txform.rcpt" "${fix}/tsink.rcpt")
disk="${work}/nvme.img"
"${out_prod}/host/ck_gpt_image" create "${disk}" 512 64 aienos-middle >/dev/null || fail "GPT boot disk image created"
if "${simg}" build "${disk}" 512 "${files[@]}" >"${work}/build.txt" 2>&1; then pass "boot disk built: $(head -1 "${work}/build.txt")"
else fail "boot disk build: $(head -3 "${work}/build.txt" | tr '\n' ' ')"; fi
"${simg}" list "${disk}" 512 >"${work}/list.txt" 2>&1 || fail "host read-back of the boot disk Store"
for f in "${files[@]}"; do
    grep -qxF "$(basename "${f}") bytes=$(stat -c %s "${f}") sha256=$(sha256sum "${f}" | cut -d' ' -f1)" "${work}/list.txt" \
        || fail "host read-back of $(basename "${f}")"
done

boot() { # image, serial text output, disk image
    local esp="${work}/esp" log="${work}/serial.log" started status
    rm -rf "${esp}"
    mkdir -p "${esp}/EFI/BOOT" "${esp}/EFI/AIENOS"
    touch "${esp}/EFI/AIENOS/BOOTREPORT.TXT"
    cp "$1" "${esp}/EFI/BOOT/BOOTAA64.EFI"
    cp "${vars_fd}" "${work}/vars.fd"
    started=$(date +%s)
    set +e
    nice -n 10 timeout "${AIENOS_QEMU_TIMEOUT:-900}" qemu-system-aarch64 \
        -M virt,virtualization=on,gic-version=3,iommu=smmuv3 -accel tcg,thread=single -cpu max -smp 4 -m 2048 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/vars.fd" \
        -drive if=none,id=esp,format=raw,file=fat:rw:"${esp}" \
        -device virtio-blk-pci,drive=esp \
        -drive if=none,id=nvme0,format=raw,file="$3" \
        -device nvme,drive=nvme0,serial=aienos-nvme-test \
        -device ramfb -display none -nic none \
        -serial file:"${log}" -no-reboot
    status=$?
    set -e
    tr -d '\r' <"${log}" >"$2"
    echo "qemu exit ${status} after $(( $(date +%s) - started )) s (commit ${commit:0:12})"
    [[ "${status}" == 0 ]] || fail "QEMU exit status ${status} (124 = timeout)"
}
has() { grep -qE -- "$2" "$1"; }
check() { if has "$1" "$3"; then pass "$2"; else fail "$2"; fi; }

serial="${work}/serial.txt"
boot "${out_task}/full/BOOTAA64.EFI" "${serial}" "${disk}"
release_flag

label='QEMU \(aarch64 virt\), TEST signer, TEST build, not physical'
tc() { # check name, description, extra regex inside the line
    check "${serial}" "$2" "^task_check: name=$1 result=PASS $3.* clean=1; ${label}\$"
}
check "${serial}" "left firmware and entered the kernel" "kernel: alive"
check "${serial}" "kernel read all six candidates (three units, three receipts) from the boot disk Store" "^artifact_candidates: 6$"
if has "${serial}" "^report-truncated:"; then fail "kernel report lines were truncated"; else pass "no kernel report line was truncated"; fi
for u in tgen txform tsink; do
    check "${serial}" "${u}.unit admitted (TEST signer, one function)" "^osc_unit: ${u}\\.unit ACCEPT .* funcs=1 caps=0 signer=TEST$"
    check "${serial}" "${u}.rcpt stored for verification at launch" "^osc_receipt: ${u}\\.rcpt STORED bytes=416 "
done
check "${serial}" "self-test announces itself TEST-ONLY and cooperative-step scheduling" \
    "^task_test: TEST-ONLY task self-test begin .*scheduling=cooperative-step .*; ${label}\$"
check "${serial}" "two admitted stages alive at once, bytes in flight, producer blocked on the full pipe" \
    "^task_check: name=two_stage_mid_run_both_alive result=PASS gen_state=[12] sink_state=[12] gen_out=[0-9]+ sink_in=[1-9][0-9]* in_flight=[1-9][0-9]* gen_blocked_on_full_pipe=[1-9][0-9]* clean=alive; ${label}\$"
tc two_stage_backpressure_exact_bytes "two stages: 163840 bytes (NULs included) arrive exact, interleaved, bounded by 64 KiB" \
    "bytes=163840 sink_count=163840 hash_ok=1 status=0,0 steps=[0-9]+ switches=[0-9]+ first_sink_step=[0-9]+ last_gen_step=[0-9]+ blocks=[1-9][0-9]* wakeups=[1-9][0-9]* max_resident=[0-9]+ capacity=65536"
tc three_stage_gen_xform_sink "a third stage: gen | xform | sink, exact transformed bytes" "bytes=100003 key=0x5a status=0,0,0 hash_ok=1"
tc nonzero_exit_consumer_exits_early "nonzero exit: last stage status 3 is the pipeline status, endless producer ends 141" \
    "pipeline_status=3 endless_producer_status=141"
tc crash_trap_mid_pipeline "crash: the middle stage traps (134), producer 141, sink sees EOF" "status=141,134,0 "
tc cancel_deadline_interrupt "cancel: deadline passes with the pipeline alive, interrupt ends both (130)" \
    "deadline_ticks=30 waited_ticks=[0-9]+ steps_before_cancel=[1-9][0-9]* status=130,130"
tc partial_spawn_missing_receipt "partial spawn: missing receipt on stage 3 -> PARTIAL_LAUNCH, two started stages reaped, no step ran" \
    "result=PARTIAL_LAUNCH started=2 cause=DENIED receipt_refusal=64 .* steps_run=0"
tc spawn_refused_before_any_effect "spawn refusals before any effect (another program's receipt, unknown entry, principal, domain)" \
    "other_programs_receipt=DENIED\\(refusal [1-9][0-9]*\\) unknown_entry=NOT_FOUND wrong_principal=DENIED hosted_domain=CAP_DOMAIN_MISMATCH steps_run=0"
tc least_privilege_endpoints "least privilege: stage ends hold exactly READ (0x1) / WRITE (0x2)" \
    "session_end_rights=0x[0-9a-f]+ stage_stdin_rights=0x1 stage_stdout_rights=0x2"
# aienos-mv#432 baselines (core/pipe_bench.h): each scenario checks its own bytes, statuses and cleanup; the numbers are
# emulator numbers (QEMU TCG), printed below and kept in the log, with no pass threshold (pipe_bench.h B1).
bl="; ${label}, emulator numbers\$"
bc() { check "${serial}" "baseline $1: $2" "^pipe_bench: case=$1 result=PASS $3.*${bl}"; }
for c in two_stage_163840 two_stage_1048576 three_stage_163840 three_stage_1048576; do
    n=${c##*_}; s=${c%%_*}; s=$([[ ${s} == two ]] && echo 2 || echo 3)
    bc "${c}" "${s} admitted stages move ${n} bytes exact, 64 KiB bound, no leak" \
        "stages=${s} bytes=${n} chunk=4096 runs=3 wall_us_min=[0-9]+ wall_us_median=[1-9][0-9]* .* max_resident=[0-9]+ capacity=65536 bad_runs=0 leaked_runs=0"
done
bc tiny_one_byte "one byte through two stages (end-to-end latency)" "stages=2 bytes=1 .*bad_runs=0 leaked_runs=0"
bc first_byte "first-byte latency" "first_byte_us=[0-9]+ turns_to_first_byte=[1-9][0-9]* first_read_bytes=[1-9]"
bc cancel_endless "cancellation of an endless pipeline, both reaped 130" "bytes_before_cancel=[1-9][0-9]* cancel_us=[0-9]+ cancel_steps=[0-9]+ terminated=[0-9]+ status=130,130"
bc capacity_64k "exactly 65536 bytes fit, then WOULD_BLOCK; drained in order" "capacity_bytes=65536 would_block_when_full=1 drained=65536 out_of_order_bytes=0 write_after_drain=1 max_resident=65536"
bc revoke_blocked_reader "revoke wakes a blocked reader stage (137)" "side=reader blocked=1 wait=1 .*status=137"
bc revoke_blocked_writer "revoke wakes a writer stage blocked on a full pipe (137)" "side=writer blocked=1 wait=2 resident_at_revoke=65536 .*status=137"
bc footprint "static footprint, no allocation" "pipe_pool_bytes=[1-9][0-9]* task_tab_bytes=[1-9][0-9]* .*allocation=none"
grep -E "^pipe_bench: " "${serial}" | sed "s/^/BASELINE /" || true
check "${serial}" "self-test done: 20 checks (9 lifecycle, 11 baselines), none failed, every EL0 step torn down" \
    "^task_test: done checks=20 failed=0 launches=[1-9][0-9]* not_torn_down=0 scheduling=cooperative-step; ${label}\$"
if has "${serial}" "^task_check: .*result=FAIL"; then fail "no task_check line failed"; else pass "no task_check line failed"; fi
if has "${serial}" "^pipe_bench: .*result=FAIL"; then fail "no pipe_bench line failed"; else pass "no pipe_bench line failed"; fi
# the gate can go red: the same checks on a copy of the log with one wrong byte count must fail
sed 's/bytes=163840 sink_count=163840/bytes=163840 sink_count=163839/' "${serial}" >"${work}/wrong.txt"
if has "${work}/wrong.txt" "^task_check: name=two_stage_backpressure_exact_bytes result=PASS bytes=163840 sink_count=163840 "; then
    fail "control: a wrong byte count still matched (the gate cannot go red)"
else pass "control: a log with one wrong byte count does not match (the gate can go red)"; fi

if [[ -n "${AIENOS_LOG_DIR:-}" ]]; then cp "${serial}" "${AIENOS_LOG_DIR}/qemu_ck_task_serial.log"; fi
if [[ "${failed}" != 0 || -n "${AIENOS_QEMU_VERBOSE:-}" ]]; then
    echo "---- serial (task/osc lines) ----"; grep -E "^(task_|osc_|artifact|kernel|panic|fault)" "${serial}" | head -80 || true
fi
echo "label: QEMU (aarch64 virt), TEST signer, TEST build, not physical"
if [[ "${failed}" == 0 ]]; then echo "${verdict}: PASS"; else echo "${verdict}: FAIL"; exit 1; fi
