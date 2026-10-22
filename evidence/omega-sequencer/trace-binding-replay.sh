set -eu
cd "$HOME/workspace/aienos-omega-driver-direct"
r=$(mktemp -d "$PWD/build/replay-trace-binding.XXXXXXXX")
mkdir "$r/driver" "$r/omega"
git archive HEAD native/omega native/kernel/gpu native/kernel/dev native/linux/aien-gpu evidence/own-gpu-driver/rung2/window1_sim_test.sh | tar -x -C "$r/driver"
git -C "$HOME/workspace/omega-osc-manual-migration" archive 9b072eb492e106d23f7eed984c5645251601adee seed | tar -x -C "$r/omega"
(cd "$r/omega"; sh seed/omega-from-seed.sh)
python3 "$r/driver/native/omega/tests/regtrace.py" "$r/driver" "$r/omega/seed/out/omc" "$r/driver/build/omega-regtrace/regtrace" > "$r/result.json"
python3 "$r/driver/native/omega/tests/regtrace-guards.py" "$r/driver" "$r/omega/seed/out/omc"
timeout 120 quiet-build make -C "$r/driver/native/linux/aien-gpu" all tools sim-test OMEGA_CC="$r/omega/seed/out/omc"
python3 - "$r/result.json" <<'PY'
import json,sys
r=json.load(open(sys.argv[1]));assert r['cases']==23 and r['failures']==0
print(json.dumps({k:r[k] for k in ['cases','failures','source_sha256','binary_sha256']}))
PY
python3 "$r/driver/native/omega/tests/regtrace-binding.py" "$r/driver" "$r/omega/seed/out/omc"
printf 'SEED_NATIVE_TRACE_BINDING_REPLAY_PASS\n'
