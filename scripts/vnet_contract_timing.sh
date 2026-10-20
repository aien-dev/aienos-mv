#!/bin/sh
# vnet_contract_timing.sh -- timing receipt, hand-written vs contract-generated
# virtio-net driver (ADR 0021 "Migration" rule d).
#
# Builds both bench binaries (make -C native/net contract-bench, -O2, same model,
# same loop), then runs them interleaved A B A B ... ROUNDS times, each run REPS
# repetitions of TRIPS round trips. Run it under quiet-measure:
#   quiet-measure scripts/vnet_contract_timing.sh [ROUNDS] [REPS] [TRIPS]
# The noise bound is measured, not assumed: the hand driver's runs are split
# into odd and even rounds and the two medians compared (A vs A). The verdict is
# WITHIN_NOISE when |contract - hand| <= max(noise, 2% of hand), else SLOWER/FASTER.
# Writes native/net/contract/receipts/vnet-timing-<UTC>.txt and prints it.
set -eu
cd "$(dirname "$0")/.."
rounds="${1:-20}"; reps="${2:-5}"; trips="${3:-20000}"
make -s -C native/net contract-bench >/dev/null
bin=native/net/out
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM
r=1
while [ "$r" -le "$rounds" ]; do
    "$bin/vnet_bench_hand" hand "$reps" "$trips" 2>/dev/null | sed "s/^/round=$r /" >> "$tmp/runs"
    "$bin/vnet_bench_contract" contract "$reps" "$trips" 2>/dev/null | sed "s/^/round=$r /" >> "$tmp/runs"
    r=$((r + 1))
done
grep -c ' violation' "$tmp/runs" >/dev/null 2>&1 && { echo "VNET_TIMING: FAIL (model violation)"; grep violation "$tmp/runs"; exit 1; }

# field FIELD IMPL [PARITY]: values of FIELD (init_ns|rtt_ns) for IMPL, optional odd/even rounds
field() {
    awk -v f="$1" -v impl="$2" -v par="${3:-}" '
        { split($1, a, "="); rnd = a[2]; ok = 0
          for (i = 2; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
          if (v["impl"] != impl) next
          if (par == "odd" && rnd % 2 == 0) next
          if (par == "even" && rnd % 2 == 1) next
          print v[f] }' "$tmp/runs"
}
median() { sort -n | awk '{ a[NR] = $1 } END { if (NR == 0) { print 0; exit } m = int((NR + 1) / 2); if (NR % 2) print a[m]; else print int((a[m] + a[m + 1]) / 2) }'; }

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p native/net/contract/receipts
out="native/net/contract/receipts/vnet-timing-${stamp}.txt"
{
    echo "receipt vnet-timing ${stamp}"
    echo "host $(hostname) $(uname -m) kernel $(uname -r) cores $(taskset -pc $$ 2>/dev/null | sed 's/.*: //')"
    echo "commit $(git rev-parse HEAD) dirty=$(git status --porcelain | grep -c . || true)"
    echo "cc $(${CC:-cc} --version | head -1)"
    echo "contract $(sed -n 's/^const char vnet_contract_digest\[65\] = "\(.*\)";/\1/p' native/net/contract/gen/driver/vnet_contract.c)"
    echo "method rounds=${rounds} reps=${reps} trips=${trips} interleaved hand,contract; median over all reps"
    verdict=WITHIN_NOISE
    for f in init_ns rtt_ns; do
        h="$(field "$f" hand | median)"; c="$(field "$f" contract | median)"
        ho="$(field "$f" hand odd | median)"; he="$(field "$f" hand even | median)"
        noise=$((ho > he ? ho - he : he - ho)); floor=$((h / 50)); [ "$noise" -lt "$floor" ] && noise=$floor
        d=$((c - h)); ad=$((d < 0 ? -d : d))
        v=WITHIN_NOISE; [ "$ad" -gt "$noise" ] && { [ "$d" -gt 0 ] && v=SLOWER || v=FASTER; }
        [ "$v" = SLOWER ] && verdict=SLOWER
        echo "${f} hand_median=${h} contract_median=${c} diff=${d} noise_bound=${noise} (hand odd ${ho} vs even ${he}, floor 2%) ${v}"
    done
    echo "VNET_TIMING: ${verdict}"
} > "$out"
cat "$out"
echo "receipt: $out"
