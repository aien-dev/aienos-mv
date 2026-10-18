#!/bin/sh
# ck_allen_ui_test.sh -- host test of Allen's window (native/kernel/ui) and
# its first local answers (native/kernel/svc/allen_reply.c).
#  1. Allen's answers: "Hello Allen" (any case, punctuation, spacing) gives
#     "Hello Humanity."; anything it does not know gives "I don't know that yet."
#  2. Allen's typeface: every printable character rasterizes, measures and
#     has ink; the integer sqrt and sine tables hold.
#  3. The window at 3840x2160 BGRX (the iMac's screen) and 1280x800 RGBX with
#     a padded pitch: short memory and unknown formats refused; drawn, typed
#     into, answered letter by letter; the conversation stays put while the
#     orb moves; red and blue swap for RGBX; nothing lands past the visible width.
# Pictures of each state land in target/native-kernel-allen-ui/ for a person
# to look at. Hosted only. Ends with "AIENOS_ALLEN_UI: PASS" or
# "AIENOS_ALLEN_UI: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
OUT=target/native-kernel-allen-ui
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_allen_ui.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail() { echo "AIENOS_ALLEN_UI: FAIL $*"; exit 1; }
CC=${HOSTCC:-cc}
mkdir -p "$OUT"
$CC -std=c11 -O2 -Wall -Wextra -Werror -o "$W/test" $K/ui/tests/allen_ui_test.c $K/ui/allen_ui.c \
    $K/ui/allen_font.c $K/svc/allen_reply.c $K/svc/allen_notes.c || fail "test does not build"
"$W/test" "$OUT" || fail "window checks"
echo "pictures: $OUT"
echo "AIENOS_ALLEN_UI: PASS"
