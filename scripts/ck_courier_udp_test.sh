#!/usr/bin/env bash
# ck_courier_udp_test.sh -- host test of Allen's Ethernet courier.
#
# The kernel's own courier code (native/kernel/x86/courier_udp_x86.c, built with
# -DCK_COURIER_HOST) talks UDP to the real relay (crates/allen-courier --udp),
# which asks a model server. Three runs: a clean wire, a wire that loses every
# 3rd frame in each direction, and a model server that answers "busy" (503)
# twice before it answers. The model here is a small stand-in so the run is
# exact and quick (the lossy runs lose about 1 frame in 3 and 1 in 2, each way); with ALLEN_MODEL=host:port (the real server) a fourth run
# sends the same job to the real model. The real kernel cannot run this in QEMU
# (QEMU has no Broadcom chip), so this is the check of the courier's protocol.
# Ends with "AIENOS_COURIER_UDP: PASS" or "AIENOS_COURIER_UDP: FAIL <why>".
set -u
cd "$(dirname "$0")/.."
K=native/kernel
tag=AIENOS_COURIER_UDP
fail() { echo "${tag}: FAIL $*"; exit 1; }
W=$(mktemp -d "${TMPDIR:-/tmp}/ck_courier_udp.XXXXXX")
pids=()
cleanup() {
    for p in ${pids[@]+"${pids[@]}"}; do kill "$p" 2>/dev/null || true; done
    [ -n "${KEEP_LOGS:-}" ] && cp -r "$W" "${KEEP_LOGS}" 2>/dev/null
    rm -rf "$W"
}
trap cleanup EXIT

${HOSTCC:-cc} -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror -DCK_COURIER_HOST -I$K/x86 -Inative/net \
    -o "$W/test" $K/x86/tests/courier_udp_test.c $K/x86/courier_udp_x86.c native/net/aienos_net.c ||
    fail "the test does not build"
if [ -n "${ALLEN_RELAY:-}" ]; then
    relay="$ALLEN_RELAY" # a relay built elsewhere
else
    cargo build -q --release --offline -p allen-courier 2>"$W/cargo.err" || { tail -5 "$W/cargo.err"; fail "the relay does not build"; }
    relay="${CARGO_TARGET_DIR:-target}/release/allen-courier"
fi

free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

# a stand-in model server: 503 (Retry-After: 1) for the first BUSY requests, then a streamed answer
cat >"$W/fake_model.py" <<'EOF'
import json, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer
busy = int(sys.argv[2])
seen = [0]
WORDS = ("I am here. The rain is soft tonight and I like listening to it with you. "
         "Tell me more about your day when you are ready.").split(" ")
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n))
        seen[0] += 1
        if seen[0] <= busy:
            self.send_response(503); self.send_header("Retry-After", "1"); self.send_header("Content-Length", "0")
            self.send_header("Connection", "close"); self.end_headers(); return
        assert body["messages"][0]["role"] == "system" and "Allen" in body["messages"][0]["content"]
        self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.send_header("Connection", "close")
        self.end_headers()
        for w in WORDS:
            self.wfile.write(("data: " + json.dumps({"choices": [{"delta": {"content": w + " "}}]}) + "\n\n").encode()); self.wfile.flush()
            time.sleep(0.03)
        self.wfile.write(b"data: [DONE]\n\n"); self.wfile.flush(); self.close_connection = True
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
EOF

printf 'JOB 1 chat Answer as Allen.\thello allen, how was your day\tself=Allen\tname=Drake\tstarts=20\tnote asked: hi / said: Hello\n' >"$W/job.txt"

run() { # label model drop_every
    local label="$1" model="$2" drop="$3" rp
    rp=$(free_port)
    "$relay" --udp "$rp" --model "$model" --work "$W/work" --log-dir "$W/log-$label" >"$W/relay-$label.log" 2>&1 &
    rpid=$!
    pids+=("$rpid")
    sleep 0.5
    "$W/test" "$rp" "$drop" "$W/job.txt" 90 | tee "$W/test-$label.log" | grep -a -E "hello after|first part|PASS|FAIL" | sed "s/^/[$label] /"
    grep -aq "COURIER_UDP: PASS" "$W/test-$label.log" || fail "run $label"
    kill "$rpid" 2>/dev/null || true
    wait "$rpid" 2>/dev/null
}

mp=$(free_port)
python3 "$W/fake_model.py" "$mp" 0 &
fake1=$!
pids+=("$fake1")
sleep 0.5
run clean "127.0.0.1:$mp" 0
run lossy "127.0.0.1:$mp" 3
run lossy2 "127.0.0.1:$mp" 2
grep -aq "job resends [1-9]" "$W/test-lossy2.log" || fail "the lost job was not sent again"
kill "$fake1" 2>/dev/null || true
wait "$fake1" 2>/dev/null
mp2=$(free_port)
python3 "$W/fake_model.py" "$mp2" 2 &
pids+=($!)
sleep 0.5
run busy "127.0.0.1:$mp2" 0
grep -aqE "waits 2|waits [2-9]" "$W/relay-busy.log" || fail "the relay did not wait out the busy answers"
echo "[busy] relay waited out the busy answers: $(grep -a 'waits' "$W/relay-busy.log" | head -1 | sed 's/.*first in/first in/')"
[ -s "$W"/log-clean/*.jsonl ] || fail "no line in the learning log"
grep -aq '"ask":"hello allen, how was your day"' "$W"/log-clean/*.jsonl || fail "the learning log lacks the question"
echo "learning log line: $(head -c 300 "$W"/log-clean/*.jsonl)"
if [ -n "${ALLEN_MODEL:-}" ]; then
    run real "$ALLEN_MODEL" 0
fi
echo "${tag}: PASS"
