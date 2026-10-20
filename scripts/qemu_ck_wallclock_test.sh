#!/usr/bin/env bash
# qemu_ck_wallclock_test.sh -- gate AIENOS_WALLCLOCK_QEMU (L200): the C kernel names the date.
# The UEFI stub reads the firmware clock through GetTime before ExitBootServices
# (native/boot/efi_time.c), the kernel advances it with the generic counter
# (native/kernel/core/wallclock.c) and prints "wallclock:" lines. QEMU is told the guest
# clock with -rtc base=...; the kernel must report that date. Two boots with two dates
# (the second one is a leap day), each checked for: the boot_utc line
# within 120 s after the QEMU base, the kernel's "now" line not before it, the shape of the
# lines against native/kernel/tests/fixtures/wallclock_qemu_expected.txt (timestamps
# normalized). QEMU qualifies nothing physical: the Spark's own firmware clock (its rtc0 is
# the Linux rtc-efi driver, the same GetTime service) is NOT_RUN here.
# Final line: AIENOS_WALLCLOCK_QEMU: PASS|FAIL|NOT_RUN [(...)]   exit 0 / 1 / 2 (tools) / 3 (NOT_RUN)
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_WALLCLOCK_QEMU"
code_fd="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
vars_fd="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
cross=""
if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
for t in qemu-system-aarch64 "${cross}gcc" date; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no AAVMF)"; exit 2; }
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
out="${repo_root}/target/native-kernel"
make -s -C native/kernel CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" >/dev/null
. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_wallclock_test "${AIENOS_GATE_MINUTES:-30}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
work="$(mktemp -d)"
cleanup() { rm -rf "${work}"; gh_lock_release; }
trap cleanup EXIT
fails=0
chk() { if [[ "$2" == ok ]]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }

run() { # name, rtc base
    local name="$1" base="$2" log="${work}/$1.log"
    mkdir -p "${work}/$1/esp/EFI/BOOT" "${work}/$1/esp/EFI/AIENOS"
    touch "${work}/$1/esp/EFI/AIENOS/BOOTREPORT.TXT"
    cp "${out}/BOOTAA64.EFI" "${work}/$1/esp/EFI/BOOT/BOOTAA64.EFI"
    cp "${vars_fd}" "${work}/$1/vars.fd"
    local base_s; base_s="$(date -u -d "${base}" +%s)"
    set +e
    timeout "${AIENOS_QEMU_TIMEOUT:-180}" qemu-system-aarch64 \
        -M virt,virtualization=on,gic-version=3 -accel tcg,thread=single -cpu max -smp 4 -m 2048 \
        -rtc base="${base}" \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/$1/vars.fd" \
        -drive if=none,id=esp,format=raw,file=fat:rw:"${work}/$1/esp" \
        -device virtio-blk-pci,drive=esp -device ramfb -display none -nic none \
        -serial file:"${log}" -no-reboot
    local qs=$?
    set -e
    tr -d '\r' <"${log}" >"${work}/$1.txt"
    echo "== run ${name} (-rtc base=${base}) =="
    chk "${name}: QEMU exited 0 (PSCI reset)" "$([[ ${qs} == 0 ]] && echo ok)"
    chk "${name}: kernel alive" "$(grep -aqx 'kernel: alive' "${work}/$1.txt" && echo ok)"
    grep -a '^wallclock: ' "${work}/$1.txt" | sed -E 's/[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z/<utc>/g; s/(unix|since_boot_read_s|since_boot_read_ms)=[0-9]+/\1=<n>/g' >"${work}/$1.norm" || true
    if diff -u native/kernel/tests/fixtures/wallclock_qemu_expected.txt "${work}/$1.norm" >"${work}/$1.diff"; then
        chk "${name}: wallclock lines equal the fixture shape" ok
    else
        chk "${name}: wallclock lines equal the fixture shape" bad; head -20 "${work}/$1.diff"
    fi
    local boot_utc now_utc unix since
    boot_utc="$(sed -nE 's/^wallclock: source=efi-gettime boot_utc=([0-9T:-]+Z) .*/\1/p' "${work}/$1.txt")"
    now_utc="$(sed -nE 's/^wallclock: now_utc=([0-9T:-]+Z) unix=([0-9]+) since_boot_read_s=([0-9]+) since_boot_read_ms=([0-9]+)$/\1/p' "${work}/$1.txt")"
    unix="$(sed -nE 's/^wallclock: now_utc=([0-9T:-]+Z) unix=([0-9]+) since_boot_read_s=([0-9]+) since_boot_read_ms=([0-9]+)$/\2/p' "${work}/$1.txt")"
    since="$(sed -nE 's/^wallclock: now_utc=([0-9T:-]+Z) unix=([0-9]+) since_boot_read_s=([0-9]+) since_boot_read_ms=([0-9]+)$/\3/p' "${work}/$1.txt")"
    local b_s=0
    [[ -n "${boot_utc}" ]] && b_s="$(date -u -d "${boot_utc}" +%s)"
    chk "${name}: boot_utc is 0..120 s after the QEMU base ${base} (got ${boot_utc:-none})" "$([[ -n "${boot_utc}" && ${b_s} -ge ${base_s} && ${b_s} -le $((base_s + 120)) ]] && echo ok)"
    chk "${name}: kernel unix equals boot read plus elapsed (unix=${unix:-none} since=${since:-none})" "$([[ -n "${unix}" && ${unix} -eq $((b_s + since)) && ${since} -ge 0 && ${since} -le 120 ]] && echo ok)"
    chk "${name}: now_utc names the same second as unix" "$([[ -n "${now_utc}" && "$(date -u -d "@${unix:-0}" +%Y-%m-%dT%H:%M:%SZ)" == "${now_utc}" ]] && echo ok)"
    local n_ms f_unix f_ms
    n_ms="$(sed -nE 's/^wallclock: now_utc=[0-9T:-]+Z unix=[0-9]+ since_boot_read_s=[0-9]+ since_boot_read_ms=([0-9]+)$/\1/p' "${work}/$1.txt")"
    f_unix="$(sed -nE 's/^wallclock: final_utc=[0-9T:-]+Z unix=([0-9]+) since_boot_read_s=[0-9]+ since_boot_read_ms=[0-9]+$/\1/p' "${work}/$1.txt")"
    f_ms="$(sed -nE 's/^wallclock: final_utc=[0-9T:-]+Z unix=[0-9]+ since_boot_read_s=[0-9]+ since_boot_read_ms=([0-9]+)$/\1/p' "${work}/$1.txt")"
    chk "${name}: counter moved the clock between the two kernel lines (now ${n_ms:-none} ms, final ${f_ms:-none} ms)" "$([[ -n "${n_ms}" && -n "${f_ms}" && ${f_ms} -gt ${n_ms} && ${f_ms} -le 120000 ]] && echo ok)"
    chk "${name}: final unix equals boot read plus the whole seconds of the counter ms" "$([[ -n "${f_unix}" && -n "${f_ms}" && ${f_unix} -eq $((b_s + f_ms / 1000)) ]] && echo ok)"
    echo "---- ${name} wallclock lines ----"; grep -a '^wallclock: ' "${work}/$1.txt" || true; echo "----"
}
run a "2026-10-20T12:34:56"
run b "2028-02-29T00:00:00"
if [[ ${fails} == 0 ]]; then echo "${tag}: PASS"; exit 0; fi
echo "${tag}: FAIL (${fails} checks)"; exit 1
