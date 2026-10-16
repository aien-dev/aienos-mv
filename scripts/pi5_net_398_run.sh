#!/usr/bin/env bash
# One Pi <-> Spark session run (aienos-mv#398). Run it under the shared Pi lock, never hold the lock between runs:
#   flock ~/workspace/pi5/.pi5-run.lock scripts/pi5_net_398_run.sh <BOOTAA64.EFI> <tag> <pispark_peer binary>
# Only once the lock is held: starts the Spark-side capture (tcpdump on enP7s7, Pi traffic only, ssh excluded) and the
# Spark peer (TEST identities), then the normal Pi harness (pi5-run.sh, KLOG=1), then stops the capture and the peer.
# Evidence: ~/workspace/pi5-runs/<tag> (Pi side, from pi5-run.sh) and ~/workspace/pi5-runs/<tag>-spark (Spark side).
set -euo pipefail
efi="$1"; tag="$2"; peer="$3"
pi_ip=192.168.1.192; pi_mac=2c:cf:67:3f:54:76; ifc=enP7s7
sp="$HOME/workspace/pi5-runs/${tag}-spark"
test ! -e "${sp}" || { echo "REFUSE: ${sp} exists"; exit 1; }
mkdir -p "${sp}"
cp "${peer}" "${sp}/pispark_peer"
sha256sum "${efi}" "${sp}/pispark_peer" > "${sp}/PINS.sha256"
date -u +%FT%TZ > "${sp}/start.txt"
sudo -n tcpdump -i "${ifc}" -U -n -w "${sp}/spark.pcap" \
    "(host ${pi_ip} or ether host ${pi_mac}) and not tcp port 22" > "${sp}/tcpdump.err" 2>&1 &
cap=$!
(cd "${sp}" && exec ./pispark_peer "${pi_ip}" 1800) > "${sp}/peer.log" 2>&1 &
pp=$!
rc=0
KLOG=1 "$HOME/workspace/pi5/pi5-run.sh" "${efi}" "${tag}" > "${sp}/pi5-run.out" 2>&1 || rc=$?
kill "${pp}" 2>/dev/null || true
sudo -n kill "${cap}" 2>/dev/null || true
wait "${pp}" 2>/dev/null || true
wait "${cap}" 2>/dev/null || true
date -u +%FT%TZ > "${sp}/end.txt"
echo "pi5-run rc=${rc}" | tee -a "${sp}/end.txt"
exit "${rc}"
