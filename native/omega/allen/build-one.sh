#!/bin/sh
# External development orchestration. Every compiler invocation verifies its pin.
set -eu
compiler=/home/drakestapleton/handoffs/allen-omega-L694/omc
[ "$(sha256sum "$compiler" | cut -d' ' -f1)" = 191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9 ]
[ ! -e "$2" ] || { echo 'fresh output required' >&2; exit 73; }
"$compiler" < "$1" > "$2"
[ -s "$2" ]
readelf -h "$2" | grep -q 'Machine:.*AArch64'
readelf -h "$2" | grep -q 'Type:.*EXEC'
if readelf -l -d "$2" | grep -Eq 'INTERP|NEEDED'; then exit 65; fi
chmod 700 "$2"
