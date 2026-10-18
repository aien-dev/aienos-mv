#!/bin/sh
# ck_chirp_test.sh -- host test of the startup sound (native/kernel/ui/chirp.c),
# the gravitational-wave chirp timed to the startup picture.
#  1. Silent while the picture holds still and after the end; both channels
#     the same; the loudest sample at the peak level; no step big enough to
#     click.
#  2. Loudness comes in as the picture moves out, only rises to the plunge,
#     peaks there and fades in tens of milliseconds.
#  3. Pitch is steady in the orbit, only rises in the spiral, reaches the top
#     in the last 20 ms; the pitch heard (zero crossings) matches.
#  4. Frames worked out in any order give the same sound.
# The sound lands in target/native-kernel-chirp/chirp.wav for a person to
# listen to. Hosted only. Ends with "AIENOS_CHIRP: PASS" or
# "AIENOS_CHIRP: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
OUT=target/native-kernel-chirp
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_chirp.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_CHIRP: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
mkdir -p "$OUT"
$CC -std=c11 -O2 -Wall -Wextra -Werror -o "$W/test" $K/ui/tests/chirp_test.c $K/ui/chirp.c || fail "test does not build"
"$W/test" "$OUT" || fail "sound checks"
echo "sound: $OUT/chirp.wav"
echo "AIENOS_CHIRP: PASS"
