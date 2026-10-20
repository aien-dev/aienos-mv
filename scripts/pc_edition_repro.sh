#!/usr/bin/env bash
# pc_edition_repro.sh -- build the AienOS PC edition twice from the same commit in two
# fresh work folders and compare every release file byte for byte (sha256). Host only.
# Usage: bash scripts/pc_edition_repro.sh
# Final line: PC_EDITION_REPRO: PASS commit=<sha> (<n> files identical)
#             PC_EDITION_REPRO: FAIL <files that differ>
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
for k in a b; do
    bash scripts/pc_edition_build.sh --work "${tmp}/work-${k}" --out "${tmp}/out-${k}" > "${tmp}/build-${k}.log" 2>&1 \
        || { tail -5 "${tmp}/build-${k}.log"; echo "PC_EDITION_REPRO: FAIL build ${k}"; exit 1; }
done
diff=()
n=0
for f in "${tmp}/out-a"/*; do
    b="${tmp}/out-b/$(basename "$f")"
    n=$((n + 1))
    cmp -s "$f" "$b" || diff+=("$(basename "$f")")
done
cat "${tmp}/out-a/SHA256SUMS"
if (( ${#diff[@]} )); then echo "PC_EDITION_REPRO: FAIL ${diff[*]}"; exit 1; fi
echo "PC_EDITION_REPRO: PASS commit=$(git rev-parse HEAD) (${n} files identical)"
