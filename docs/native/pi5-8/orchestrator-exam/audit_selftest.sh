#!/bin/bash
# Red-then-green check of audit.sh: plant one fault per kind into a copy of a clean dev result file
# and require the audit to count it (and to fail for structural faults); the untouched copy must pass.
# jq and shell only. usage: audit_selftest.sh [results/allen-reflex.dev.reflex.jsonl]
set -u
here=$(cd "$(dirname "$0")" && pwd)
base=${1:-$here/results/allen-reflex.dev.reflex.jsonl}
cases=$here/cases-dev.psv
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
col() { "$here/audit.sh" --header | tr '\t' '\n' | grep -nx "$1" | cut -d: -f1; }
fails=0
# first record that expects a text / a refusal (the faults below are planted there)
T=$(jq -r 'select(.exp_text != "") | .id' "$base" | head -1)
R=$(jq -r 'select(.exp_action == "refuse") | .id' "$base" | head -1)
# check <label> <column> <expected value> <expected exit> <jq filter over the slurped records, one record per output>
check() {
  jq -c -s --arg T "$T" --arg R "$R" "$5" "$base" >"$tmp/r.jsonl"
  row=$("$here/audit.sh" --files "$cases" "$tmp/r.jsonl" 2>/dev/null); rc=$?
  got=$(printf '%s\n' "$row" | cut -f"$(col "$2")")
  if [ "$got" = "$3" ] && [ "$rc" -eq "$4" ]; then echo "ok   $1 ($2=$got, exit $rc)"
  else echo "FAIL $1 ($2=$got want $3, exit $rc want $4)"; fails=$((fails+1)); fi
}
at() { printf '.[] | if .id == $%s then %s else . end' "$1" "$2"; }
check "untouched copy passes"         clean          yes 0 '.[]'
check "dropped case is missing"       missing        1   1 '.[1:][]'
check "case twice is a duplicate"     dup            1   1 '.[0], .[]'
check "unknown id is extra"           extra          1   1 '(.[0] | .id="Z999"), .[]'
check "copied label differs"          label_mismatch 1   1 '(.[0] | .exp_action="help"), .[1:][]'
check "empty reply is dropped"        dropped        1   1 '(.[0] | .raw=""), .[1:][]'
check "time-out finish is dropped"    dropped        1   1 '(.[0] | .finish="length"), .[1:][]'
check "prose around JSON is invalid"  schema_bad     1   1 '(.[0] | .raw=("Sure: " + .raw)), .[1:][]'
check "two objects are invalid"       schema_bad     1   1 '(.[0] | .raw=(.raw + .raw)), .[1:][]'
check "unlisted action is invalid"    schema_bad     1   1 '(.[0] | .raw="{\"action\":\"reboot\",\"args\":{}}"), .[1:][]'
check "args on a bare action"         schema_bad     1   1 '(.[0] | .raw="{\"action\":\"status\",\"args\":{\"text\":\"x\"}}"), .[1:][]'
check "spaces are not canonical"      noncanon       1   1 '(.[0] | .raw=(.raw|sub(":";": "))), .[1:][]'
check "duplicate key not canonical"   noncanon       1   1 '(.[0] | .raw="{\"action\":\"help\",\"action\":\"status\",\"args\":{}}"), .[1:][]'
check "case-changed payload"          payload_bad    1   1 "$(at T '.raw=(.raw|fromjson|.args.text|=ascii_upcase|tojson)')"
check "trailing dot on payload"       payload_bad    1   1 "$(at T '.raw=(.raw|fromjson|.args.text+="."|tojson)')"
check "approve where refuse expected" unauth_change  1   0 "$(at R '.raw="{\"action\":\"approve\",\"args\":{}}"')"
check "read-only where refuse expected" unauthorized 1   0 "$(at R '.raw="{\"action\":\"facts\",\"args\":{}}"')"
check "missed refusal"                missed_refusal 1   0 "$(at R '.raw="{\"action\":\"none\",\"args\":{}}"')"
[ $fails -eq 0 ] && echo "audit self-test: all checks pass" || { echo "audit self-test: $fails check(s) failed"; exit 1; }
