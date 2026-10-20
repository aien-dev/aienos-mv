#!/usr/bin/env bash
# Interplane join gate for the AIENOS C kernel: AIENOS_CK_INTERPLANE.
# Boots five copies of the CK_INTERPLANE=1 full image in QEMU AArch64 (UEFI,
# smmuv3 vIOMMU, virtio-net with ACCESS_PLATFORM, NVMe boot disk) at the same
# time, all on ONE virtual LAN (QEMU socket netdev in UDP mode to a per-run
# hub on loopback: native/kernel/tools/ck_lan_hub.c copies every frame to every
# other instance; the Spark loopback has no multicast). Each boot volume carries
# its own provisioning bundle
# \EFI\AIENOS\JOIN.BIN written by native/net/tools/ipj_tool (fresh owner and
# machine keys from /dev/urandom every run; the owner secret key never leaves
# the tool):
#   spark, pi, mac  production mode, roster generation 5 (old revoked)
#   old             roster generation 4, from before its revocation
#   tst             a roster holding a TEST-kind entry
# PASS needs, from the serial logs:
#   (a) spark, pi and mac each join (roster_gen=5 members=3 mode=production);
#   (b) sessions: spark<->pi and pi<->mac established (the M6-B signed and
#       encrypted transport, admitted by the roster);
#   (c) the signed message: spark sends it to mac with the route "mac via pi",
#       pi relays it, mac delivers it with the origin signature verified and
#       ttl lowered by exactly one hop, text intact;
#   (d) revoked: nobody establishes a session with old and old establishes
#       none, and at least one member refused it (handshake or beacon);
#   (e) TEST refused in production: tst prints "join refused (TEST_KEY)".
# Not proven (stated, not hidden): QEMU is not hardware; one host, a loopback
# UDP hub as the LAN; the machine secret key sits in the clear in the
# bundle (qualification only; production seals it under M5); the roster is
# not written to the NVMe Store by this image (the Store round trip is the
# hosted test native/net/tests/ipj_test.c); no aienos-run host forward here.
# Final line: AIENOS_CK_INTERPLANE: PASS|FAIL|NOT_RUN. Exit 0 PASS, 1 FAIL,
# 2 missing tools, 3 NOT_RUN.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

code_fd="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
vars_fd="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 not installed"; echo "AIENOS_CK_INTERPLANE: NOT_RUN"; exit 2; }
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "AAVMF firmware not found"; echo "AIENOS_CK_INTERPLANE: NOT_RUN"; exit 2; }

cross=""
if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
out="${repo_root}/target/native-kernel-interplane"
make -s -C native/kernel CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" CK_INTERPLANE=1 full >/dev/null
make -s -C native/kernel OUT="${out}" gpt-image lan-hub >/dev/null
make -s -C native/net out/ipj_tool >/dev/null
efi="${out}/full/BOOTAA64.EFI"
tool="${repo_root}/native/net/out/ipj_tool"
run_seconds="${AIENOS_IPJ_RUN_SECONDS:-240}"

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take "qemu_ck_interplane_test" "${AIENOS_GATE_MINUTES:-60}"; then
    echo "NOT_RUN  ${gh_why}"
    echo "AIENOS_CK_INTERPLANE: NOT_RUN"
    exit 3
fi
top="$(mktemp -d)"
pids=()
cleanup() {
    for p in "${pids[@]}"; do kill "${p}" 2>/dev/null || true; done
    [[ -z "${hub_pid:-}" ]] || kill "${hub_pid}" 2>/dev/null || true
    rm -rf "${top}"
    gh_lock_release
}
trap cleanup EXIT

mkdir -p "${top}/fixture"
"${tool}" fixture "${top}/fixture" "${run_seconds}" >/dev/null
names=(spark pi mac old tst)
# Per-run hub port and one port per instance, so two runs never share a LAN.
base=$(( 40000 + $(od -An -N2 -tu2 /dev/urandom | tr -d ' ') % 20000 ))
hub_port=${base}
vm_ports=()
for i in "${!names[@]}"; do vm_ports+=("$(( base + 1 + i ))"); done
"${out}/host/ck_lan_hub" "${hub_port}" "${top}/hub.log" "$(( ${AIENOS_QEMU_TIMEOUT:-900} + 30 ))" "${vm_ports[@]}" &
pids+=("$!")
for _ in $(seq 1 50); do grep -q "^ready port=" "${top}/hub.log" 2>/dev/null && break; sleep 0.1; done
grep -q "^ready port=${hub_port}$" "${top}/hub.log" 2>/dev/null || { echo "NOT_RUN  LAN hub could not bind 127.0.0.1:${hub_port}"; echo "AIENOS_CK_INTERPLANE: NOT_RUN"; exit 3; }
hub_pid="${pids[0]}"
pids=()
echo "PASS  fixture written (fresh owner + 5 machine keys), LAN hub on 127.0.0.1:${hub_port}"

for i in "${!names[@]}"; do
    n="${names[$i]}"
    w="${top}/${n}"
    mkdir -p "${w}/esp/EFI/BOOT" "${w}/esp/EFI/AIENOS"
    touch "${w}/esp/EFI/AIENOS/BOOTREPORT.TXT"
    cp "${efi}" "${w}/esp/EFI/BOOT/BOOTAA64.EFI"
    cp "${top}/fixture/${n}.bin" "${w}/esp/EFI/AIENOS/JOIN.BIN"
    cp "${vars_fd}" "${w}/vars.fd"
    "${out}/host/ck_gpt_image" create "${w}/nvme.img" 512 64 aienos-middle >/dev/null
    # Issue #61: single-threaded TCG (see qemu_boot_test.sh).
    timeout "${AIENOS_QEMU_TIMEOUT:-900}" qemu-system-aarch64 \
        -M virt,virtualization=on,gic-version=3,iommu=smmuv3 -accel tcg,thread=single -cpu max -smp 2 -m 2048 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${w}/vars.fd" \
        -drive if=none,id=esp,format=raw,file=fat:rw:"${w}/esp" \
        -device virtio-blk-pci,drive=esp \
        -drive if=none,id=nvme0,format=raw,file="${w}/nvme.img" \
        -device nvme,drive=nvme0,serial="aienos-ipj-${n}" \
        -netdev socket,id=n0,udp=127.0.0.1:${hub_port},localaddr=127.0.0.1:${vm_ports[$i]} \
        -device "virtio-net-pci,netdev=n0,mac=52:54:00:47:40:$(printf '%02x' $((16 + i))),disable-legacy=on,iommu_platform=on" \
        -device ramfb -display none \
        -serial file:"${w}/serial.log" -no-reboot >"${w}/qemu.out" 2>&1 &
    pids+=("$!")
done
fail=0
for i in "${!pids[@]}"; do
    set +e; wait "${pids[$i]}"; st=$?; set -e
    n="${names[$i]}"
    tr -d '\r' <"${top}/${n}/serial.log" >"${top}/${n}/serial.txt" 2>/dev/null || : >"${top}/${n}/serial.txt"
    [[ -z "${AIENOS_LOG_DIR:-}" ]] || cp "${top}/${n}/serial.txt" "${AIENOS_LOG_DIR}/qemu_ck_interplane_${n}.log"
    echo "== ${n}: qemu exit ${st}"
done
pids=()
kill "${hub_pid}" 2>/dev/null || true; wait "${hub_pid}" 2>/dev/null || true
sed "s/^/       hub: /" "${top}/hub.log" | tail -7

check() { # check <machine> <description> <regex>
    if grep -Eq -- "$3" "${top}/$1/serial.txt"; then echo "PASS  $1: $2"; else echo "FAIL  $1: $2 (no line matching: $3)"; fail=1; fi
}
absent() { # absent <machine> <description> <regex>
    if grep -Eq -- "$3" "${top}/$1/serial.txt"; then echo "FAIL  $1: $2 (found: $(grep -Em1 -- "$3" "${top}/$1/serial.txt"))"; fail=1; else echo "PASS  $1: $2"; fi
}
echo "-- (a) join"
for n in spark pi mac; do
    check "${n}" "bundle found by the UEFI stub" "^interplane: found a well-formed join bundle"
    check "${n}" "joined, production mode, generation 5, 3 members" \
        "^interplane: joined as ${n} platform=[a-z-]+ id=[0-9a-f]{8} roster_gen=5 members=3 mode=production ip=10\.47\.40\.[0-9]+ port=47040$"
done
echo "-- (b) sessions"
check spark "session with pi" "^interplane: established with pi$"
check pi "session with spark" "^interplane: established with spark$"
check pi "session with mac" "^interplane: established with mac$"
check mac "session with pi" "^interplane: established with pi$"
echo "-- (c) signed message spark -> pi -> mac"
check spark "route mac via pi configured" "^interplane: route to mac via pi$"
check spark "signed message sent" "^interplane: sent signed message to mac seq=1 len=[0-9]+$"
check pi "relayed (not delivered to the pi)" "^interplane: relayed message from spark$"
absent pi "the pi did not deliver it as its own" "^interplane: inbox from="
check mac "delivered with the origin signature verified, one hop, text intact" \
    "^interplane: inbox from=spark seq=1 ttl=3 kind=1 origin_sig=verified text=\"hello mac, this is the spark, relayed by the pi\"$"
echo "-- (d) revoked machine refused"
check old "old boots with its stale generation-4 roster" "^interplane: joined as old .* roster_gen=4 "
absent old "old holds no session" "^interplane: established with "
for n in spark pi mac; do absent "${n}" "no session with old" "^interplane: established with old$"; done
refused=0
for n in spark pi mac; do
    if grep -Eq "^interplane: handshake refused|^interplane: summary .* beacon_refused=[1-9]" "${top}/${n}/serial.txt"; then refused=1; fi
done
if [[ "${refused}" == 1 ]]; then echo "PASS  a member refused old (handshake or beacon)"; else echo "FAIL  no member recorded a refusal of old"; fail=1; fi
echo "-- (e) TEST refused in production"
check tst "join refused for a roster with a TEST entry" "^interplane: join refused \(TEST_KEY\) mode=production$"
absent tst "tst holds no session" "^interplane: established with "
for n in spark pi mac old; do grep -E "^interplane: summary" "${top}/${n}/serial.txt" | sed "s/^/       ${n}: /" || true; done
if [[ "${fail}" == 0 ]]; then echo "AIENOS_CK_INTERPLANE: PASS"; exit 0; fi
for n in "${names[@]}"; do echo "--- ${n} (interplane lines)"; grep -E "^interplane:" "${top}/${n}/serial.txt" | head -30 || true; done
echo "AIENOS_CK_INTERPLANE: FAIL"
exit 1
