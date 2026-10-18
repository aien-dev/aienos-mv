#!/bin/bash
# make-task-units.sh: build the three pipeline stage units of the native task lifecycle test (aienos-mv#429) and their
# realization receipts. Usage:
#   make-task-units.sh SPEC_DIR OMEGA_DIR [OUT_DIR]     write tgen/txform/tsink .unit and .rcpt to OUT_DIR (default here)
#   make-task-units.sh --check SPEC_DIR OMEGA_DIR       rebuild in a temporary directory and require every committed
#                                                      .unit and .rcpt to be byte-identical (oscc, the container
#                                                      signature and the receipt signature are all deterministic)
#   SPEC_DIR   a checkout of aien-protocols specs/osc-unit-artifact (tools/make-vectors.sh helpers, keys/test1.pub)
#   OMEGA_DIR  a checkout of aien-dev/omega with src/compiler (oscc) and src/checker2 (receipts); built into a
#              temporary OUT_DIR, omega's own build/ is not touched
# The IR and code are REAL oscc output of tgen.osc, txform.osc and tsink.osc (one function each, entry records from
# oscc's .entries). Containers: TEST signer (the spec's throwaway key test1), no caps, max_stack_bytes 16384,
# cpu_ticks 1000. Receipts: checker2 TEST key, target aienos-ck-aarch64-el0, epoch 10, expiry 1000, then verified by
# checker2 under the kernel's qualification policy (floor 10, epoch 20) and required VALID.
# QEMU (aarch64 virt), TEST signer, not physical. Needs bash, make, cc, xxd, sha256sum, openssl (Ed25519). No Python.
set -eu
export LC_ALL=C
mydir=$(cd "$(dirname "$0")" && pwd)
check=0
if [ "${1:-}" = --check ]; then check=1; shift; fi
SPEC=$(cd "${1:?SPEC_DIR}" && pwd)
OMEGA=$(cd "${2:?OMEGA_DIR}" && pwd)
OUT=${3:-$mydir}
STACK=16384
TICKS=1000
TARGET=aienos-ck-aarch64-el0
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ "$check" = 1 ]; then OUT=$work/out; fi
mkdir -p "$OUT"

make -s -C "$OMEGA" OUT_DIR="$work/b" "$work/b/compiler/oscc" "$work/b/checker2/checker2" >/dev/null
OSCC=$work/b/compiler/oscc
C2=$work/b/checker2/checker2
for u in tgen txform tsink; do
    "$OSCC" "$mydir/$u.osc" "$work/$u" >"$work/$u.oscc.txt"
done

n=$(grep -n '^cp "\$SRC/min.ir"' "$SPEC/tools/make-vectors.sh" | head -1 | cut -d: -f1)
{ echo 'here='"$SPEC/tools"; sed -n "1,$((n - 1))p" "$SPEC/tools/make-vectors.sh" | sed '/^here=/d'; } >"$work/helpers.sh"
OUT_UNIT=$OUT
WORK=$work
set -- "$work/keys-out" "$work/keys-out" # helpers read OUT_DIR=$1, KEYS_DIR=$2
mkdir -p "$work/keys-out"
SRC=$SPEC/src
# shellcheck disable=SC1090
. "$work/helpers.sh"
OUT=$OUT_UNIT
work=$WORK
cmp -s "$KEYS/test1.pub" "$SPEC/keys/test1.pub" || { echo "TEST key mismatch: generator is not the frozen one" >&2; exit 1; }

le() { echo "$1" | sed -E 's/(..)/\1 /g' | awk '{for(i=NF;i>0;i--) printf "%s ", $i}'; }
for u in tgen txform tsink; do
    nfn=$(wc -l <"$work/$u.entries")
    while read -r idx nregs ret k0 k1 k2 k3 k4 k5 off name; do
        entry "$idx" "$nregs" "$ret" "$k0" "$k1" "$k2" "$k3" "$k4" "$k5" "$off" "$name"
    done <"$work/$u.entries" >"$work/ent_$u"
    caps_none >"$work/caps0"
    build "$OUT/$u.unit" test1 1 5 1 "$nfn" "$work/$u.ir" "$work/$u.code" "$work/ent_$u" "$work/caps0"
    # header offset 36: max_stack_bytes (u32 LE), offset 40: cpu_ticks (u64 LE); both are signed, so re-sign
    s=$(printf '%08x' "$STACK")
    t=$(printf '%016x' "$TICKS")
    # shellcheck disable=SC2046
    patch "$OUT/$u.unit" 36 $(le "$s") $(le "$t") >"$OUT/$u.unit.new"
    mv "$OUT/$u.unit.new" "$OUT/$u.unit"
    resign "$OUT/$u.unit" test1
    rm -rf "$work/store"
    mkdir -p "$work/store"
    line=$("$C2" receipt issue "$work/$u.ir" "$work/$u.code" --target "$TARGET" --epoch 10 --expiry 1000 --out-dir "$work/store")
    path=${line#receipt=}
    [ -f "$path" ] || { echo "receipt issue $u: $line" >&2; exit 1; }
    cat "$path" >"$OUT/$u.rcpt"
    v=$("$C2" receipt verify "$OUT/$u.rcpt" "$work/$u.ir" "$work/$u.code" --allow-test-key --floor 10 --epoch 20 \
        --target "$TARGET" 2>&1 || true)
    [ "${v#receipt=}" = VALID ] || { echo "checker2 verify $u.rcpt: $v" >&2; exit 1; }
    echo "built $u.unit ($nfn function, $(grep code_sha256 "$work/$u.oscc.txt")) and $u.rcpt (checker2 VALID)"
done

if [ "$check" = 1 ]; then
    bad=0
    for f in tgen.unit txform.unit tsink.unit tgen.rcpt txform.rcpt tsink.rcpt; do
        if cmp -s "$OUT/$f" "$mydir/$f"; then echo "same $f"; else echo "DIFFERS $f"; bad=1; fi
    done
    exit "$bad"
fi
