set -eu
cd "$HOME/workspace/aienos-omega-driver-direct"
r=$(mktemp -d "$PWD/build/replay-regpolicy.XXXXXXXX")
mkdir "$r/driver" "$r/omega"
git archive HEAD native/omega native/kernel/gpu/gsp_seq.c native/kernel/gpu/gsp_seq.h native/linux/aien-gpu/aien_gpu.c evidence/omega-sequencer/policy-wolfram.json | tar -x -C "$r/driver"
git -C "$HOME/workspace/omega-osc-manual-migration" archive 9b072eb492e106d23f7eed984c5645251601adee seed | tar -x -C "$r/omega"
(cd "$r/omega"; sh seed/omega-from-seed.sh)
python3 "$r/driver/native/omega/tests/regpolicy.py" "$r/driver" "$r/omega" > "$r/driver/evidence/omega-sequencer/policy-green.json"
python3 "$r/driver/native/omega/tests/regpolicy-io.py" "$r/driver"
python3 "$r/driver/native/omega/tests/regpolicy-wolfram.py" "$r/driver/evidence/omega-sequencer"
python3 "$r/driver/native/omega/tests/check.py" "$r/driver" "$r/omega" > "$r/driver/evidence/omega-sequencer/v2-green.json"
python3 "$r/driver/native/omega/tests/io.py" "$r/driver"
python3 - "$r/driver/evidence/omega-sequencer" <<'PY'
import pathlib,sys,json
p=pathlib.Path(sys.argv[1]);r=json.loads((p/'policy-green.json').read_text());s=json.loads((p/'v2-green.json').read_text())
assert r['cases']==32 and r['requests']==3332 and r['failures']==0 and r['allowlist_equality'];assert s['cases']==222 and s['failures']==0
print(json.dumps({k:r[k] for k in ['cases','requests','failures','allowlist_equality','source_sha256','binary_sha256','shim_sha256']}));print('GSQ2_REGRESSION_PASS222')
PY
printf 'SEED_NATIVE_REGPOLICY_REPLAY_PASS\n'
