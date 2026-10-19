#!/usr/bin/env bash
# AIENOS_CK_OSH_OP gate (OSH-AIENOS-OP, aien-architecture#158): a REAL capability-authorized operation through the shared
# OSH interface. The same three compiled shell units (lex_run, parse_run, expand_run) run as admitted EL0 tasks; on a ready
# pipeline the request record goes to the adapter's operation layer (native/kernel/core/osh_op.c) instead of a fake status.
# Operation: `klog <words>` writes "osh_op_out: <words>" to the console. Capability: WRITE on the console-log resource in
# the session's kernel IPC capability table (domain 1). The OSH_CORE gate (qemu_ck_osh_core_test.sh, fixture traces) is
# unchanged and separate; this gate does not run the fixtures.
#
# Label: QEMU (aarch64 virt), TEST signer, not physical. Nothing here ran on a Spark or any real machine.
# Cases (boot 1 of 3, CK_SEED0B_TEST_ANCHOR=1 CK_OSH_TEST=1 CK_OSH_OP_TEST=1, image full-osh-op-test):
#   allow           WRITE held: two lines and two "grant used" receipts on the console (the second shows $? = the real status 0 flowing back into the shell core)
#   deny_no_cap     no capability: DENIED, no bytes, outcome FAILED_NO_EFFECT, a security receipt line
#   deny_read_only  READ only: DENIED, no bytes
#   revoked         revoked between request build and effect: REVOKED, no bytes
#   interrupt       interrupt before pipeline 2 of 3: pipeline 1 done, 2 CANCELLED, 3 never runs, status 130
#   lost            adapter lost after the effect: bytes appear once; restart reports OUTCOME_UNKNOWN with the digest, never replays
#   after_restart   a new request after the loss is a new decision and runs
#   unsupported     klog in a pipe, klog with a redirection, unknown command: NOT_SUPPORTED / NOT_FOUND before any effect
# aienos-mv#430 native stage pipelines (gen = tgen.unit, xform = txform.unit, sink = tsink.unit, admitted with receipts):
#   stream3         gen | xform | sink, 160 KiB through 64 KiB pipes: all three alive at once, backpressure, sink's count and
#                   hash checked against an independent computation, $? = the last command's status
#   last_status     the pipeline status is the last command's (xform exits 3 when last, sink's 0 when it follows)
#   redir           >p >o in record order, truncate, a location read back by a second pipeline, append
#   redir_dup       2>&1 >o through the real shell units: o gets stdout (the fd mapping itself is checked by test_osh_op)
#   not_found       a missing middle command: 127 for it only, its pipe ends closed, the others run
#   nounit          a stage unit with no receipt: refused at its spawn (126), the launcher counts the refusal
#   foreign         a name of a class this adapter does not provide: UNAVAILABLE for the whole pipeline, nothing starts
#   no_spawn_cap    no spawn capability: DENIED before any spawn
#   loc_read_only   a write redirection with READ only on the location capability: DENIED, status 1
#   revoke_between  the spawn capability revoked after command 0 started: command 1 REVOKED
#   partial         a launch failure after two spawns: PARTIAL_LAUNCH, both cancelled and reaped, status 1
#   lost_stage      the adapter lost after a spawn: OUTCOME_UNKNOWN, the kernel reaps the stage, a restart never replays
#   interrupt_stages, deadline   an endless producer stopped by an interrupt or by the session deadline
#   direct, malicious, direct_no_cap   the direct-argv route: same executor, malformed records refused, same authority
# every case: capability released at the end, no EL0 task slot left held.
# aienos-mv#431 effect journal (durable intent before every effect, outcome after, receipt last; recovery never replays).
# The #429/#430 cases above run on a labelled RAM journal (durable=0, never counted as durable). The durable cases run on
# the canonical Cortex over the boot disk's sealed TEST-key Store, three boots of the SAME disk:
#   boot 1 (fresh)    d_ok, d_write, then a loss at every crash point of a one-stage pipeline (before intent, after
#                     intent, after spawn, after STARTED, after finish, during cancellation, during the receipt), each
#                     followed by a read-only look (no OSH session, writes refused), recovery and a second scan; the
#                     operator reconciles one OUTCOME_UNKNOWN effect (a non-operator is DENIED, a second verdict refused)
#   boot 2 (powercut) gen | sink loses its adapter after both spawns are STARTED and the machine halts; the host kills QEMU
#   boot 3 (recover)  read-only look before any shell unit runs, recovery (2 x OUTCOME_UNKNOWN, nothing re-spawned),
#                     a fresh decision runs once, the journal is filled to its 64-chunk limit, then effects are refused
#                     by name (FAILED_NO_EFFECT, nothing spawned)
# Last line: AIENOS_CK_OSH_OP: PASS|FAIL|NOT_RUN.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
verdict=AIENOS_CK_OSH_OP
code_fd="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
vars_fd="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 not installed"; echo "${verdict}: NOT_RUN"; exit 2; }
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "AAVMF firmware not found"; echo "${verdict}: NOT_RUN"; exit 2; }

# Machine quiet flag (read only) and the QEMU gate lock (one gate at a time):
# scripts/lib_gate_hold.sh (aienos#278).
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take "qemu_ck_osh_op_test" "${AIENOS_GATE_MINUTES:-60}"; then
    echo "NOT_RUN  ${gh_why}"
    echo "${verdict}: NOT_RUN"
    exit 3
fi
# Release only this run's own gate lock record, at most once.
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
out_osh="${AIENOS_CK_OUT_OSH:-${repo_root}/target/native-kernel-osh-op-test}"
mk() { make -s -C native/kernel CROSS="${cross}" AIENOS_COMMIT="${commit}" "$@" >/dev/null; }

fix="native/kernel/tests/fixtures/osh"
mk OUT="${out_osh}" CK_SEED0B_TEST_ANCHOR=1 CK_OSH_TEST=1 CK_OSH_OP_TEST=1 full
make -s -C native/kernel OUT="${out_prod}" store-image gpt-image >/dev/null
simg="${out_prod}/host/ck_store_image"

# aienos#383: each launch needs the unit's realization receipt
files=("${fix}/osh_lex.unit" "${fix}/osh_parse.unit" "${fix}/osh_expand.unit" "${fix}/../osc_receipt/osh_lex.rcpt" "${fix}/../osc_receipt/osh_parse.rcpt" "${fix}/../osc_receipt/osh_expand.rcpt")
# aienos-mv#430: the three stage units of the task gate and their receipts (the native operations gen, xform, sink)
tfix="native/kernel/tests/fixtures/task"
files+=("${tfix}/tgen.unit" "${tfix}/txform.unit" "${tfix}/tsink.unit" "${tfix}/tgen.rcpt" "${tfix}/txform.rcpt" "${tfix}/tsink.rcpt")
disk="${work}/nvme.img"
"${out_prod}/host/ck_gpt_image" create "${disk}" 512 64 aienos-middle >/dev/null || fail "GPT boot disk image created"
if "${simg}" build "${disk}" 512 "${files[@]}" >"${work}/build.txt" 2>&1; then pass "boot disk built: $(head -1 "${work}/build.txt")"
else fail "boot disk build: $(head -3 "${work}/build.txt" | tr '\n' ' ')"; fi

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

# boot_kill: as boot, but stops QEMU with SIGKILL once the HALT line is on the console (a power cut, no reset path)
boot_kill() { # image, serial text output, disk image
    local esp="${work}/esp" log="${work}/serial.log" pid deadline
    rm -rf "${esp}"
    mkdir -p "${esp}/EFI/BOOT" "${esp}/EFI/AIENOS"
    touch "${esp}/EFI/AIENOS/BOOTREPORT.TXT"
    cp "$1" "${esp}/EFI/BOOT/BOOTAA64.EFI"
    cp "${vars_fd}" "${work}/vars.fd"
    : >"${log}"
    nice -n 10 qemu-system-aarch64 \
        -M virt,virtualization=on,gic-version=3,iommu=smmuv3 -accel tcg,thread=single -cpu max -smp 4 -m 2048 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/vars.fd" \
        -drive if=none,id=esp,format=raw,file=fat:rw:"${esp}" \
        -device virtio-blk-pci,drive=esp \
        -drive if=none,id=nvme0,format=raw,file="$3" \
        -device nvme,drive=nvme0,serial=aienos-nvme-test \
        -device ramfb -display none -nic none \
        -serial file:"${log}" -no-reboot &
    pid=$!
    deadline=$(( $(date +%s) + ${AIENOS_QEMU_TIMEOUT:-900} ))
    while (( $(date +%s) < deadline )); do
        grep -qE '^osh_op_powercut: (HALT|NOT_HALTED)' "${log}" 2>/dev/null && break
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.2
    done
    kill -9 "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
    tr -d '\r' <"${log}" >"$2"
    echo "qemu killed at the HALT line (commit ${commit:0:12})"
}

efi="${out_osh}/full-osh-op-test/BOOTAA64.EFI"
serial1="${work}/serial1.txt" serial2="${work}/serial2.txt" serial3="${work}/serial3.txt"
boot "${efi}" "${serial1}" "${disk}"
boot_kill "${efi}" "${serial2}" "${disk}"
boot "${efi}" "${serial3}" "${disk}"
release_flag
# the #429/#430 checks read boot 1 up to the end of those cases; the #431 checks read each boot whole
serial="${work}/serial.txt"
sed '/^osh_op_done: cases=25;/q' "${serial1}" >"${serial}"

label='QEMU \(aarch64 virt\), TEST signer, not physical'
count() { grep -cE -- "$2" "$1" || true; }
exact() { # description, regex, expected count
    local n; n="$(count "${serial}" "$2")"
    if [[ "${n}" == "$3" ]]; then pass "$1 (${n})"; else fail "$1: expected $3, got ${n}"; fi
}
generic() { # serial, boot name, 1 if the boot ends normally
    check "$1" "$2: left firmware and entered the kernel" "kernel: alive"
    check "$1" "$2: kernel read all twelve candidates (three shell units, three stage units, six receipts) from the boot disk Store" "^artifact_candidates: 12$"
    for u in osh_lex osh_parse osh_expand; do
        check "$1" "$2: ${u} admitted (real oscc output, TEST signer)" "^osc_unit: ${u}\\.unit ACCEPT unit_digest=[0-9a-f]{64} program_id=[0-9a-f]{64} funcs=[0-9]+ caps=0 signer=TEST$"
    done
    check "$1" "$2: core line: 3 units" "^osh_core: units=3 ws_cells=11456 ws_pages=23 input_max=4096; ${label}\$"
    if [[ "$3" == 1 ]]; then check "$1" "$2: report reached the console" "report_kind: final"; fi
    if has "$1" "report_kind: (panic|fault)"; then fail "$2: panic or fault reported"; else pass "$2: no kernel panic or fault report"; fi
    if has "$1" "^osh_fault:"; then fail "$2: a shell launch did not return"; else pass "$2: every shell launch returned"; fi
}
generic "${serial1}" "boot 1" 1
generic "${serial2}" "boot 2 (power cut)" 0
generic "${serial3}" "boot 3" 1
exact "no fixture trace ran in this build" "^osh_script_begin" 0
check "${serial}" "all 25 cases ran (8 klog, 17 native stage pipelines)" "^osh_op_done: cases=25; ${label}\$"

P="^osh_op_pipeline: case="
E="^osh_op_end: case="
# allow
exact "allow: the real effect, line 1 on the console" "^osh_op_out: hello from osh\$" 1
exact "allow: line 2 carries \$? = the real status 0 back through the shell core" "^osh_op_out: status=0\$" 1
exact "allow: both pipelines COMPLETED with status 0" "${P}allow n=[12] status=0 error=OK outcome=COMPLETED stop=0 crashed=0 request_digest=[0-9a-f]{64}; ${label}\$" 2
check "${serial}" "allow: 2 console writes, no task slot held, capability released" "${E}allow rc=0 final_status=0 sink_calls=2 launches=[0-9]+ slots_held=0 cap_table_empty=1 cap_released=1; ${label}\$"
# denials
for c in deny_no_cap deny_read_only; do
    check "${serial}" "${c}: refused DENIED before any effect (status 126, FAILED_NO_EFFECT)" "${P}${c} n=1 status=126 error=DENIED outcome=FAILED_NO_EFFECT stop=0 crashed=0 request_digest=[0-9a-f]{64}; ${label}\$"
    check "${serial}" "${c}: no console write, capability released" "${E}${c} rc=0 final_status=126 sink_calls=0 launches=[0-9]+ slots_held=0 cap_table_empty=1 cap_released=1; ${label}\$"
    check "${serial}" "${c}: a security receipt names the denial, principal and capability" "^osh_op_receipt: seq=1 op=console_write result=DENIED request_digest=[0-9a-f]{64} effect=none principal=[0-9a-f]{64} cap_domain=1 cap_index=[0-9]+ cap_generation=[0-9]+ resource_class=1 operation=1 case=${c}; ${label}\$"
done
check "${serial}" "revoked: REVOKED at the effect boundary, FAILED_NO_EFFECT" "${P}revoked n=1 status=126 error=REVOKED outcome=FAILED_NO_EFFECT stop=0 crashed=0 "
check "${serial}" "revoked: no console write, capability released" "${E}revoked rc=0 final_status=126 sink_calls=0 launches=[0-9]+ slots_held=0 cap_table_empty=1 cap_released=1; "
check "${serial}" "revoked: receipt names REVOKED" "^osh_op_receipt: seq=1 op=console_write result=REVOKED "
exact "no refused case wrote any secret marker" "^osh_op_out: .*SECRET" 0
# interrupt
check "${serial}" "interrupt: pipeline 1 completed" "${P}interrupt n=1 status=0 error=OK outcome=COMPLETED "
check "${serial}" "interrupt: pipeline 2 INTERRUPTED, CANCELLED, list stops, status 130" "${P}interrupt n=2 status=130 error=INTERRUPTED outcome=CANCELLED stop=1 crashed=0 "
exact "interrupt: pipeline 3 never ran" "${P}interrupt n=3 " 0
check "${serial}" "interrupt: one console write, status 130, capability released, no task slot held" "${E}interrupt rc=1 final_status=130 sink_calls=1 launches=[0-9]+ slots_held=0 cap_table_empty=1 cap_released=1; "
exact "interrupt: 'one' printed, 'two' and 'three' not" "^osh_op_out: one\$" 1
exact "interrupt: 'two'/'three' absent" "^osh_op_out: (two|three)\$" 0
# lost
exact "lost: the effect happened exactly once" "^osh_op_out: LOST-MARKER\$" 1
check "${serial}" "lost: outcome open (OUTCOME_UNKNOWN, crashed)" "${P}lost n=1 status=70 error=OUTCOME_UNKNOWN outcome=OUTCOME_UNKNOWN stop=1 crashed=1 "
check "${serial}" "lost: restart reports the open intent as OUTCOME_UNKNOWN with its digest, replayed=0" "^osh_op_record: seq=1 outcome=OUTCOME_UNKNOWN request_digest=[0-9a-f]{64} replayed=0 .* case=lost; "
check "${serial}" "lost: restart found 1 open intent, a 2nd scan 0, no console write added, restarted adapter holds no capability" "^osh_op_restart: case=lost open_intents=1 second_scan=0 sink_calls_before=1 sink_calls_after=1 held_cap=0; "
exact "lost: still exactly one LOST-MARKER after restart (never replayed)" "^osh_op_out: LOST-MARKER\$" 1
check "${serial}" "lost: capability released" "${E}lost rc=1 final_status=70 sink_calls=1 launches=[0-9]+ slots_held=0 cap_table_empty=1 cap_released=1; "
exact "after_restart: a new request is a new decision and runs" "^osh_op_out: fresh decision\$" 1
# unsupported
check "${serial}" "unsupported: a pipe is NOT_SUPPORTED before any effect" "${P}unsupported n=1 status=126 error=NOT_SUPPORTED outcome=NOT_STARTED "
check "${serial}" "unsupported: a redirection is NOT_SUPPORTED" "${P}unsupported n=2 status=126 error=NOT_SUPPORTED outcome=NOT_STARTED "
check "${serial}" "unsupported: an unknown command is NOT_FOUND (127)" "${P}unsupported n=3 status=127 error=NOT_FOUND outcome=NOT_STARTED "
check "${serial}" "unsupported: no console write, no task slot held" "${E}unsupported rc=0 final_status=127 sink_calls=0 launches=[0-9]+ slots_held=0 cap_table_empty=1 cap_released=1; "
# ---- aienos-mv#430: native stage pipelines (gen = tgen.unit, xform = txform.unit, sink = tsink.unit) ----
for u in tgen txform tsink; do
    check "${serial}" "${u} admitted (real oscc output, TEST signer)" "^osc_unit: ${u}\\.unit ACCEPT unit_digest=[0-9a-f]{64} program_id=[0-9a-f]{64} funcs=[0-9]+ caps=0 signer=TEST$"
done
exact "every stage case started (no NOT_RUN)" "^osh_op_stages: NOT_RUN" 0
S="^osh_op_stages: case="
N="^osh_op_native_end: case="
check "${serial}" "stream3: three distinct admitted stages, COMPLETED, status 0" "${P}stream3 n=1 status=0 error=OK outcome=COMPLETED stop=0 crashed=0 "
check "${serial}" "stream3: all three ran at once; sink counted 163840 bytes and its hash matches gen then xform 90, computed independently" "${S}stream3 n=1 launch=OK started=3 cmds=3 c0=0/COMPLETED/OK c1=0/COMPLETED/OK c2=0/COMPLETED/OK stdout_bytes=16 sink_count=163840 sink_hash_ok=1 max_in_pipes=[0-9]+ mid_alive=3 mid_in_flight=[0-9]+; "
check "${serial}" "stream3: backpressure: a 64 KiB pipe filled to capacity while the consumers were slower" "${S}stream3 n=1 .* max_in_pipes=65536 "
exact "stream3: \$? = 0 flows back into the shell" "^osh_op_out: stream3 status=0\$" 1
exact "stream3: three spawn receipts, each after its grant" "^osh_op_receipt: seq=1 cmd=[012] op=task_spawn result=OK .* case=stream3; " 3
check "${serial}" "last_status: xform last exits 3, the pipeline status is 3" "${P}last_status n=1 status=3 error=OK outcome=COMPLETED "
exact "last_status: \$? = 3 in the shell" "^osh_op_out: last=3\$" 1
check "${serial}" "last_status: with sink after it, the pipeline status is sink's 0" "${P}last_status n=3 status=0 error=OK outcome=COMPLETED "
exact "last_status: \$? = 0 in the shell" "^osh_op_out: last=0\$" 1
check "${serial}" "redir: >p >o in record order: p created empty, o gets the 100 bytes" "${S}redir n=1 launch=OK started=1 cmds=1 c0=0/COMPLETED/OK loc_[op]=[0-9]+ loc_[op]=[0-9]+ stdout_bytes=0 " 
check "${serial}" "redir: p is 0 bytes" "${S}redir n=1 .* loc_p=0 "
check "${serial}" "redir: o is 100 bytes" "${S}redir n=1 .* loc_o=100 "
check "${serial}" "redir: >o truncates, then holds 1000 bytes" "${S}redir n=2 .* loc_o=1000 "
check "${serial}" "redir: xform 7 <o | sink reads the location back: 1000 bytes, hash matches" "${S}redir n=3 launch=OK started=2 cmds=2 c0=0/COMPLETED/OK c1=0/COMPLETED/OK .* stdout_bytes=16 sink_count=1000 sink_hash_ok=1 "
check "${serial}" "redir: >>o appends 10 bytes" "${S}redir n=4 .* loc_o=1010 "
check "${serial}" "redir_dup: 2>&1 >o: stdout to o (100 bytes), stderr where stdout was" "${S}redir_dup n=1 launch=OK started=1 cmds=1 c0=0/COMPLETED/OK .*loc_o=100 "
check "${serial}" "not_found: the missing middle command is 127 and ends only itself" "${S}not_found n=1 launch=OK started=2 cmds=3 c0=[0-9]+/COMPLETED/[A-Z_]+ c1=127/FAILED_NO_EFFECT/NOT_FOUND c2=0/COMPLETED/OK stdout_bytes=16 sink_count=0 sink_hash_ok=1 "
check "${serial}" "not_found: the pipeline status is the last command's (0)" "${P}not_found n=1 status=0 "
check "${serial}" "nounit: a unit with no receipt is refused at its spawn (126), nothing of it runs" "${S}nounit n=1 launch=OK started=1 cmds=2 c0=[0-9]+/COMPLETED/[A-Z_]+ c1=126/FAILED_NO_EFFECT/DENIED "
check "${serial}" "nounit: pipeline status 126" "${P}nounit n=1 status=126 "
check "${serial}" "nounit: the launcher refused the receipt" "${N}nounit tasks_empty=1 pipes_empty=1 stage_steps=[0-9]+ not_torn_down=0 admit_refused=1; "
check "${serial}" "foreign: a name of a class this adapter does not provide refuses the whole pipeline (UNAVAILABLE, 126), nothing started" "${P}foreign n=1 status=126 error=UNAVAILABLE outcome=NOT_STARTED "
check "${serial}" "foreign: no stage started" "${S}foreign n=1 launch=UNAVAILABLE started=0 "
check "${serial}" "no_spawn_cap: no spawn capability: DENIED before any spawn (126)" "${P}no_spawn_cap n=1 status=126 error=DENIED outcome=FAILED_NO_EFFECT "
check "${serial}" "no_spawn_cap: nothing started" "${S}no_spawn_cap n=1 launch=DENIED started=0 "
exact "no_spawn_cap: the spawn denial has its receipt" "^osh_op_receipt: seq=1 cmd=0 op=task_spawn result=DENIED .* case=no_spawn_cap; " 1
check "${serial}" "loc_read_only: a write redirection needs WRITE on the location capability: DENIED, status 1, no location made" "${S}loc_read_only n=1 launch=OK started=0 cmds=1 c0=1/FAILED_NO_EFFECT/DENIED stdout_bytes=0 "
check "${serial}" "revoke_between: the spawn capability revoked after command 0 started: command 1 REVOKED (126)" "${S}revoke_between n=1 launch=OK started=1 cmds=2 c0=[0-9]+/COMPLETED/[A-Z_]+ c1=126/FAILED_NO_EFFECT/REVOKED "
check "${serial}" "revoke_between: pipeline status 126" "${P}revoke_between n=1 status=126 "
check "${serial}" "partial: a launch failure after two started: PARTIAL_LAUNCH, status 1" "${P}partial n=1 status=1 error=PARTIAL_LAUNCH "
check "${serial}" "partial: both started stages CANCELLED, the failing one FAILED_NO_EFFECT" "${S}partial n=1 launch=PARTIAL_LAUNCH started=2 cmds=3 c0=[0-9]+/CANCELLED/[A-Z_]+ c1=[0-9]+/CANCELLED/[A-Z_]+ c2=1/FAILED_NO_EFFECT/LIMIT "
check "${serial}" "lost_stage: lost after the first spawn: OUTCOME_UNKNOWN, the list stops" "${P}lost_stage n=1 status=70 error=OUTCOME_UNKNOWN outcome=OUTCOME_UNKNOWN stop=1 crashed=1 "
check "${serial}" "lost_stage: the restart reports the open spawn intent once, never replays, holds no capability" "^osh_op_restart: case=lost_stage open_intents=1 second_scan=0 sink_calls_before=0 sink_calls_after=0 held_cap=0; "
check "${serial}" "interrupt_stages: an interrupt on an endless producer: INTERRUPTED, CANCELLED, the list stops" "${P}interrupt_stages n=1 status=[0-9]+ error=INTERRUPTED outcome=CANCELLED stop=1 crashed=0 "
check "${serial}" "interrupt_stages: both stages CANCELLED" "${S}interrupt_stages n=1 launch=OK started=2 cmds=2 c0=[0-9]+/CANCELLED/[A-Z_]+ c1=[0-9]+/CANCELLED/[A-Z_]+ "
check "${serial}" "deadline: the session deadline stops an endless producer the same way" "${P}deadline n=1 status=[0-9]+ error=INTERRUPTED outcome=CANCELLED stop=1 "
exact "no line after a loss or an interrupt ran" "^osh_op_out: SECRET-after-" 0
check "${serial}" "direct: the direct-argv route runs the same stages" "${S}direct n=1 launch=OK started=2 cmds=2 c0=0/COMPLETED/OK c1=0/COMPLETED/OK stdout_bytes=16 sink_count=4096 sink_hash_ok=1 "
check "${serial}" "malicious: a builtin on a direct record is malformed: INVALID_ARG, nothing started" "${S}malicious n=1 launch=INVALID_ARG started=0 "
check "${serial}" "malicious: an argv outside OUT: INVALID_ARG, nothing started" "${S}malicious n=2 launch=INVALID_ARG started=0 "
check "${serial}" "malicious: nothing ran" "${N}malicious tasks_empty=1 pipes_empty=1 stage_steps=0 "
check "${serial}" "direct_no_cap: a direct record without a spawn capability is DENIED like the shell's" "${P}direct_no_cap n=1 status=126 error=DENIED "
exact "every native case ended with no task, no pipe, every EL0 slot torn down" "${N}[a-z_0-9]+ tasks_empty=1 pipes_empty=1 stage_steps=[0-9]+ not_torn_down=0 " 17
exact "no native case left a task, a pipe or an EL0 slot" "${N}.*(tasks_empty=0|pipes_empty=0|not_torn_down=[1-9])" 0
# totals
G="^osh_op_receipt: seq=[0-9]+ op=console_write result=OK request_digest=[0-9a-f]{64} effect=console_write principal=[0-9a-f]{64} cap_domain=1 cap_index=[0-9]+ cap_generation=[0-9]+ resource_class=1 operation=1"
exact "grant used: one security receipt per console write, 8 in all" "${G} case=[a-z_0-9]+; ${label}\$" 8
exact "grant used: allow wrote 2 lines, 2 receipts" "${G} case=allow; " 2
exact "grant used: even the lost-after-effect case has its receipt" "${G} case=lost; " 1
exact "grant used: no receipt for a refused case" "^osh_op_receipt: .* result=OK .* case=(deny_no_cap|deny_read_only|revoked|unsupported); " 0
exact "the whole boot wrote exactly eight console lines through the operation" "^osh_op_out: " 8
exact "no case leaked a task slot" "^osh_op_end: .* slots_held=[1-9]" 0
exact "every shell case released its capability (8 klog, 14 native)" "^osh_op_end: .* cap_released=1; " 22
exact "every shell case left the session table empty" "^osh_op_end: .* cap_table_empty=1 " 22
# ---- aienos-mv#431: the effect journal across three boots of the same disk ----
exactf() { # serial, description, regex, expected count
    local n; n="$(count "$1" "$3")"
    if [[ "${n}" == "$4" ]]; then pass "$2 (${n})"; else fail "$2: expected $4, got ${n}"; fi
}
I="^osh_op_inspect: case="
J="^osh_op_journal: seq="
D="^osh_op_durable_end: case="
R="^osh_op_recovery: case="
# boot 1: the #429/#430 cases ran on the labelled RAM journal only, never counted as durable
exact "boot 1: every #429/#430 pipeline that recorded an effect named the RAM journal (durable=0)" "${J}[0-9]+ .* durable=0 label=ram_volatile_test case=" 26
exact "boot 1: no #429/#430 pipeline claimed a durable journal" "${J}[0-9]+ .* durable=1 " 0
check "${serial1}" "boot 1: the Store journal was empty and created (header only, 1 chunk)" "${I}journal_boot when=boot rc=0 label=store_cortex durable=1 readonly=0 records=0 osh=0 .* chunks=1 osh_units_launched=0; "
check "${serial1}" "boot 1: mode fresh" "^osh_op_journal_boot: mode=fresh created=1 heap=1; "
check "${serial1}" "d_ok: gen | sink on the Store journal: COMPLETED, sink's hash matches" "^osh_op_stages: case=d_ok n=1 launch=OK started=2 cmds=2 c0=0/COMPLETED/OK c1=0/COMPLETED/OK stdout_bytes=16 sink_count=4096 sink_hash_ok=1 "
check "${serial1}" "d_ok: 2 durable intents, 2 outcomes, a receipt, durable=1" "${J}1 epoch=[0-9]+ intents=2 refused=0 outcomes=2 receipt=[1-9][0-9]* durable=1 label=store_cortex case=d_ok; "
check "${serial1}" "d_ok: 2 spawns, 7 records (8 chunks with the header)" "${D}d_ok spawned=2 chunks=8; "
check "${serial1}" "d_write: gen 100 >o: the write-class open and the spawn each have intent and outcome" "${J}1 epoch=[0-9]+ intents=2 refused=0 outcomes=2 receipt=[1-9][0-9]* durable=1 label=store_cortex case=d_write; "
check "${serial1}" "d_write: o holds 100 bytes, 1 spawn" "^osh_op_stages: case=d_write n=1 launch=OK started=1 cmds=1 c0=0/COMPLETED/OK .*loc_o=100 "
check "${serial1}" "d_write: 15 chunks" "${D}d_write spawned=1 chunks=15; "
# crash point: name, unknown_intent, unknown_started, unsealed_finished, spawned, outcomes and receipts recovery writes, chunks after
while read -r c ui us uf sp ow rw ch; do
    check "${serial1}" "${c}: a read-only look after the loss (no OSH session): ${ui} intent-only, ${us} started, ${uf} finished without receipt, chain intact" "${I}${c} when=after_loss rc=0 label=store_cortex durable=1 readonly=1 .* bad=0 .* dup_outcomes=0 .* unknown_intent=${ui} unknown_started=${us} .* unsealed_finished=${uf} chain_ok=1 "
    check "${serial1}" "${c}: the read-only handle refused a write and the journal did not change" "^osh_op_readonly: case=${c} write_refused=1; "
    check "${serial1}" "${c}: recovery wrote ${ow} OUTCOME_UNKNOWN and ${rw} receipt, failed 0; a second scan wrote nothing; nothing re-spawned" "${R}${c} unknown_intent=${ui} unknown_started=${us} outcomes_written=${ow} receipts_written=${rw} failed=0 second_written=0 spawned_since_case=${sp} "
    check "${serial1}" "${c}: restart: ${ow} open, second scan 0, no capability held" "^osh_op_restart: case=${c} open_intents=${ow} second_scan=0 .* held_cap=0; "
    check "${serial1}" "${c}: after recovery nothing is open and every pipeline is sealed" "${I}${c} when=after_recovery rc=0 .* unknown_intent=0 unknown_started=0 .* unsealed_finished=0 chain_ok=1 "
    check "${serial1}" "${c}: ${sp} spawn(s) in all, ${ch} chunks" "${D}${c} spawned=${sp} chunks=${ch}; "
    exactf "${serial1}" "${c}: one OUTCOME_UNKNOWN line per open intent, replayed=0" "^osh_op_record: seq=1 outcome=OUTCOME_UNKNOWN request_digest=[0-9a-f]{64} replayed=0 .* durable=1 case=${c}; " "${ow}"
done <<'EOT'
d_before_intent 0 0 0 0 0 0 15
d_after_intent 1 0 0 0 1 1 18
d_after_spawn 1 0 0 1 1 1 21
d_after_started 0 1 0 1 1 1 26
d_after_finish 0 1 0 1 1 1 30
d_during_cancel 0 1 0 1 1 1 34
d_during_receipt 0 0 1 1 0 1 38
EOT
check "${serial1}" "d_after_intent: recovery names it UNKNOWN_INTENT_ONLY (the spawn never ran)" "^osh_op_record: .* category=UNKNOWN_INTENT_ONLY effect=task_spawn cmd=0 name=gen .* case=d_after_intent; "
check "${serial1}" "d_after_started: recovery names it UNKNOWN_STARTED" "^osh_op_record: .* category=UNKNOWN_STARTED effect=task_spawn cmd=0 name=gen .* case=d_after_started; "
check "${serial1}" "d_during_receipt: the receipt append was lost whole (sealed Store transaction)" "${J}1 .* receipt=0 durable=1 label=store_cortex lost_in_receipt=1 case=d_during_receipt; "
check "${serial1}" "d_during_cancel: an interrupted pipeline lost during its cancellation" "^osh_op_pipeline: case=d_during_cancel n=1 .* crashed=1 "
check "${serial1}" "reconcile: a non-operator DENIED, the operator's verdict is one record, a second verdict refused" "^osh_op_reconcile: case=d_reconcile intent=[1-9][0-9]* grant=0 not_operator=DENIED operator=OK again=INVALID_ARG records_added=0/1/0; "
check "${serial1}" "reconcile: its security receipt" "^osh_op_receipt: op=reconcile result=OK intent=[1-9][0-9]* verdict=1 record=[1-9][0-9]* request_digest=[0-9a-f]{64} principal=[0-9a-f]{64} cap_domain=1 cap_index=[0-9]+ cap_generation=[0-9]+ resource_class=4 operation=4 replayed=0 case=d_reconcile; "
check "${serial1}" "reconcile: the effect is RECONCILED, nothing else changed" "${I}d_reconcile when=after_reconcile rc=0 .* reconciles=1 .* reconciled=1 .* chain_ok=1 "
check "${serial1}" "boot 1 end: 37 records, nothing open, 8 pipelines all sealed, 38 chunks" "${I}d_end when=end rc=0 label=store_cortex durable=1 readonly=0 records=37 osh=37 other=0 bad=0 intents=10 started=8 outcomes=10 receipts=8 reconciles=1 dup_outcomes=0 finished=5 unknown_intent=0 unknown_started=0 recovered=4 reconciled=1 pipelines=8 sealed=8 unsealed_finished=0 chain_ok=1 chunks=38 "
check "${serial1}" "boot 1: the durable cases finished" "^osh_op_durable_done: mode=fresh cases=10; "
check "${serial1}" "boot 1: the Store keys erased and the disk released after the test" "^store: TEST-ONLY OSH effect journal: Store keys erased, NVMe released$"
# boot 2: a real power cut with two spawns recorded STARTED
check "${serial2}" "boot 2: the journal from boot 1 read back read-only: 37 records, nothing open" "${I}journal_boot when=boot rc=0 label=store_cortex durable=1 readonly=1 records=37 osh=37 .* unknown_intent=0 unknown_started=0 .* chain_ok=1 chunks=38 osh_units_launched=0; "
check "${serial2}" "boot 2: mode powercut" "^osh_op_journal_boot: mode=powercut created=0 heap=1; "
check "${serial2}" "boot 2: halted after both spawns were STARTED (4 records more on the disk)" "^osh_op_powercut: HALT case=powercut n=1 crash_point=4 spawned=2 chunks=42; "
exactf "${serial2}" "boot 2: no other case ran" "^osh_op_begin: case=" 1
exactf "${serial2}" "boot 2: no report, no release: the machine stopped there" "^(report_kind: final|store: TEST-ONLY OSH effect journal: Store keys erased)" 0
# boot 3: recovery after the power cut, a fresh decision, the journal full, refusal by name
check "${serial3}" "boot 3: a read-only look before any shell unit ran: 2 spawns left STARTED" "${I}journal_boot when=boot rc=0 label=store_cortex durable=1 readonly=1 records=41 osh=41 .* unknown_intent=0 unknown_started=2 .* chain_ok=1 chunks=42 osh_units_launched=0; "
check "${serial3}" "boot 3: mode recover" "^osh_op_journal_boot: mode=recover created=0 heap=1; "
exactf "${serial3}" "boot 3: both lost spawns reported OUTCOME_UNKNOWN (UNKNOWN_STARTED), replayed=0" "^osh_op_record: seq=1 outcome=OUTCOME_UNKNOWN request_digest=[0-9a-f]{64} replayed=0 category=UNKNOWN_STARTED effect=task_spawn cmd=[01] name=(gen|sink) .* durable=1 case=powercut; " 2
check "${serial3}" "boot 3: recovery wrote 2 outcomes and 1 receipt, a second scan nothing, nothing spawned, no capability" "^osh_op_restart: case=powercut open_intents=2 second_scan=0 unknown_intent=0 unknown_started=2 outcomes_written=2 receipts_written=1 failed=0 second_written=0 spawned=0 held_cap=0 "
check "${serial3}" "boot 3: after recovery nothing is open, 9 pipelines all sealed" "${I}powercut when=after_recovery rc=0 .* intents=12 started=10 outcomes=12 receipts=9 .* unknown_intent=0 unknown_started=0 .* pipelines=9 sealed=9 unsealed_finished=0 chain_ok=1 chunks=45 "
check "${serial3}" "boot 3: a new request after the power cut is a new decision: it runs once, durably" "^osh_op_stages: case=pc_fresh n=1 launch=OK started=2 cmds=2 c0=0/COMPLETED/OK c1=0/COMPLETED/OK stdout_bytes=16 sink_count=4096 sink_hash_ok=1 "
check "${serial3}" "boot 3: pc_fresh: 2 spawns, 52 chunks" "${D}pc_fresh spawned=2 chunks=52; "
check "${serial3}" "boot 3: the journal filled to its 64-chunk limit with 3 one-stage pipelines" "^osh_op_fill: pipelines=3 chunks=64 max=64; "
check "${serial3}" "boot 3: the next spawn is refused BY NAME: its intent could not be made durable (FAILED_NO_EFFECT, IO)" "^osh_op_refused: seq=1 cmd=0 name=gen effect=task_spawn error=IO outcome=FAILED_NO_EFFECT reason=intent_not_durable durable=1 request_digest=[0-9a-f]{64} case=full; "
check "${serial3}" "boot 3: after that failed append the journal is poisoned: both commands of the next pipeline refused by name" "^osh_op_refused: seq=2 cmd=1 name=sink effect=task_spawn error=IO outcome=FAILED_NO_EFFECT reason=journal_failed durable=1 "
check "${serial3}" "boot 3: refused pipelines: status 1, FAILED_NO_EFFECT" "^osh_op_pipeline: case=full n=2 status=1 error=IO outcome=FAILED_NO_EFFECT stop=0 crashed=0 "
check "${serial3}" "boot 3: no durable intent and nothing spawned by the refused pipelines" "${D}full spawned=0 chunks=64; "
exactf "${serial3}" "boot 3: refused pipelines recorded no intent" "${J}[12] .* intents=0 refused=[12] outcomes=0 receipt=0 durable=1 label=store_cortex case=full; " 2
check "${serial3}" "boot 3 end: 63 records, 17 intents and 17 outcomes, nothing open, 13 pipelines all sealed, 64 chunks" "${I}d_end when=end rc=0 label=store_cortex durable=1 readonly=0 records=63 osh=63 other=0 bad=0 intents=17 started=15 outcomes=17 receipts=13 reconciles=1 dup_outcomes=0 finished=10 unknown_intent=0 unknown_started=0 recovered=6 reconciled=1 pipelines=13 sealed=13 unsealed_finished=0 chain_ok=1 chunks=64 "
check "${serial3}" "boot 3: the Store keys erased and the disk released after the test" "^store: TEST-ONLY OSH effect journal: Store keys erased, NVMe released$"
# no duplicate effect across the three boots: every spawn of the durable cases is one durable spawn intent
d_spawns=0
for f in "${serial1}" "${serial2}" "${serial3}"; do
    n="$(grep -E "^osh_op_receipt: seq=[0-9]+ cmd=[0-9]+ op=task_spawn result=OK .* case=(d_[a-z_]+|powercut|pc_fresh|fill); " "${f}" | wc -l)"
    d_spawns=$(( d_spawns + n ))
done
if [[ "${d_spawns}" == 15 ]]; then pass "no duplicate effect: 15 granted spawns in the durable cases over three boots: 16 durable spawn intents, one lost before its spawn (d_after_intent)"
else fail "no duplicate effect: expected 15 granted spawns in the durable cases, got ${d_spawns}"; fi
if has "${serial3}" "^osh_op_journal_boot: mode=powercut"; then fail "control (#431): boot 3 cannot be mistaken for the power-cut boot"; else pass "control (#431): a wrong expected mode for boot 3 is not found (red)"; fi
# control: a wrong expectation must turn the check red, then the right one green
if has "${serial}" "^osh_op_out: hello from osh DOES-NOT-APPEAR\$"; then fail "control: the gate cannot go red"; else pass "control: a deliberately wrong expected console line is not found (red), the real one is (above)"; fi

if [[ -n "${AIENOS_LOG_DIR:-}" ]]; then
    cp "${serial1}" "${AIENOS_LOG_DIR}/qemu_ck_osh_op_serial.log"
    cp "${serial2}" "${AIENOS_LOG_DIR}/qemu_ck_osh_op_serial_powercut.log"
    cp "${serial3}" "${AIENOS_LOG_DIR}/qemu_ck_osh_op_serial_recover.log"
fi
if [[ "${failed}" != 0 || -n "${AIENOS_QEMU_VERBOSE:-}" ]]; then
    echo "---- serial (osh_op lines) ----"; grep -E "^(osc_|osh_|artifact|kernel|report_kind|panic|fault)" "${serial}" | head -120 || true
fi
echo "label: QEMU (aarch64 virt), TEST signer, not physical"
if [[ "${failed}" == 0 ]]; then echo "${verdict}: PASS"; else echo "${verdict}: FAIL"; exit 1; fi
