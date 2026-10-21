#!/usr/bin/env bash
# QEMU gate for the CK_APPLE=1 raw arm64 Image (native/kernel/apple, L250,
# docs/APPLE_SILICON_PLATFORM.md). QEMU's own loader boots the Image with
# -kernel the way m1n1 does on a Mac: x0 = device tree, MMU off, EL2 (or EL1).
# QEMU is not a Mac: a PASS here qualifies nothing on Apple hardware.
#
# Rows (each its own QEMU run, -M virt, -cpu cortex-a76, 4 CPUs, 1 GiB):
#   el2      virtualization=on: EL2 -> EL1 with HCR_EL2.E2H 0, PL011 console
#            from /chosen stdout-path, vectors, timer, "not an Apple
#            platform" refusal line, PSCI SYSTEM_OFF via smc.
#   el2-e2h  TEST-ONLY image (CK_APPLE_TEST_E2H=1) that sets HCR_EL2.E2H as
#            Apple CPUs force it: the SCTLR_EL12 / E2H-layout path.
#   el1      virtualization=off: entered at EL1, PSCI via hvc.
#   t8142    -dtb QEMU's tree with root compatible apple,t8142 (an Apple SoC
#            not in the family table): "REFUSED unknown Apple SoC".
#   noaic    -dtb QEMU's tree claiming apple,t8112 without an AIC node:
#            "REFUSED apple,t8112 row wants interrupt controller".
#   mutant   TEST-ONLY E2H image with the E2H-0 CNTHCTL_EL2 bits: must FAIL
#            (red before green; proves the gate sees the timer-access layout).
# Every row except mutant also needs "AIENOS_APPLE_IMAGE: PASS" and QEMU exit 0.
# Last line: AIENOS_CK_APPLE_BOOT: PASS|FAIL|NOT_RUN.
# Lock and quiet flag: scripts/lib_gate_hold.sh, like every QEMU gate here.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 not installed"; echo "AIENOS_CK_APPLE_BOOT: NOT_RUN"; exit 2; }

cross=""
if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
out="${repo_root}/target/native-kernel"
make -s -C native/kernel CROSS="${cross}" OUT="${out}" CK_APPLE=1 apple-image
make -s -C native/kernel CROSS="${cross}" OUT="${out}" CK_APPLE=1 CK_APPLE_TEST_E2H=1 apple-image
make -s -C native/kernel CROSS="${cross}" OUT="${out}" CK_APPLE=1 CK_APPLE_TEST_E2H=1 CK_APPLE_TEST_MUTANT=cnthctl apple-image
fix="${repo_root}/native/kernel/tests/fixtures/apple"

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_apple_boot_test "${AIENOS_GATE_MINUTES:-20}"; then
    echo "NOT_RUN  ${gh_why}"
    echo "AIENOS_CK_APPLE_BOOT: NOT_RUN"
    exit 3
fi
work="$(mktemp -d)"
cleanup() { gh_lock_release; rm -f "${work}"/*.log "${work}"/*.txt; rmdir "${work}" 2>/dev/null || true; }
trap cleanup EXIT

fail=0
# run_row <name> <image> <virtualization on|off> <extra qemu args...>
run_row() {
    local name="$1" image="$2" virt="$3"
    shift 3
    local log="${work}/${name}.log" rc=0
    timeout "${AIENOS_QEMU_TIMEOUT:-60}" qemu-system-aarch64 -M "virt,virtualization=${virt},gic-version=3" \
        -cpu cortex-a76 -smp 4 -m 1024 -nographic -monitor none -kernel "${image}" "$@" \
        -serial file:"${log}" -no-reboot >/dev/null 2>&1 || rc=$?
    tr -d '\r' <"${log}" >"${work}/${name}.txt"
    [[ -z "${AIENOS_LOG_DIR:-}" ]] || cp "${work}/${name}.txt" "${AIENOS_LOG_DIR}/qemu_ck_apple_${name}.log"
    echo "---- ${name} (qemu exit ${rc}) ----"
    sed -n '/AIENOS_APPLE_IMAGE: entry/,$p' "${work}/${name}.txt"
    eval "${name//-/_}_rc=${rc}"
}
# want <row> <fixed string>: the row's serial log has the line.
want() {
    if grep -qF -- "$2" "${work}/$1.txt"; then echo "  ok   $1: $2"; else echo "  FAIL $1: missing \"$2\""; fail=1; fi
}
# common <row> <rc>: verdict line and a clean QEMU exit.
common() {
    want "$1" "AIENOS_APPLE_IMAGE: PASS"
    want "$1" "apple: fdt ok version=17"
    want "$1" "apple: console pl011 at 0x9000000 (stdout-path -> /pl011@9000000)"
    want "$1" "apple: vectors ok (brk at EL1 taken=1, returned)"
    want "$1" "apple: timer ok"
    want "$1" "apple: family table rows=14"
    want "$1" "apple: idle policy in force=spin"
    if grep -qF "FAULT" "${work}/$1.txt"; then echo "  FAIL $1: fault line"; fail=1; fi
    if [ "$2" = 0 ]; then echo "  ok   $1: qemu exit 0"; else echo "  FAIL $1: qemu exit $2"; fail=1; fi
}

img="${out}/apple/Image"
img_e2h="${out}/apple-test-e2h/Image"
run_row el2 "${img}" on
run_row el2-e2h "${img_e2h}" on
run_row el1 "${img}" off
run_row t8142 "${img}" on -dtb "${fix}/qemu-virt-t8142.dtb"
run_row noaic "${img}" on -dtb "${fix}/qemu-virt-t8112-noaic.dtb"
AIENOS_QEMU_TIMEOUT=15 run_row mutant "${out}/apple-test-e2h-mutant-cnthctl/Image" on

echo "---- checks ----"
common el2 "${el2_rc}"
want el2 "AIENOS_APPLE_IMAGE: entry el=2 e2h=0"
want el2 "apple: el2->el1 done (CurrentEL=1, HCR_EL2.E2H=0)"
want el2 'apple: REFUSED not an Apple platform (root compatible "linux,dummy-virt"); Apple drivers not started'
want el2 "apple: psci SYSTEM_OFF via smc"
common el2-e2h "${el2_e2h_rc}"
want el2-e2h "AIENOS_APPLE_IMAGE: entry el=2 e2h=1"
want el2-e2h "apple: el2->el1 done (CurrentEL=1, HCR_EL2.E2H=1, SCTLR_EL12 path)"
common el1 "${el1_rc}"
want el1 "AIENOS_APPLE_IMAGE: entry el=1 e2h=0"
want el1 "apple: entered at EL1 (CurrentEL=1)"
want el1 "apple: psci SYSTEM_OFF via hvc"
common t8142 "${t8142_rc}"
want t8142 'apple: REFUSED unknown Apple SoC (root compatible "apple,j999","apple,t8142","apple,arm-platform"; no family table row); Apple drivers not started'
common noaic "${noaic_rc}"
want noaic 'apple: REFUSED apple,t8112 row wants interrupt controller "apple,aic2"; tree has none; Apple drivers not started'

# Red before green: the CNTHCTL_EL2 layout mutant (E2H 0 bits under E2H 1)
# must NOT pass: EL1 timer access traps to an EL2 with no vectors.
if grep -qF "AIENOS_APPLE_IMAGE: PASS" "${work}/mutant.txt"; then
    echo "  FAIL mutant: CNTHCTL layout mutant passed (gate does not catch it)"; fail=1
else
    echo "  ok   mutant: CNTHCTL layout mutant fails as required (qemu exit ${mutant_rc})"
fi
if [ "${fail}" = 0 ]; then echo "AIENOS_CK_APPLE_BOOT: PASS"; else echo "AIENOS_CK_APPLE_BOOT: FAIL"; exit 1; fi
