#!/usr/bin/env bash
# aienos_run_try_test.sh -- end-to-end test of the one-command try path on this Linux
# host, with local files instead of aienos.com (docs/AIENOS_RUN_LINUX.md):
#
#   row T1  try.sh downloads aienos-run from a file:// manifest, verifies it, installs it,
#           and aienos-run downloads + verifies the kernel image and boots it to the
#           console (receipt line console_ready=...) with the chosen backend
#   row T2  a tampered aienos-run download is refused (exit 12) and nothing is installed
#   row T3  a tampered image is refused by aienos-run (exit 4) and never booted
#   row T4  --where prints the paths; --reset --yes recreates an empty Store disk
#
# Needs: target/debug/aienos-run (or AIENOS_RUN_BIN) and target/native-kernel/BOOTAA64.EFI
# (or AIENOS_KERNEL_EFI). Backend: AIENOS_TRY_BACKEND (default kvm).
# Final line: "AIENOS_RUN_TRY: PASS|FAIL". Exit 0 PASS, 1 FAIL, 2 missing inputs.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="${AIENOS_RUN_BIN:-${repo_root}/target/debug/aienos-run}"
efi="${AIENOS_KERNEL_EFI:-${repo_root}/target/native-kernel/BOOTAA64.EFI}"
backend="${AIENOS_TRY_BACKEND:-kvm}"
[[ -x "$bin" && -f "$efi" ]] || { echo "missing $bin or $efi"; echo "AIENOS_RUN_TRY: FAIL (inputs missing)"; exit 2; }

case "$(uname -m)" in aarch64 | arm64) platform=linux-aarch64 ;; *) platform=linux-x86_64 ;; esac
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/site" "$work/home"
cp "$bin" "$work/site/aienos-run"
cp "$efi" "$work/site/BOOTAA64.EFI"
"${repo_root}/scripts/aienos_run_manifest.sh" "$work/site/manifest.txt" test \
    "image:aarch64:uefi-app:$work/site/BOOTAA64.EFI" "runner:${platform}:$work/site/aienos-run"
failed=0
row() { echo "TRY_ROW $1: $2${3:+ ($3)}"; [[ "$2" == PASS ]] || failed=1; }

export AIENOS_RUN_HOME="$work/home"
export AIENOS_TRY_BIN_DIR="$work/bin"
export AIENOS_TRY_BASE="file://$work/site"

# T1
AIENOS_TRY_NO_RUN=0 sh "${repo_root}/scripts/try.sh" --backend "$backend" --headless --receipt \
    --exit-on-ready --timeout 120 >"$work/t1.out" 2>"$work/t1.err"
t1=$?
ready="$(grep -o 'receipt console_ready=[0-9.]* ms' "$work/t1.err" | head -1)"
if [[ $t1 == 0 && -x "$work/bin/aienos-run" && -n "$ready" ]] && grep -q "report_kind: final" "$work/t1.out"; then
    row T1 PASS "exit 0, ${ready}, backend ${backend}"
else
    row T1 FAIL "exit $t1"
    tail -20 "$work/t1.err"
fi

# T2
rm -rf "$work/bin"
cp "$work/site/aienos-run" "$work/good-runner"
printf 'x' >>"$work/site/aienos-run"
AIENOS_TRY_NO_RUN=1 sh "${repo_root}/scripts/try.sh" >/dev/null 2>"$work/t2.err"
t2=$?
if [[ $t2 == 12 && ! -e "$work/bin/aienos-run" ]] && grep -q "mismatch" "$work/t2.err"; then
    row T2 PASS "exit 12, not installed"
else
    row T2 FAIL "exit $t2"
fi
cp "$work/good-runner" "$work/site/aienos-run"

# T3: a different image under the same manifest line.
rm -rf "$work/home/cache"
printf 'x' >>"$work/site/BOOTAA64.EFI"
"$bin" --manifest "file://$work/site/manifest.txt" --backend "$backend" --headless --timeout 60 \
    >"$work/t3.out" 2>"$work/t3.err"
t3=$?
if [[ $t3 == 4 ]] && grep -q "mismatch" "$work/t3.err" && [[ ! -s "$work/t3.out" ]]; then
    row T3 PASS "exit 4, never booted"
else
    row T3 FAIL "exit $t3"
fi

# T4
"$bin" --where >"$work/t4.out" 2>&1
w=$?
"$bin" --reset --yes >/dev/null 2>&1
r=$?
size="$(stat -c %s "$work/home/data/store.img" 2>/dev/null || echo 0)"
if [[ $w == 0 && $r == 0 && "$size" == 1073741824 ]] && grep -q "store disk:.*$work/home/data/store.img" "$work/t4.out" \
    && grep -q "forward:.*udp 127.0.0.1:47040" "$work/t4.out"; then
    row T4 PASS "where ok, reset gives a 1 GiB empty disk"
else
    row T4 FAIL "where=$w reset=$r size=$size"
fi

[[ $failed == 0 ]] && { echo "AIENOS_RUN_TRY: PASS"; exit 0; }
echo "AIENOS_RUN_TRY: FAIL"
exit 1
