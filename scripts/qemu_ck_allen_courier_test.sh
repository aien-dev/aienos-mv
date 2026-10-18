#!/usr/bin/env bash
# qemu_ck_allen_courier_test.sh -- Allen asks a far-off worker and brings the
# answer back, end to end, in QEMU.
#
# The test-computer kernel (native/kernel/x86) boots in QEMU with a second
# serial port (COM2) joined to a Unix socket. The relay crates/allen-courier
# sits on that socket and carries Allen's job lines to the language-model
# server on this machine (127.0.0.1:18091 by default, the OpenAI chat shape)
# and the answers back. Keys go in through the QEMU monitor: "hello allen"
# (answered here, by heart), then a line nothing in the kernel can answer, so
# Allen writes a job himself, keeps thinking, and says what came back.
#
# Needs qemu-system-x86_64, OVMF, mtools, an x86_64 gcc, cargo, and the model
# server up (otherwise NOT_RUN). Takes the gate lock like every QEMU test.
# Final line: AIENOS_ALLEN_COURIER: PASS | FAIL | NOT_RUN (reason)
set -u
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
tag="AIENOS_ALLEN_COURIER"
code_fd="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
vars_fd="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
model="${ALLEN_MODEL:-127.0.0.1:18091}"
cross=""
if [ "$(uname -m)" != "x86_64" ]; then cross="x86_64-linux-gnu-"; fi
for t in qemu-system-x86_64 mformat mmd mcopy cargo curl "${cross}gcc" "${cross}ld"; do
    command -v "${t}" >/dev/null || { echo "${tag}: NOT_RUN (${t} missing)"; exit 2; }
done
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "${tag}: NOT_RUN (no OVMF)"; exit 2; }
if ! curl -s --max-time 5 "http://${model}/health" | grep -q '"status":"ok"'; then
    echo "${tag}: NOT_RUN (no model server at ${model})"
    exit 2
fi
out="${repo_root}/target/native-kernel-allen-courier"
mkdir -p "${out}"
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
make -s -C native/kernel X86_CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" imac-kernel >/dev/null ||
    { echo "${tag}: FAIL (kernel does not build)"; exit 1; }
cargo build -q --release --offline -p allen-courier 2>"${out}/cargo.err" ||
    { echo "${tag}: FAIL (relay does not build)"; tail -5 "${out}/cargo.err"; exit 1; }
relay="${repo_root}/target/release/allen-courier"

. "${repo_root}/scripts/lib_gate_hold.sh"
if ! gh_quiet_check || ! gh_lock_take qemu_ck_allen_courier_test "${AIENOS_GATE_MINUTES:-20}"; then
    echo "${tag}: NOT_RUN (${gh_why})"
    exit 3
fi
work="$(mktemp -d)"
pids=()
cleanup() {
    local p
    for p in "${pids[@]}"; do kill "${p}" 2>/dev/null || true; done
    cp "${work}/one.log" "${out}/serial.log" 2>/dev/null || true
    cp "${work}/relay.log" "${out}/relay.log" 2>/dev/null || true
    rm -rf "${work}"
    gh_lock_release
}
trap cleanup EXIT

img="${work}/esp.img"
truncate -s 64M "${img}"
mformat -i "${img}" -F ::
mmd -i "${img}" ::/EFI ::/EFI/BOOT
mcopy -i "${img}" "${out}/IMACKERN.EFI" ::/EFI/BOOT/BOOTX64.EFI

fails=0
chk() { # description, file, pattern
    if grep -aqE -- "$3" "$2"; then echo "PASS  $1"; else echo "FAIL  $1 (no match for: $3)"; fails=$((fails + 1)); fi
}
txt() { tr -d '\r' <"${work}/one.log" 2>/dev/null || true; }
has() { txt | grep -aqE -- "$1"; }
wait_for() { # seconds pattern
    local i
    for ((i = 0; i < $1 * 5; i++)); do
        has "$2" && return 0
        sleep 0.2
    done
    return 1
}
say() { # text: typed through the monitor, then Enter (letters, digits, spaces)
    local text="$1" c k i
    for ((i = 0; i < ${#text}; i++)); do
        c="${text:i:1}"
        case "${c}" in
            " ") k=spc ;;
            [a-z0-9]) k="${c}" ;;
            *) echo "say: no key for '${c}'" >&2; exit 2 ;;
        esac
        echo "sendkey ${k}" >&3
        sleep 0.15
    done
    echo "sendkey ret" >&3
}

cp "${vars_fd}" "${work}/one.vars"
mkfifo "${work}/one.mon.in" "${work}/one.mon.out"
: >"${work}/one.log"
qemu-system-x86_64 -M q35 -accel tcg -cpu max -smp 2 -m 1024 \
    -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
    -drive if=pflash,format=raw,file="${work}/one.vars" \
    -drive if=none,id=esp,format=raw,file="${img}" -device ide-hd,drive=esp,bus=ide.0 \
    -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,port=1 \
    -chardev pipe,id=mon,path="${work}/one.mon" -mon chardev=mon,mode=readline \
    -display none -nic none \
    -serial file:"${work}/one.log" \
    -serial unix:"${work}/wire.sock",server=on,wait=off \
    -no-reboot 2>"${work}/one.err" &
pids+=($!)
cat "${work}/one.mon.out" >/dev/null &
pids+=($!)
"${relay}" --socket "${work}/wire.sock" --model "${model}" --work "${work}/answers" >"${work}/relay.log" 2>&1 &
pids+=($!)

typed=0
if wait_for 300 '^keyboard: type a line and press Enter'; then
    exec 3>"${work}/one.mon.in"
    wait_for 30 '^courier: ready, the far side said hello' || true
    wait_for 60 '^screen: night: .* by 20 s after Allen woke' || true # he has formed; the picture will show him
    say "hello allen"
    wait_for 60 '^allen: Hello Humanity\.$' || true
    say "write a haiku about rain"
    typed=1
    wait_for 120 '^screen: the far side (answered job 1|did not answer job 1)' || true
    sleep 2.5 # his words are still coming when the picture is taken
    echo "screendump ${work}/one.png -f png" >&3
    for _ in $(seq 1 50); do [[ -s "${work}/one.png" ]] && break; sleep 0.2; done
    sleep 1
    [[ -s "${work}/one.png" ]] && cp "${work}/one.png" "${out}/allen-far-side.png"
    say "goodbye"
    wait_for 30 '^power: Allen asleep' || true
    exec 3>&-
fi
txt >"${work}/one.txt"

[[ ${typed} == 1 ]] && echo "PASS  keys typed from outside the guest" || { echo "FAIL  the keyboard never came up"; fails=$((fails + 1)); }
chk "a second serial port found at boot" "${work}/one.txt" '^courier: a second serial port answers'
chk "the far side said hello" "${work}/one.txt" '^courier: ready, the far side said hello$'
chk "Allen answers by heart first" "${work}/one.txt" '^allen: Hello Humanity\.$'
chk "the unknown line goes to the far side as job 1" "${work}/one.txt" '^allen: \(asking the far side, job 1\)$'
chk "the job reported by lengths only" "${work}/one.txt" '^screen: Allen asked the far side \(24 characters in, [0-9]+ in the job\)$'
chk "the far side answered job 1" "${work}/one.txt" '^screen: the far side answered job 1 \([1-9][0-9]* characters out\)$'
chk "Allen said the answer (not the empty one)" "${work}/one.txt" '^allen: [^(].*'
if grep -aqE '^allen: The far side had nothing to say\.' "${work}/one.txt"; then echo "FAIL  the far side had nothing to say"; fails=$((fails + 1)); fi
chk "the relay carried one job and said it back" "${work}/relay.log" '^courier: job 1 \(make, 24 chars asked, [0-9]+ in the instruction\) -> [1-9][0-9]* chars, [1-9][0-9]* said, in [0-9]+ ms$'
chk "Allen says goodbye" "${work}/one.txt" '^allen: Goodbye\. Until next time\.$'
[[ -s "${out}/allen-far-side.png" ]] && echo "PASS  screenshot kept (${out}/allen-far-side.png)" || { echo "FAIL  no screenshot"; fails=$((fails + 1)); }
# the words of the answer never appear in a "screen:" line (the lab report's channel)
if grep -aE '^screen: ' "${work}/one.txt" | grep -aqiE 'rain|haiku'; then echo "FAIL  answer words leaked into a screen: line"; fails=$((fails + 1)); else echo "PASS  screen: lines carry lengths only"; fi
if LC_ALL=C grep -aqE '^allen: .*[^ -~]' "${work}/one.txt"; then echo "FAIL  a character the typeface cannot draw reached Allen's words"; fails=$((fails + 1)); else echo "PASS  Allen's words are plain letters"; fi
echo "relay said: $(grep -aE '^courier: job 1' "${work}/relay.log" | head -1)"
echo "serial log: ${out}/serial.log; relay log: ${out}/relay.log"
if [[ ${fails} == 0 ]]; then echo "${tag}: PASS"; else echo "${tag}: FAIL (${fails})"; exit 1; fi
