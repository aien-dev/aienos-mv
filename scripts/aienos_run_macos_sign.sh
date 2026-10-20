#!/bin/sh
# aienos_run_macos_sign.sh -- sign the macOS aienos-run with the one entitlement it needs
# (com.apple.security.virtualization; Virtualization.framework refuses to start a VM
# for a program without it). docs/AIENOS_RUN_MACOS.md "Signing and notarization".
#
#   scripts/aienos_run_macos_sign.sh <binary>                 ad-hoc (local use, curl | sh)
#   scripts/aienos_run_macos_sign.sh <binary> "<identity>"    Developer ID, hardened runtime,
#                                                             secure timestamp (release)
#
# It never notarizes: notarization needs the owner's Apple Developer account and is an
# owner step (the commands are printed, not run). Exit 0 signed and verified, 1 failed,
# 2 usage.
set -eu
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: $0 <binary> [identity]" >&2; exit 2; }
bin=$1
ent="$(cd "$(dirname "$0")/.." && pwd)/crates/aienos-run/macos/aienos-run.entitlements"
if [ $# -eq 1 ]; then
    codesign --force --sign - --entitlements "$ent" "$bin"
else
    codesign --force --sign "$2" --options runtime --timestamp --entitlements "$ent" "$bin"
fi
codesign --verify --strict "$bin"
codesign -d --entitlements - "$bin" 2>/dev/null | grep -q com.apple.security.virtualization ||
    { echo "entitlement missing after signing" >&2; exit 1; }
echo "signed: $bin ($( [ $# -eq 1 ] && echo ad-hoc || echo "$2" ))"
if [ $# -eq 2 ]; then
    cat <<NOTE
not notarized (owner step). To notarize, with the owner's notarytool profile:
  ditto -c -k --keepParent "$bin" aienos-run-macos-aarch64.zip
  xcrun notarytool submit aienos-run-macos-aarch64.zip --keychain-profile <profile> --wait
A bare program cannot be stapled; Gatekeeper looks the ticket up online.
NOTE
fi
