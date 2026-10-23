set -eu
cd "$HOME/workspace/aienos-omega-driver-direct"
r=$(mktemp -d "$PWD/build/replay-bootsim.XXXXXXXX")
mkdir "$r/driver" "$r/omega"
git archive HEAD native/omega native/kernel/gpu native/kernel/dev native/linux/aien-gpu evidence/own-gpu-driver/rung2/window1_sim_test.sh | tar -x -C "$r/driver"
git -C "$HOME/workspace/omega-osc-manual-migration" archive 9b072eb492e106d23f7eed984c5645251601adee seed | tar -x -C "$r/omega"
(cd "$r/omega"; sh seed/omega-from-seed.sh)
python3 "$r/driver/native/omega/tests/regtrace.py" "$r/driver" "$r/omega/seed/out/omc" "$r/driver/build/omega-regtrace/regtrace" > "$r/trace.json"
if timeout 180 quiet-build make -C "$r/driver/native/linux/aien-gpu" tools sim-test OMEGA_CC="$r/omega/seed/out/omc" > "$r/make.log" 2>&1; then
  cat "$r/make.log"
else
  cat "$r/make.log"
  exit 1
fi
python3 - "$r/make.log" "$r/trace.json" <<'PY'
import json,re,sys
s=open(sys.argv[1]).read();objects=[]
for m in re.finditer(r'(?m)^\{',s):
 try:objects.append(json.JSONDecoder().raw_decode(s[m.start():])[0])
 except ValueError:pass
for count in [125,146]:
 r=next(x for x in objects if x.get('cases')==count)
 assert r['failures']==0 and len(r['results'])==count and all(x['pass'] for x in r['results'])
 print(json.dumps({k:r[k] for k in ['cases','failures','source_sha256','binary_sha256']}))
io=[x for x in objects if x.get('cases')==4];assert len(io)==2 and all(all(y['pass'] for y in x['results']) for x in io)
assert s.count('WINDOW1_SIM_TEST: PASS')==2 and 'BOOT_NATIVE_GATE_PASS' in s
r=json.load(open(sys.argv[2]));assert r['cases']==23 and r['failures']==0
print('TRACE_REGRESSION_23_PASS')
PY
printf 'SEED_NATIVE_CAUSAL_BOOT_REPLAY_PASS\n'
