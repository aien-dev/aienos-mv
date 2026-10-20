#!/usr/bin/env bash
# qemu_nvme_bench.sh -- QEMU timing of the native/disk NVMe driver: the
# queue-depth-1 bounce driver (the driver at BASE, default 18e2765) against
# the current tree, same payload source, same image file, same QEMU command,
# same host, run back to back (ABAB...).
#
# The payload writes a 32 MiB extent then reads it back three times, timing
# each pass with the guest generic timer and checking a 64-bit hash of the
# data. It prints raw "BENCH ... bytes=N ticks=T freq=F" lines; this script
# collects them into ${out}/bench.tsv. MB/s is bytes * freq / ticks / 1e6.
#
# QEMU is an emulator (TCG, no KVM on this host): these are QEMU numbers for
# the driver's own overhead and queue use, not the Spark SSD. Label them QEMU.
#
# Verdict line: AIENOS_NVME_BENCH_QEMU: DONE | FAIL | NOT_RUN
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
base="${AIENOS_BENCH_BASE:-18e276590424593647cd7c39c6d16c2f9f8d6027}"
rounds="${AIENOS_BENCH_ROUNDS:-2}"
qemu_timeout="${AIENOS_QEMU_TIMEOUT:-600}"
out="${AIENOS_BENCH_OUT:-${repo}/target/qemu-nvme-bench}"
. "${repo}/scripts/lib_gate_hold.sh"

if ! gh_quiet_check; then
    echo "NOT_RUN  ${gh_why}"
    echo "AIENOS_NVME_BENCH_QEMU: NOT_RUN"
    exit 0
fi
for tool in qemu-system-aarch64 timeout make git; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "required tool ${tool} not found"
        echo "AIENOS_NVME_BENCH_QEMU: NOT_RUN"
        exit 0
    fi
done
if ! gh_lock_take qemu_nvme_bench "${AIENOS_GATE_MINUTES:-90}"; then
    echo "NOT_RUN  ${gh_why}"
    echo "AIENOS_NVME_BENCH_QEMU: NOT_RUN"
    exit 0
fi
cleanup() { gh_lock_release; }
trap cleanup EXIT

cross=""
if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
mkdir -p "${out}"
rm -rf "${out}/old" "${out}/new"

# Baseline tree: the driver files at BASE, the current payload source (its
# queue-depth code compiles out against the old header).
mkdir -p "${out}/old/qemu"
for f in disk.c disk.h disk_nvme.c disk_nvme.h; do
    git -C "${repo}" show "${base}:native/disk/${f}" >"${out}/old/${f}"
done
cp "${repo}/native/disk/qemu/payload.c" "${repo}/native/disk/qemu/payload.ld" \
   "${repo}/native/disk/qemu/start.S" "${repo}/native/disk/qemu/Makefile" "${out}/old/qemu/"
mkdir -p "${out}/new/qemu"
cp "${repo}/native/disk/"disk.c "${repo}/native/disk/"disk.h \
   "${repo}/native/disk/"disk_nvme.c "${repo}/native/disk/"disk_nvme.h "${out}/new/"
cp "${repo}/native/disk/qemu/payload.c" "${repo}/native/disk/qemu/payload.ld" \
   "${repo}/native/disk/qemu/start.S" "${repo}/native/disk/qemu/Makefile" "${out}/new/qemu/"
for t in old new; do
    if ! make -s -C "${out}/${t}/qemu" OUT=out CC="${cross}gcc" LD="${cross}ld" NM="${cross}nm" bench \
            >"${out}/${t}-build.log" 2>&1; then
        tail -20 "${out}/${t}-build.log"
        echo "AIENOS_NVME_BENCH_QEMU: FAIL (${t} payload build)"
        exit 1
    fi
done

img="${out}/bench.img"
printf 'run\tdriver\tpath\tdepth\top\tbytes\tticks\tfreq\tverify\n' >"${out}/bench.tsv"
{
    echo "host: $(uname -srm)"
    echo "qemu: $(qemu-system-aarch64 --version | head -1)"
    echo "base: ${base}"
    echo "tree: $(git -C "${repo}" rev-parse HEAD)$(git -C "${repo}" diff --quiet || echo '+dirty')"
    echo "nproc: $(nproc)"
} >"${out}/host.txt"

fail=0
for r in $(seq 1 "${rounds}"); do
    for t in old new; do
        rm -f "${img}"
        truncate -s 128M "${img}"
        log="${out}/run${r}-${t}.log"
        echo "loadavg before run${r}-${t}: $(cut -d' ' -f1-3 /proc/loadavg)" >>"${out}/host.txt"
        set +e
        timeout --kill-after=5 "${qemu_timeout}" qemu-system-aarch64 \
            -M virt,highmem=off -cpu cortex-a57 -m 256M -smp 1 \
            -nographic -monitor none -serial stdio -no-reboot \
            -kernel "${out}/${t}/qemu/out/nvme_bench.elf" \
            -drive "file=${img},if=none,id=nvm1,format=raw" \
            -device nvme,id=nvme0,serial=aienos-bench \
            -device nvme-ns,drive=nvm1,bus=nvme0,nsid=1,logical_block_size=512,physical_block_size=512 \
            </dev/null >"${log}" 2>&1
        qrc=$?
        set -e
        tr -d '\r' <"${log}" | grep '^BENCH ' | sed -n 's/^BENCH driver=\([^ ]*\) path=\([^ ]*\) depth=\([0-9]*\) op=\([a-z]*\) bytes=\([0-9]*\) ticks=\([0-9]*\) freq=\([0-9]*\) verify=\([a-zA-Z]*\)$/\1\t\2\t\3\t\4\t\5\t\6\t\7\t\8/p' \
            | sed "s/^/run${r}-${t}\t/" >>"${out}/bench.tsv"
        if [ "${qrc}" -ne 0 ] || ! tr -d '\r' <"${log}" | grep -qx 'NVME_QEMU_PAYLOAD: PASS'; then
            echo "run${r}-${t}: payload did not pass (qemu rc=${qrc})"
            tr -d '\r' <"${log}" | tail -5
            fail=1
        fi
    done
done
column -t -s "$(printf '\t')" "${out}/bench.tsv"
if [ "${fail}" -ne 0 ] || grep -q MISMATCH "${out}/bench.tsv"; then
    echo "AIENOS_NVME_BENCH_QEMU: FAIL"
    exit 1
fi
echo "(QEMU emulated NVMe, TCG; not the Spark SSD)"
echo "AIENOS_NVME_BENCH_QEMU: DONE (${out}/bench.tsv)"
