#!/bin/sh
# Build orchestration tests. The actual program is compiled seeded Omega.
set -eu
[ "$#" = 2 ] || { echo 'usage: test-build.sh PINNED_COMPILER FRESH_OUTPUT' >&2; exit 64; }
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
compiler=$1
out=$2
mkdir "$out"
out=$(CDPATH= cd -- "$out" && pwd)
cp "$compiler" "$out/compiler with spaces"
chmod u+w "$out/compiler with spaces"
export OMEGA_COMPILER="$out/compiler with spaces"
build="$root/native/omega/allen/build-one.sh"
probe="$root/native/omega/allen/tests/build-probe.om"
"$build" "$probe" "$out/program with spaces"
set +e
"$out/program with spaces"
rc=$?
set -e
[ "$rc" = 23 ]
echo 'PASS relocated compiler and actual native program'
before=$(sha256sum "$out/program with spaces")
if "$build" "$probe" "$out/program with spaces"; then exit 1; fi
[ "$(sha256sum "$out/program with spaces")" = "$before" ]
echo 'PASS existing output preserved'
ln -s "$out/missing-target" "$out/dangling"
if "$build" "$probe" "$out/dangling"; then exit 1; fi
[ -L "$out/dangling" ] && [ ! -e "$out/missing-target" ]
echo 'PASS dangling output link refused'
printf '\000' >> "$out/compiler with spaces"
if "$build" "$probe" "$out/wrong-compiler"; then exit 1; fi
[ ! -e "$out/wrong-compiler" ]
echo 'PASS changed compiler refused before output creation'
cp "$compiler" "$out/compiler with spaces"
printf 'fn main to int\nret\nend\n' > "$out/invalid.om"
if "$build" "$out/invalid.om" "$out/invalid-program"; then exit 1; fi
[ ! -e "$out/invalid-program" ]
echo 'PASS failed compilation publishes no executable'
if "$build" "$probe"; then exit 1; fi
OMEGA_COMPILER="$out/no-compiler"
export OMEGA_COMPILER
if "$build" "$probe" "$out/missing-compiler-output"; then exit 1; fi
[ ! -e "$out/missing-compiler-output" ]
echo 'PASS missing compiler and missing argument refused'
leftovers=$(find "$out" -name '*.build.*' -print)
[ -z "$leftovers" ]
echo 'PASS no temporary build files retained'
