#!/bin/sh
# receipt.sh [BASE] -- L339 before/after receipt for the native crypto speed work.
# Builds receipt.c against BASE (default origin/main) and against this
# worktree with the same flags, runs both pinned to one core, and fails
# unless the output digests are equal. Run it under quiet-measure on the Spark.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
base=${1:-origin/main}
work=$(mktemp -d /tmp/l339-receipt.XXXXXX)
git -C "$root" archive "$base" native/argus native/crypto native/sig native/net | tar -x -C "$work"
build() { # $1 = tree root, $2 = output
    n=$1/native
    cc -std=gnu11 -O2 -I"$n/crypto" -I"$n/sig" -I"$n/argus" -I"$n/net" -o "$2" "$here/receipt.c" \
        "$n/crypto/aes.c" "$n/crypto/polyval.c" "$n/crypto/gcm_siv.c" "$n/crypto/ct.c" "$n/argus/sha256.c" \
        "$n/sig/ed25519.c" "$n/sig/sha512.c" "$n/sig/ct.c" "$n/net/x25519.c"
}
build "$work" "$work/before"
build "$root" "$work/after"
cpu=${RECEIPT_CPU:-5}
echo "== before: $base $(git -C "$root" rev-parse --short "$base")"
taskset -c "$cpu" "$work/before" | tee "$work/before.txt"
echo "== after: worktree $(git -C "$root" rev-parse --short HEAD)$(git -C "$root" diff --quiet || echo +dirty)"
taskset -c "$cpu" "$work/after" | tee "$work/after.txt"
b=$(grep '^output digest' "$work/before.txt"); a=$(grep '^output digest' "$work/after.txt")
[ "$a" = "$b" ] && echo "RECEIPT: outputs identical" || { echo "RECEIPT: OUTPUTS DIFFER"; exit 1; }
