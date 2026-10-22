set -eu
cd "$HOME/workspace/aienos-omega-driver-direct"
replay=$(mktemp -d "$PWD/build/replay-sequencer-v2.XXXXXXXX")
mkdir "$replay/driver" "$replay/omega"
git archive HEAD native/omega native/kernel/gpu/gsp_seq.c native/kernel/gpu/gsp_seq.h evidence/omega-sequencer/v2-wolfram.json | tar -x -C "$replay/driver"
git -C "$HOME/workspace/omega-osc-manual-migration" archive 9b072eb492e106d23f7eed984c5645251601adee seed | tar -x -C "$replay/omega"
(cd "$replay/omega"; sh seed/omega-from-seed.sh)
python3 "$replay/driver/native/omega/tests/check.py" "$replay/driver" "$replay/omega" > "$replay/driver/evidence/omega-sequencer/v2-green.json"
python3 "$replay/driver/native/omega/tests/wolfram.py" "$replay/driver/evidence/omega-sequencer"
python3 "$replay/driver/native/omega/tests/io.py" "$replay/driver"
python3 - "$replay/driver/evidence/omega-sequencer/v2-green.json" <<'PY'
import sys,json
from pathlib import Path
r=json.loads(Path(sys.argv[1]).read_text());assert r['cases']==222 and r['failures']==0 and r['preflight_refusals']==122
print(json.dumps({k:r[k] for k in ['cases','failures','preflight_refusals','source_sha256','binary_sha256','oracle_source_sha256']}))
PY
printf 'SEED_NATIVE_SEQUENCER_V2_REPLAY_PASS\n'
