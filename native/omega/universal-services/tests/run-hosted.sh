#!/bin/sh
# Host orchestration only: all assertions and application logic are Omega/byte fixtures.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPO=$(CDPATH= cd -- "$HERE/../../.." && pwd)
COMPILER=${OMEGA_COMPILER:-/home/drakestapleton/handoffs/universal-adapter-L707/shared/omc}
CORE=${OMEGA_MATH_CORE:-/home/drakestapleton/handoffs/universal-adapter-L707/L716/source/omega/seed/math/core.om}
CLI=${OMEGA_MATH_CLI:-/home/drakestapleton/handoffs/universal-adapter-L707/L716/source/omega/seed/math/cli.om}
EXPECTED_COMPILER=191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9
EXPECTED_CORE=b70391a7a38464cd63ff796748565baf3dc2f73afaed90cbc2b94774dba02db6
EXPECTED_CLI=145db7e5184efbc5076ce27e93ddc7822e3f509c793f04f2413e0fd29f3343d0
[ "$(sha256sum "$COMPILER" | cut -d" " -f1)" = "$EXPECTED_COMPILER" ]
[ "$(sha256sum "$CORE" | cut -d" " -f1)" = "$EXPECTED_CORE" ]
[ "$(sha256sum "$CLI" | cut -d" " -f1)" = "$EXPECTED_CLI" ]
TMP=$(mktemp -d "${TMPDIR:-/home/drakestapleton/handoffs/universal-adapter-L707/L716}/l716-hosted.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
compile() {
    [ "$(sha256sum "$COMPILER" | cut -d" " -f1)" = "$EXPECTED_COMPILER" ]
    "$COMPILER" < "$1" > "$2"
    chmod 700 "$2"
}
cat "$HERE/services.om" "$HERE/tests/facade-test.om" > "$TMP/facade-test.om"
compile "$TMP/facade-test.om" "$TMP/facade-test"
"$TMP/facade-test" </dev/null
cat "$HERE/services.om" "$HERE/tests/facade-io.om" > "$TMP/facade-io.om"
compile "$TMP/facade-io.om" "$TMP/facade-io"
printf "\000" > "$TMP/nul.in"
printf "OK\n" > "$TMP/ok.out"
"$TMP/facade-io" < "$TMP/nul.in" > "$TMP/facade-io.out"
cmp "$TMP/ok.out" "$TMP/facade-io.out"
cat "$HERE/services.om" "$HERE/tests/input-error.om" > "$TMP/input-error.om"
compile "$TMP/input-error.om" "$TMP/input-error"
"$TMP/input-error" <&- >/dev/null
cat "$HERE/services.om" "$HERE/tests/output-error.om" > "$TMP/output-error.om"
compile "$TMP/output-error.om" "$TMP/output-error"
"$TMP/output-error" 1>&- 2>/dev/null
"$TMP/output-error" >/dev/full 2>/dev/null
cat "$HERE/services.om" "$HERE/tests/facade-stderr.om" > "$TMP/facade-stderr.om"
compile "$TMP/facade-stderr.om" "$TMP/facade-stderr"
printf E > "$TMP/e.out"
"$TMP/facade-stderr" >/dev/null 2>"$TMP/e.err"
cmp "$TMP/e.out" "$TMP/e.err"
cat "$HERE/services.om" "$HERE/tests/facade-shortread.om" > "$TMP/shortread.om"
compile "$TMP/shortread.om" "$TMP/shortread"
printf 'A\000B' > "$TMP/shortread.in"
"$TMP/shortread" < "$TMP/shortread.in"
cat "$HERE/services.om" "$HERE/tests/facade-shortwrite.om" > "$TMP/shortwrite.om"
compile "$TMP/shortwrite.om" "$TMP/shortwrite"
python3 "$HERE/tests/shortwrite-host.py" "$TMP/shortwrite" "$TMP/shortwrite.err"
cat "$CORE" "$CLI" > "$TMP/reference.om"
compile "$TMP/reference.om" "$TMP/reference"
cat "$CORE" "$HERE/services.om" "$HERE/math-stream.om" > "$TMP/application.om"
compile "$TMP/application.om" "$TMP/application"
# Recovery, exact results/refusal codes, CRLF and partial final frame (no trailing LF).
printf 'eval 1/3+1/6\r\neval 1+\neval 7/-9\neval 2+2' > "$TMP/requests.in"
"$TMP/reference" < "$TMP/requests.in" > "$TMP/reference.out"
"$TMP/application" < "$TMP/requests.in" > "$TMP/application.out"
cmp "$TMP/reference.out" "$TMP/application.out"
# Core-capacity refusal followed by a valid request must recover in the same process.
printf 'eval ' > "$TMP/long.in"
head -c 4091 /dev/zero | tr '\000' '0' >> "$TMP/long.in"
printf '\neval 2+2\n' >> "$TMP/long.in"
"$TMP/reference" < "$TMP/long.in" > "$TMP/reference.out"
"$TMP/application" < "$TMP/long.in" > "$TMP/application.out"
cmp "$TMP/reference.out" "$TMP/application.out"
# The per-request transport limit is 65536 bytes; the extra byte causes bounded exit 1.
head -c 65536 /dev/zero | tr '\000' '0' > "$TMP/transport.in"
set +e
"$TMP/reference" < "$TMP/transport.in" > "$TMP/reference.out"; ref_rc=$?
"$TMP/application" < "$TMP/transport.in" > "$TMP/application.out"; app_rc=$?
set -e
[ "$ref_rc" -eq 1 ] && [ "$app_rc" -eq 1 ]
cmp "$TMP/reference.out" "$TMP/application.out"
# Exactly the request-session cap is processed; the next frame is not read.
awk 'BEGIN { for (i = 0; i < 65536; i++) print "eval 0" }' > "$TMP/session.in"
set +e
"$TMP/reference" < "$TMP/session.in" > "$TMP/reference.out"; ref_rc=$?
"$TMP/application" < "$TMP/session.in" > "$TMP/application.out"; app_rc=$?
set -e
[ "$ref_rc" -eq 3 ] && [ "$app_rc" -eq 3 ]
cmp "$TMP/reference.out" "$TMP/application.out"
# Compiled behavioral mutants must be valid AArch64 executables and be caught.
cat "$HERE/tests/mutants/wrong-read-rights.om" "$HERE/tests/facade-test.om" > "$TMP/mutant-rights.om"
compile "$TMP/mutant-rights.om" "$TMP/mutant-rights"
readelf -h "$TMP/mutant-rights" | grep -q 'Machine:.*AArch64'
set +e; "$TMP/mutant-rights" </dev/null >/dev/null 2>&1; rc=$?; set -e
[ "$rc" -eq 11 ]
cat "$HERE/tests/mutants/premature-eof.om" "$HERE/tests/facade-io.om" > "$TMP/mutant-eof.om"
compile "$TMP/mutant-eof.om" "$TMP/mutant-eof"
readelf -h "$TMP/mutant-eof" | grep -q 'Machine:.*AArch64'
set +e; "$TMP/mutant-eof" < "$TMP/nul.in" >/dev/null 2>&1; rc=$?; set -e
[ "$rc" -eq 21 ]
printf 'PASS USV1 hosted facade + exact math streaming, recovery, syscall errors and 2 compiled mutants\n'
