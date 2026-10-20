#!/usr/bin/env bash
# HDA gate for the AIENOS C kernel (L207): the Intel High Definition Audio
# driver (native/kernel/dev/hda.c, fenced by dev/hda_fence.c) against QEMU's
# intel-hda controller and hda-output codec. QEMU is not hardware: a PASS here
# qualifies nothing on the DGX Spark (its NVDA2014 HDMI audio is not exercised).
#
# Boots the TONE image (make full CK_HDA_TONE=1, TEST-ONLY) in QEMU AArch64
# with UEFI, an NVMe disk (normal full boot), qemu-xhci + usb-kbd (so the
# earlier stages behave as in the keyboard gate), "-device intel-hda" with
# "-device hda-output" and "-audiodev wav": whatever the kernel's tone stream
# delivers to the emulated codec is written to a WAV file, which
# native/kernel/tools/ck_wav_check.c then measures from OUTSIDE the guest.
#
#  boot hda-smmu    iommu=smmuv3:
#   row H1  "hda: pci SSSS:BB:DD.F ..." (controller found on the ECAM walk)
#   row H2  "hda_pci: command=0x.... bus_master=off" before "dma_gate: hda "
#   row H3  grant confined ("smmu_dma_window: hda only, translation active",
#           "dma_gate: hda granted (Confined)"), never an unconfined grant
#   row H4  controller up ("hda: controller version 1.0 ...", codecs=0x0001)
#   row H5  codec enumerated ("hda: codec 0 vendor=0x........ ... widgets=N")
#           with at least one pin and one output converter listed
#   row H6  an output path was chosen ("hda: output codec=0 pin=N dac=M")
#   row H7  the stream ran ("hda: tone start freq=1000 rate=48000", "hda: tone
#           played bytes=N" with N >= 1.5 s of 48 kHz stereo 16-bit, "hda: tone done")
#   row H8  the audio QEMU captured is a clean 1000 Hz tone, peak 6000 +-10%,
#           at least 1000 ms, both channels equal (ck_wav_check)
#   row H9  release order: shutdown (stream stopped, controller in reset) <
#           bus master revoked < COMMAND read back off < stream returned to
#           abort < "devices: hda=fenced (rc=0)"
#   row H10 no "report_kind: (panic|fault)", "report_kind: final", QEMU exit 0,
#           every M1 check (scripts/lib_ck_m1_checks.sh), image is this commit
#  boot hda-nosmmu  no SMMU:
#   row H11 "dma_gate: hda denied (NoSmmu), bus master stays off", "hda:
#           unavailable (SMMU DMA isolation not active)", COMMAND read back
#           bus_master=off, no grant, no controller bring-up, "devices:
#           hda=denied (rc=-803)"; rows H1, H2, H10 as above
#
# --mutation (self-test; TEST-ONLY images, one boot each): the named row must
# FAIL while the boot still reaches the final report with row H1 PASS:
#   no-revoke      bus master left on after the phase (smmu)   -> H9
#   grant-no-smmu  DMA granted without an SMMU (nosmmu)        -> H11
#   wrong-tone     1250 Hz played while the log says 1000 Hz   -> H8
# It also checks the build refuses a mutation together with CK_HARDWARE_STAGING=1
# and CK_QEMU_UNSAFE_DMA=1, and that the default image has no tone and no banner.
#
# Like the other gates: NOT_RUN (exit 3) while the machine quiet flag exists or
# the QEMU gate lock is held. Exit 0 PASS, 1 FAIL, 2 missing tools, 3 NOT_RUN.
# Final line: "AIENOS_CK_HDA: PASS|FAIL|NOT_RUN (...)" ("AIENOS_CK_HDA_MUTATION" for --mutation).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

mutation_run=0
case "${1:-}" in
    "") ;;
    --mutation) mutation_run=1 ;;
    *) echo "usage: $0 [--mutation]"; exit 2 ;;
esac
final_tag="AIENOS_CK_HDA"
[[ "${mutation_run}" == 0 ]] || final_tag="AIENOS_CK_HDA_MUTATION"

code_fd="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
vars_fd="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 not installed"; echo "${final_tag}: NOT_RUN (qemu-system-aarch64 missing)"; exit 2; }
[[ -r "${code_fd}" && -r "${vars_fd}" ]] || { echo "AAVMF firmware not found"; echo "${final_tag}: NOT_RUN (AAVMF missing)"; exit 2; }
qemu-system-aarch64 -audiodev help 2>&1 | grep -qx wav || { echo "QEMU has no wav audio backend"; echo "${final_tag}: NOT_RUN (no wav audiodev)"; exit 2; }

cross=""
if [ "$(uname -m)" != "aarch64" ]; then cross="aarch64-linux-gnu-"; fi
commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
out="${repo_root}/target/native-kernel"
mk() { make -s -C native/kernel CROSS="${cross}" OUT="${out}" AIENOS_COMMIT="${commit}" "$@"; }
mk full CK_HDA_TONE=1 >/dev/null
tone_efi="${out}/full-hda-tone/BOOTAA64.EFI"
mk full >/dev/null
default_efi="${out}/full/BOOTAA64.EFI"
mutations=(no-revoke grant-no-smmu wrong-tone)
if [[ "${mutation_run}" == 1 ]]; then
    for m in "${mutations[@]}"; do mk full CK_HDA_TONE=1 CK_TEST_HDA_MUTATION="${m}" >/dev/null; done
fi

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib_gate_hold.sh"
if ! gh_quiet_check; then
    echo "NOT_RUN  ${gh_why}"
    echo "${final_tag}: NOT_RUN (quiet flag held)"
    exit 3
fi
if ! gh_lock_take qemu_ck_hda_test "${AIENOS_GATE_MINUTES:-60}"; then
    echo "NOT_RUN  ${gh_why}"
    echo "${final_tag}: NOT_RUN (QEMU gate lock held)"
    exit 3
fi
top="$(mktemp -d)"
qemu_pid=""
cleanup() {
    [[ -z "${qemu_pid}" ]] || kill "${qemu_pid}" 2>/dev/null || true
    rm -rf "${top}"
    gh_lock_release
}
trap cleanup EXIT

# The host tool that measures the captured audio (no outside libraries).
wavtool="${top}/ck_wav_check"
cc -O2 -Wall -Wextra -Werror -o "${wavtool}" native/kernel/tools/ck_wav_check.c

# shellcheck source=scripts/lib_ck_m1_checks.sh
source "${repo_root}/scripts/lib_ck_m1_checks.sh"
image="${top}/nvme.img"
truncate -s 67108864 "${image}"
m1_fail=0
boot_timeout="${AIENOS_QEMU_TIMEOUT:-240}"

# boot <name> <efi> smmu|nosmmu: one QEMU boot to the kernel's own end.
boot() {
    work="${top}/$1"
    mkdir -p "${work}/esp/EFI/BOOT" "${work}/esp/EFI/AIENOS"
    touch "${work}/esp/EFI/AIENOS/BOOTREPORT.TXT"
    cp "$2" "${work}/esp/EFI/BOOT/BOOTAA64.EFI"
    cp "${vars_fd}" "${work}/vars.fd"
    local machine="virt,virtualization=on,gic-version=3"
    [[ "$3" != smmu ]] || machine+=",iommu=smmuv3"
    set +e
    timeout "${boot_timeout}" qemu-system-aarch64 \
        -M "${machine}" -accel tcg,thread=single -cpu max -smp 4 -m 2048 \
        -drive if=pflash,format=raw,readonly=on,file="${code_fd}" \
        -drive if=pflash,format=raw,file="${work}/vars.fd" \
        -drive if=none,id=esp,format=raw,file=fat:rw:"${work}/esp" \
        -device virtio-blk-pci,drive=esp \
        -drive if=none,id=nvme0,format=raw,file="${image}" \
        -device nvme,drive=nvme0,serial=aienos-hda-test \
        -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
        -audiodev wav,id=snd0,path="${work}/out.wav",frequency=48000,channels=2,format=s16 \
        -device intel-hda,id=hda0 -device hda-output,bus=hda0.0,audiodev=snd0 \
        -device ramfb -display none -nic none \
        -serial file:"${work}/serial.log" -no-reboot 2>"${work}/qemu.err" </dev/null
    qemu_status=$?
    set -e
    tr -d '\r' <"${work}/serial.log" >"${work}/serial.txt"
    [[ -z "${AIENOS_LOG_DIR:-}" ]] || cp "${work}/serial.txt" "${AIENOS_LOG_DIR}/qemu_ck_hda_$1.log"
    echo "== boot $1 ($3): qemu exit ${qemu_status}"
}

all_rows=(H1 H2 H3 H4 H5 H6 H7 H8 H9 H10 H11)
declare -A rowfail rowmodes
reset_rows() { local r; for r in "${all_rows[@]}"; do rowfail[$r]=0; rowmodes[$r]=""; done; }
reset_rows
mode=""
note_mode() { [[ " ${rowmodes[$1]} " == *" ${mode} "* ]] || rowmodes[$1]="${rowmodes[$1]:+${rowmodes[$1]} }${mode}"; }
rpass() { note_mode "$1"; echo "ROW $1 PASS  ${mode}: $2"; }
rfail() { note_mode "$1"; rowfail[$1]=1; echo "ROW $1 FAIL  ${mode}: $2"; }
rcheck() { if grep -qE -- "$3" "${work}/serial.txt"; then rpass "$1" "$2"; else rfail "$1" "$2"; fi; }
rabsent() { if grep -qE -- "$3" "${work}/serial.txt"; then rfail "$1" "$2"; else rpass "$1" "$2"; fi; }
line_of() { { grep -nE -m1 -- "$1" "${work}/serial.txt" || true; } | cut -d: -f1; }
before() { # row, description, earlier ERE, later ERE
    local a b
    a="$(line_of "$3")"; b="$(line_of "$4")"
    if [[ -n "${a}" && -n "${b}" && "${a}" -lt "${b}" ]]; then rpass "$1" "$2 (lines ${a} < ${b})"
    else rfail "$1" "$2 (lines ${a:-none} ${b:-none})"; fi
}

evaluate() { # smmu|nosmmu
    mode="$1"
    rcheck H1 "HD Audio controller found on the ECAM walk" '^hda: pci 0000:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7] vendor=0x[0-9a-f]{4} device=0x[0-9a-f]{4} mmio 0x[0-9a-f]+$'
    rcheck H2 "bus master off before the DMA gate" '^hda_pci: command=0x[0-9a-f]{4} bus_master=off$'
    before H2 "COMMAND read back before the HD Audio DMA gate" '^hda_pci: command=' '^dma_gate: hda '
    if [[ "${mode}" == nosmmu ]]; then
        rcheck H11 "HD Audio DMA denied without an SMMU" '^dma_gate: hda denied \(NoSmmu\), bus master stays off$'
        rcheck H11 "fail-closed message" '^hda: unavailable \(SMMU DMA isolation not active\)$'
        rcheck H11 "COMMAND read back after the deny: bus master off" '^hda_pci: after deny command=0x[0-9a-f]{4} bus_master=off$'
        rabsent H11 "never granted DMA" 'dma_gate: hda granted'
        rabsent H11 "no controller bring-up without a grant" '^hda: controller version'
        rabsent H11 "no SMMU window claimed" '^smmu_dma_window: hda'
        rcheck H11 "devices stage reports the deny" '^devices: hda=denied \(rc=-803\)$'
        return
    fi
    rcheck H3 "HD Audio SMMU window" '^smmu_dma_window: hda only, translation active iova=0x[0-9a-f]+ len=0x[0-9a-f]+ stream=0x[0-9a-f]+$'
    rcheck H3 "granted only as confined" '^dma_gate: hda granted \(Confined\) smmu=0x[0-9a-f]+ stream=0x[0-9a-f]+, bus master on$'
    rabsent H3 "no unconfined grant" 'dma_gate: hda granted \((UnsafeBypass|TestMutation)'
    rcheck H4 "controller up" '^hda: controller version 1\.0 gcap=0x[0-9a-f]{4} oss=[1-9][0-9]* iss=[0-9]+ bss=[0-9]+ addr64=(yes|no) codecs=0x0001 corb=[0-9]+ rirb=[0-9]+$'
    rcheck H5 "codec enumerated" '^hda: codec 0 vendor=0x[0-9a-f]{8} revision=0x[0-9a-f]{8} subsystem=0x[0-9a-f]{8} afg=[1-9][0-9]* widgets=[1-9][0-9]*$'
    rcheck H5 "a pin widget listed" '^hda: widget 0\.[0-9]+ pin caps=0x[0-9a-f]{8} pincap=0x[0-9a-f]{8} config=0x[0-9a-f]{8} conns=[0-9]+$'
    rcheck H5 "an output converter listed" '^hda: widget 0\.[0-9]+ out caps=0x[0-9a-f]{8} rates=0x[0-9a-f]{4} formats=0x[0-9a-f]{8}$'
    rcheck H6 "output path chosen" '^hda: output codec=0 pin=[0-9]+ dac=[0-9]+ path=[0-9]+ widgets$'
    rcheck H7 "tone started" '^hda: tone start freq=1000 rate=48000 channels=2 bits=16 amp=6000 ms=1500$'
    played="$(grep -E -m1 '^hda: tone played bytes=' "${work}/serial.txt" | sed -E 's/.*bytes=([0-9]+).*/\1/' || true)"
    if [[ -n "${played}" && "${played}" -ge 288000 ]]; then rpass H7 "stream consumed ${played} bytes (>= 288000 = 1.5 s)"; else rfail H7 "stream consumed ${played:-none} bytes (< 288000)"; fi
    rcheck H7 "tone stopped" '^hda: tone done$'
    rabsent H7 "tone not cut short" '^hda: tone short'
    set +e
    wav_out="$("${wavtool}" "${work}/out.wav" 1000 6000 1000 2>&1)"
    wav_rc=$?
    set -e
    echo "${wav_out}" | sed 's/^/   /'
    if [[ "${wav_rc}" == 0 ]]; then rpass H8 "captured audio is the tone"; else rfail H8 "captured audio is not the expected tone"; fi
    rcheck H9 "shutdown: stream stopped, controller in reset" '^hda: shutdown stream stopped, controller in reset: yes \(rc=0\)$'
    rcheck H9 "bus master revoked" '^dma_gate: hda bus master revoked$'
    rcheck H9 "COMMAND read back after the phase: bus master off" '^hda_pci: after phase command=0x[0-9a-f]{4} bus_master=off$'
    rcheck H9 "stream returned to abort" '^smmu: hda stream 0x[0-9a-f]+ returned to abort \(rc=0\)$'
    rabsent H9 "no revoke failure" '^dma_gate: hda bus master revoke FAILED'
    before H9 "order: shutdown < revoke" '^hda: shutdown ' '^dma_gate: hda bus master revoked$'
    before H9 "order: revoke < read back" '^dma_gate: hda bus master revoked$' '^hda_pci: after phase command='
    before H9 "order: read back < stream abort" '^hda_pci: after phase command=' '^smmu: hda stream .* returned to abort'
    before H9 "order: stream abort < end of the HD Audio phase" '^smmu: hda stream .* returned to abort' '^devices: hda=fenced \(rc=0\)$'
}

final_rows() { # row bookkeeping for one boot's final report
    rcheck H10 "final report reached" '^report_kind: final$'
    rabsent H10 "no panic or fault" '^report_kind: (panic|fault)'
    if [[ "${qemu_status}" == 0 ]]; then rpass H10 "QEMU exit 0"; else rfail H10 "QEMU exit ${qemu_status}"; fi
    failed=0
    ck_m1_checks
    [[ "${failed}" == 0 ]] || m1_fail=1
    if [[ "${failed}" == 0 ]]; then rpass H10 "M1 checks"; else rfail H10 "an M1 check failed"; fi
    rcheck H10 "image is this commit" "aienos_commit: ${commit}"
}

if [[ "${mutation_run}" == 0 ]]; then
    modes=(smmu nosmmu)
    gate_fail=0
    boot hda-smmu "${tone_efi}" smmu
    evaluate smmu
    final_rows
    rabsent H10 "no TEST-ONLY mutation banner" 'TEST-ONLY HD AUDIO MUTATION'
    rabsent H10 "no unsafe DMA bypass" 'UNSAFE DMA BYPASS'
    boot hda-nosmmu "${tone_efi}" nosmmu
    evaluate nosmmu
    final_rows
    # The default image: controller brought up and listed, no tone.
    boot hda-default "${default_efi}" smmu
    mode=default
    rcheck H4 "default image brings the controller up" '^hda: controller version 1\.0 '
    rabsent H7 "default image plays no tone" '^hda: tone '
    rcheck H9 "default image releases the controller" '^smmu: hda stream 0x[0-9a-f]+ returned to abort \(rc=0\)$'
    if grep -aqF "hda: tone start" "${default_efi}"; then rfail H7 "default image carries the tone code"; else rpass H7 "default image carries no tone code"; fi
    echo "== per-row results (QEMU only; hardware NOT_RUN)"
    for r in "${all_rows[@]}"; do
        if [[ -z "${rowmodes[$r]}" ]]; then echo "HDA_ROW ${r}: NOT_RUN"; gate_fail=1
        elif [[ "${rowfail[$r]}" == 0 ]]; then echo "HDA_ROW ${r}: PASS (${rowmodes[$r]})"
        else echo "HDA_ROW ${r}: FAIL (${rowmodes[$r]})"; gate_fail=1; fi
    done
    if [[ "${gate_fail}" == 0 ]]; then
        echo "${final_tag}: PASS (rows H1-H11; boots hda-smmu hda-nosmmu hda-default; QEMU only, hardware NOT_RUN)"
        exit 0
    fi
    echo "${final_tag}: FAIL (QEMU only)"
    exit 1
fi

# ---- --mutation: every mutation must make its row FAIL ----
mut_fail=0
mut_ok() { echo "MUTATION PASS  $1"; }
mut_bad() { echo "MUTATION FAIL  $1"; mut_fail=1; }
if grep -aqF "TEST-ONLY HD AUDIO MUTATION" "${tone_efi}"; then mut_bad "tone image carries the mutation banner"; else mut_ok "tone image carries no mutation banner"; fi
for combo in "CK_HARDWARE_STAGING=1" "CK_QEMU_UNSAFE_DMA=1"; do
    for flag in CK_HDA_TONE=1 "CK_HDA_TONE=1 CK_TEST_HDA_MUTATION=no-revoke"; do
        set +e
        # shellcheck disable=SC2086
        msg="$(make -s -n -C native/kernel OUT="${top}/refuse" full ${flag} "${combo}" 2>&1)"
        st=$?
        set -e
        if [[ "${st}" != 0 ]] && grep -qE "CK_HDA_TONE.*cannot be combined with ${combo%%=*}" <<<"${msg}"; then
            mut_ok "build refuses ${flag} with ${combo}"
        else
            mut_bad "build did not refuse ${flag} with ${combo} (status ${st}): $(head -c 200 <<<"${msg}")"
        fi
    done
done
declare -A mut_mode=([no-revoke]=smmu [grant-no-smmu]=nosmmu [wrong-tone]=smmu)
declare -A mut_row=([no-revoke]=H9 [grant-no-smmu]=H11 [wrong-tone]=H8)
for m in "${mutations[@]}"; do
    efi="${out}/full-test-hda-${m}/BOOTAA64.EFI"
    boot "mut-${m}" "${efi}" "${mut_mode[$m]}"
    reset_rows
    evaluate "${mut_mode[$m]}"
    r="${mut_row[$m]}"
    if ! grep -qF "WARNING: TEST-ONLY HD AUDIO MUTATION BUILD (CK_TEST_HDA_MUTATION=" "${work}/serial.txt"; then
        mut_bad "${m}: image did not announce the mutation"
    elif ! grep -q "report_kind: final" "${work}/serial.txt" || [[ "${rowfail[H1]}" != 0 ]]; then
        mut_bad "${m}: boot did not reach the final report with the controller found (a dead boot proves nothing)"
    elif [[ "${rowfail[$r]}" == 1 ]]; then
        mut_ok "${m} (${mut_mode[$m]}): row ${r} FAILs as it must"
    else
        mut_bad "${m} (${mut_mode[$m]}): row ${r} still PASSES: the check does not catch this mutation"
    fi
done
if [[ "${mut_fail}" != 0 || -n "${AIENOS_QEMU_VERBOSE:-}" ]]; then
    for s in "${top}"/*/serial.txt; do
        echo "---- serial console $(basename "$(dirname "${s}")") (device lines) ----"
        grep -E '^(WARNING|hda|dma_gate|smmu|devices:|report_kind:)' "${s}" | head -60 || true
    done
fi
if [[ "${mut_fail}" == 0 ]]; then echo "${final_tag}: PASS (no-revoke->H9 grant-no-smmu->H11 wrong-tone->H8)"; exit 0; fi
echo "${final_tag}: FAIL"
exit 1
