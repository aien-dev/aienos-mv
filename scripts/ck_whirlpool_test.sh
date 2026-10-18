#!/bin/sh
# ck_whirlpool_test.sh -- host test of the startup picture (native/kernel/ui/whirlpool.c).
#  1. Refusals: too little memory, bad geometry, unknown pixel formats.
#  2. Frame 0 is the screen as the firmware left it; the picture holds still
#     while the water comes in, orbits the black hole, shrinks, and is gone
#     shortly before CK_WHIRL_MERGE_MS and for good by CK_WHIRL_MS.
#  3. The water keeps turning (also ten minutes in); both pixel orders draw the
#     same picture; a blank or grey screen gives the whirlpool alone; a 4K
#     screen in blocks of 3.
# Pictures of each moment land in target/native-kernel-whirlpool/ for a person
# to look at. Hosted only. Ends with "AIENOS_WHIRLPOOL: PASS" or
# "AIENOS_WHIRLPOOL: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
OUT=target/native-kernel-whirlpool
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_whirlpool.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_WHIRLPOOL: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
mkdir -p "$OUT"
$CC -std=c11 -O2 -Wall -Wextra -Werror -o "$W/test" $K/ui/tests/whirlpool_test.c $K/ui/whirlpool.c \
    || fail "test does not build"
"$W/test" "$OUT" || fail "picture checks"
echo "pictures: $OUT"
echo "AIENOS_WHIRLPOOL: PASS"
