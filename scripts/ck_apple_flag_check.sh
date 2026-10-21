#!/usr/bin/env bash
# ck_apple_flag_check.sh -- proves the Apple platform layer (native/kernel/apple,
# CK_APPLE=1) leaves the existing UEFI/ACPI AArch64 images byte for byte as
# they are (L250 brief: "the existing UEFI/ACPI aarch64 path (Spark, Pi) must
# be byte-for-byte unaffected").
#
# For each image kind (default core image; CK_PI5=1 Raspberry Pi 5 image) it
# builds three copies with the same AIENOS_COMMIT string:
#   base   the merge base with origin/main, in a temporary git worktree;
#   off    this tree, CK_APPLE unset;
#   on     this tree, CK_APPLE=1 (which also builds the Apple Image);
# then cmp's the three BOOTAA64.EFI and checks the linked ELF of "off" and
# "on" carries no Apple symbol (ck_fdt_*, ck_apple_*, ck_aic_*, apple_*).
# Last line: CK_APPLE_FLAG_CHECK: PASS|FAIL.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
base="$(git merge-base HEAD origin/main)"
tmp="$(mktemp -d /tmp/ck-apple-flag.XXXXXX)"
wt="${tmp}/base-tree"
git worktree add -q --detach "${wt}" "${base}"
cleanup() { git worktree remove --force "${wt}" >/dev/null 2>&1 || true; rm -rf "${tmp}"; }
trap cleanup EXIT
commit="ck-apple-flag-check"
fail=0

for kind in default pi5; do
    extra=()
    [ "${kind}" = pi5 ] && extra=(CK_PI5=1)
    make -s -C "${wt}/native/kernel" OUT="${tmp}/${kind}-base" AIENOS_COMMIT="${commit}" "${extra[@]}" >/dev/null
    make -s -C native/kernel OUT="${tmp}/${kind}-off" AIENOS_COMMIT="${commit}" "${extra[@]}" >/dev/null
    make -s -C native/kernel OUT="${tmp}/${kind}-on" AIENOS_COMMIT="${commit}" CK_APPLE=1 "${extra[@]}" >/dev/null
    b="${tmp}/${kind}-base/BOOTAA64.EFI"
    for v in off on; do
        f="${tmp}/${kind}-${v}/BOOTAA64.EFI"
        if cmp -s "${b}" "${f}"; then
            echo "  ok   ${kind} CK_APPLE=${v}: BOOTAA64.EFI identical to ${base:0:8} ($(stat -c %s "${f}") bytes, sha256 $(sha256sum "${f}" | cut -c1-16))"
        else
            echo "  FAIL ${kind} CK_APPLE=${v}: BOOTAA64.EFI differs from ${base:0:8}"; fail=1
        fi
        elf="$(ls "${tmp}/${kind}-${v}"/*.elf 2>/dev/null | head -1 || true)"
        if [ -n "${elf}" ]; then
            n="$(nm "${elf}" | grep -cE ' (ck_fdt_|ck_apple_|ck_aic_|apple_)' || true)"
            if [ "${n}" = 0 ]; then echo "  ok   ${kind} CK_APPLE=${v}: no Apple symbol in $(basename "${elf}")";
            else echo "  FAIL ${kind} CK_APPLE=${v}: ${n} Apple symbols in $(basename "${elf}")"; fail=1; fi
        else
            echo "  FAIL ${kind} CK_APPLE=${v}: no linked ELF found to inspect"; fail=1
        fi
    done
done
if [ -f "${tmp}/default-on/apple/Image" ]; then
    echo "  ok   CK_APPLE=1 also built the Apple Image ($(stat -c %s "${tmp}/default-on/apple/Image") bytes)"
else
    echo "  FAIL CK_APPLE=1 did not build the Apple Image"; fail=1
fi
if [ "${fail}" = 0 ]; then echo "CK_APPLE_FLAG_CHECK: PASS"; else echo "CK_APPLE_FLAG_CHECK: FAIL"; exit 1; fi
