#!/bin/sh
# External build/output comparison only. All behavioral tests execute Omega ELFs.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
cd "$root"
out=$1
mkdir -p "$out"
[ -z "$(ls -A "$out")" ] || { echo 'gate requires fresh output directory' >&2; exit 73; }
core="$out/core.om"
cat native/omega/allen/base.om native/omega/allen/vendor/sha256-core.om \
 native/omega/allen/runtime.om native/omega/allen/code.om \
 native/omega/allen/courier.om native/omega/allen/memory.om \
 native/omega/allen/intent.om native/omega/allen/reply.om \
 native/omega/allen/view.om native/omega/allen/scene.om native/omega/allen/paint.om > "$core"
for name in runtime code courier memory intent reply view scene paint edges fixture golden render; do
 cat "$core" > "$out/$name.om"
 case "$name" in golden|render) ;; *) cat native/omega/allen/tests/check.om >> "$out/$name.om" ;; esac
 cat "native/omega/allen/tests/$name.om" >> "$out/$name.om"
 native/omega/allen/build-one.sh "$out/$name.om" "$out/$name"
 case "$name" in
 fixture)
  "$out/$name" < evidence/allen-omega-L694/short.wire > "$out/short.log"
  "$out/$name" < evidence/allen-omega-L694/long.wire > "$out/long.log"
  ;;
 golden) "$out/$name" > "$out/memory-native.bin" ;;
 render) "$out/$name" > "$out/mind-component.ppm" ;;
 *) "$out/$name" > "$out/$name.log" ;;
 esac
 printf '%s compile=0 native-exit=0\n' "$name"
done
native/omega/allen/golden.sh "$out/reference"
cmp "$out/memory-native.bin" "$out/reference/memory-golden.bin"
cat "$core" native/omega/allen/main.om > "$out/allen.om"
native/omega/allen/build-one.sh "$out/allen.om" "$out/allen"
"$out/allen" < evidence/allen-omega-L694/stdio-input.txt > "$out/stdio-job.txt" 2> "$out/stdio-display.txt"
cmp "$out/stdio-job.txt" evidence/allen-omega-L694/stdio-job.txt
cmp "$out/stdio-display.txt" evidence/allen-omega-L694/stdio-display.txt
printf 'stdio native-exit=0 golden-cmp=0\n'
# Compiled behavioral mutants must be valid ELFs and fail the original gates.
sed 's/if or eq get R 1 0 ne seq get R 0/if eq get R 1 0/' "$core" > "$out/stale-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/runtime.om >> "$out/stale-mutant.om"
sed 's/if ne ulong codecrc get C 3/if false/' "$core" > "$out/crc-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/code.om >> "$out/crc-mutant.om"
sed 's/ret eq d 0/ret true/' "$core" > "$out/hash-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/memory.om >> "$out/hash-mutant.om"
sed 's/if ne d 0/if false/' "$core" > "$out/readback-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/memory.om >> "$out/readback-mutant.om"
sed 's/if ne run get Q 0/if false/' "$core" > "$out/session-mutant.om"
cat native/omega/allen/tests/check.om native/omega/allen/tests/courier.om >> "$out/session-mutant.om"
for name in stale crc hash readback session; do
 native/omega/allen/build-one.sh "$out/$name-mutant.om" "$out/$name-mutant"
 set +e
 "$out/$name-mutant" > "$out/$name-mutant.log"
 rc=$?
 set -e
 if [ "$rc" -le 0 ] || [ "$rc" -ge 100 ]; then echo "mutant did not fail behaviorally: $name exit=$rc" >&2; exit 1; fi
 printf '%s mutant compile=0 rejected-exit=%s\n' "$name" "$rc"
done
find native/omega/allen -type f -print0 | sort -z | xargs -0 sha256sum > "$out/source.sha256"
find "$out" -maxdepth 1 -type f -perm -100 -exec sha256sum {} + > "$out/binaries.sha256"
printf 'ALLEN_OMEGA_HOST: PASS; target=NOT_RUN; live=NOT_RUN\n'
