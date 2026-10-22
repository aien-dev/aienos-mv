set -eu
cd "$HOME/workspace/aienos-omega-driver-direct"
r=$(mktemp -d "$PWD/build/replay-boot.XXXXXXXX")
mkdir "$r/driver" "$r/omega"
git archive HEAD native/omega native/kernel/gpu native/kernel/dev native/linux/aien-gpu evidence/own-gpu-driver/rung2/window1_sim_test.sh | tar -x -C "$r/driver"
git -C "$HOME/workspace/omega-osc-manual-migration" archive 9b072eb492e106d23f7eed984c5645251601adee seed | tar -x -C "$r/omega"
(cd "$r/omega"; sh seed/omega-from-seed.sh)
python3 "$r/driver/native/omega/tests/bootseq.py" "$r/driver" "$r/omega/seed/out/omc" > "$r/result.json"
python3 "$r/driver/native/omega/tests/bootseq-io.py" "$r/driver"
python3 "$r/driver/native/omega/tests/regtrace.py" "$r/driver" "$r/omega/seed/out/omc" "$r/driver/build/omega-regtrace/regtrace" > "$r/trace.json"
timeout 120 quiet-build make -C "$r/driver/native/linux/aien-gpu" all tools sim-test OMEGA_CC="$r/omega/seed/out/omc"
python3 - "$r/result.json" "$r/trace.json" <<'PY'
import json,sys
r=json.load(open(sys.argv[1]));assert r['cases']==len(r['results'])==125 and r['failures']==0 and all(x['pass'] for x in r['results'])
print(json.dumps({k:r[k] for k in ['cases','failures','differential_cases','source_sha256','binary_sha256','oracle_source_sha256']}))
t=json.load(open(sys.argv[2]));assert t['cases']==23 and t['failures']==0
print('TRACE_REGRESSION_23_PASS')
PY
printf 'SEED_NATIVE_BOOT_REPLAY_PASS\n'
