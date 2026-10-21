#!/usr/bin/env bash
# L332 rung 0 cell (a): one spark-window, idle control, Omega launch-cost test unprobed/probed/strace at two rep counts, idle again.
set -u
here=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-$HOME/workspace/targets/l332-omega/gpu_launch_cost_test}
OUT=${OUT:-$here/a-$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$OUT"
while [ ! -x "$BIN" ]; do sleep 30; done          # build lands via quiet-build
sha256sum "$BIN" > "$OUT/bin.sha256"
ledger note L332 "measure: in line for spark-window l332-rung0a" >/dev/null 2>&1
spark-window open l332-rung0a --ledger L332 --gb10 exclusive --wait > "$OUT/window.open" 2>&1 || { echo "window open failed" >> "$OUT/window.open"; exit 1; }
R="spark-window run $here/rung0.sh $OUT"
$R idle idle1 30
$R run  run-r3  -- "$BIN" --reps 3
$R run  run-r30 -- "$BIN" --reps 30
$R probe probe-r3  -- "$BIN" --reps 3
$R probe probe-r30 -- "$BIN" --reps 30
$R strace strace-r3 -- "$BIN" --reps 3
$R idle idle2 30
spark-window close > "$OUT/window.close" 2>&1
sudo rm -f "$OUT"/*.perf.data; sudo chown -R "$(id -un)" "$OUT"
ledger note L332 "measure: rung0a window done, results $OUT" >/dev/null 2>&1
echo DONE > "$OUT/DONE"
