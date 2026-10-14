#!/usr/bin/env bash
# ck_pi5_rc_check.sh -- build-side proof of the Pi 5 release candidate (PI5-8 cut R, "make pi5-rc").
#
#   bash scripts/ck_pi5_rc_check.sh
#
# The release candidate is the unlock-card lifecycle image whose artifact loader trusts the owner root PUBLIC key from
# the PI5-8 key ceremony and nothing else. It cannot set CK_HARDWARE_STAGING (svc/store_boot.c refuses it with the Pi
# unlock, and that rule stays), so it takes the staging protections over in five layers (native/kernel/Makefile,
# CK_PI5_RC block; docs/native/PI5-8-QUALIFICATION.md, "PI5-8 RC"). This script proves each layer refuses:
#   1. inputs: every TEST key/anchor, mutation, debug, unsafe-DMA, fw_cfg, staging and extra-flags switch, on the
#      command line, from the environment, or through the top-level "make pi5-rc", is refused before anything builds;
#   2. owner file: missing, TEST-fixture, one-byte-changed and RFC 8032 TEST owner files are refused; ck_owner_gen
#      --anchor refuses the TEST keys and placeholders;
#   3. compile fence: every switch in include/ck_pi5_rc_guard.h stops the preprocessor; every test-class name in the
#      sources is in the guard; a switch smuggled past layer 1 (extra makefile) is stopped at compile time;
#   4. flag scan: a test-class -D smuggled past layer 1 is refused when the makefile is read;
#   5. image check: the RC ELF and EFI pass scripts/ck_pi5_rc_image_check.sh (owner key bytes once and equal to the
#      ceremony file, no TEST key or seed bytes, no TEST Store identity, no fw_cfg); the plain lifecycle image, a copy of
#      the RC image with one owner key byte flipped, and the RC image checked against another owner file all FAIL;
# and then:
#   6. the owner-key verification path (tests/test_pi5_rc_anchor.c, same anchor header and cka_verify as the image):
#      throwaway-, TEST 1- and TEST 3-signed artifacts and a forged owner-fingerprint artifact are REFUSED, a positive
#      control verifies; built with the throwaway key as "owner" (red control) the test FAILS;
#   7. two RC builds in separate output directories give the same BOOTAA64.EFI bytes.
# The owner file is PI5_RC_OWNER_PUBKEYS, default ~/aienos-owner-ceremony/20261014T114310Z/owner-pubkeys.txt (public; the
# repository keeps no owner file, native/kernel/README.md). Without it (CI) the steps that need it SKIP; with
# AIENOS_STRICT=1 a SKIP is a FAIL. Never touches a YubiKey, never signs with the owner key, never boots anything.
# CPU only; builds go to a private temporary OUT. Ends with "PI5_RC_CHECK: PASS" or "PI5_RC_CHECK: FAIL".
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
kdir="${root}/native/kernel"
own="${PI5_RC_OWNER_PUBKEYS:-${HOME}/aienos-owner-ceremony/20261014T114310Z/owner-pubkeys.txt}"
pin="$(sed -n 's/^override PI5_RC_OWNER_SHA256 := //p' "${kdir}/Makefile")"
fix="${kdir}/tests/fixtures/owner/TEST-FIXTURE-owner-pubkeys.txt"
fail=0
skips=0
T="$(mktemp -d)"
trap 'rm -rf "${T}"' EXIT
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }
skip() { echo "SKIP $*"; skips=$((skips + 1)); }

have_owner=0
if [ -f "${own}" ] && [ "$(sha256sum "${own}" | cut -c1-64)" = "${pin}" ]; then
    have_owner=1
    pass "owner file ${own} is the PI5-8 ceremony file (sha256 ${pin})"
elif [ -f "${own}" ]; then
    bad "owner file ${own} is not the ceremony file (sha256 $(sha256sum "${own}" | cut -c1-64), pinned ${pin})"
else
    skip "no ceremony owner file at ${own}: steps that build the real RC are skipped"
fi
# A file that passes the pin is needed to reach the later make checks; without it, layer-1 refusals still run (they
# come first) and the owner-file refusals use the fixture.
okown="${own}"
[ "${have_owner}" = 1 ] || okown="${fix}"

# expect_refused <label> <expected message part> <make args...>
expect_refused() {
    local label="$1" want="$2"
    shift 2
    local out
    if out="$(make -n -C "${kdir}" "$@" 2>&1)"; then
        bad "${label}: make accepted it"
    elif grep -qF -- "${want}" <<<"${out}"; then
        pass "${label}"
    else
        bad "${label}: refused, but not with \"${want}\": $(grep -m1 -E 'Error|\*\*\*' <<<"${out}")"
    fi
}

RC=(CK_PI5_RC=1 CK_PI5_UNLOCK=1 CK_PI5_UNLOCK_CARD=1 CK_PI5_LIFECYCLE=1 CK_INFER_LIB="${T}/no-lib.a" OUT="${T}/n")

echo "== layer 1: inputs =="
for kv in CK_SEED0B_TEST_ANCHOR=1 CK_SERVICE_TEST_KEYS=1 CK_TEST_SLOT_KEY=1 CK_OSC_LAUNCH_TEST=1 CK_OSH_TEST=1 \
    CK_OSH_OP_TEST=1 CK_TEST_XHCI_MUTATION=no-fence CK_TEST_CONSOLE_MUTATION=1 CK_CONSOLE_SESSION=1 \
    CK_TEST_STORE_CRASH=1 CK_TEST_STORE_CRASH_MUTANT=skip-root-flush CK_TEST_CONTINUITY=1 CK_TEST_CONTINUITY_MUTANT=1 \
    CK_TEST_ROLLBACK=bad-magic CK_TEST_STALE_HANDOFF=1 CK_TEST_SMP_SKIP_CPU=1 CK_B7B_PROBE_TEST_SEGMENT0=1 \
    CK_TEST_DISK_XLATE_BYPASS=1 CK_QEMU_UNSAFE_DMA=1 CK_TEST_FWCFG_ARTIFACTS=1 CK_HARDWARE_STAGING=1 \
    CK_MACHINE_ID="${kdir}/tests/fixtures/owner/TEST-FIXTURE-machine-id.txt" CK_EXTRA_CFLAGS=-DCK_STORE_TEST_KEYS=1 \
    CK_RUST_LIBS=x.a CK_SERVICE=1 CK_PI5_STORE=1 CK_PI5_ARTIFACT=1 CK_ARTIFACT_CARD=1 CK_PI5_PRODUCTION=1 \
    CK_PI5_AGENT=1 CK_PI5_FACT=1 CK_PI5_NET=1 CK_FB_HOLD_S=120 KCFLAGS=-DCK_TEST_ROLLBACK=1 \
    STAGE_DMA_FLAGS=-DCK_QEMU_UNSAFE_DMA=1 ART_TRUST_FLAGS=-DCK_SEED0B_TEST_ANCHOR=1 PI5_RC_INPUTS=x \
    PI5_RC_OWNER_SHA256="$(sha256sum "${fix}" | cut -c1-64)" PI5_RC_REFUSED= PI5_RC_GIVEN=; do
    name="${kv%%=*}"
    expect_refused "RC refuses ${name} on the command line" "refused: ${name}" full "${RC[@]}" CK_OWNER_PUBKEYS="${okown}" "${kv}"
    expect_refused "make pi5-rc refuses ${name} on the command line" "refused: ${name}" pi5-rc CK_OWNER_PUBKEYS="${okown}" "${kv}"
done
for kv in CK_TEST_ROLLBACK=bad-magic CK_EXTRA_CFLAGS=-DCK_STORE_TEST_KEYS=1 CK_SEED0B_TEST_ANCHOR=1 CK_QEMU_UNSAFE_DMA=1; do
    name="${kv%%=*}"
    if out="$(env "${kv}" make -n -C "${kdir}" full "${RC[@]}" CK_OWNER_PUBKEYS="${okown}" 2>&1)"; then
        bad "RC accepted ${name} from the environment"
    elif grep -qF "refused: ${name}" <<<"${out}"; then pass "RC refuses ${name} from the environment"
    else bad "RC refused ${name} from the environment with another message"; fi
done
if out="$(env CK_TEST_ROLLBACK=bad-magic make -e -n -C "${kdir}" full "${RC[@]}" CK_OWNER_PUBKEYS="${okown}" 2>&1)"; then
    bad "RC accepted make -e"
elif grep -qF "refused:" <<<"${out}"; then pass "RC refuses make -e (environment overrides)"
else bad "RC refused make -e with another message"; fi
if [ "${have_owner}" = 1 ]; then
    expect_refused "RC alone is refused (needs the lifecycle image)" "CK_PI5_RC is the unlock-card lifecycle image" full \
        CK_PI5_RC=1 CK_OWNER_PUBKEYS="${own}" OUT="${T}/n"
else
    skip "RC-alone refusal (no ceremony file)"
fi
expect_refused "pi5-rc must be the only goal" "pi5-rc must be the only goal" pi5-rc all CK_OWNER_PUBKEYS="${okown}"
expect_refused "pi5-rc refuses a preset CK_PI5_RC" "it sets CK_PI5_RC" pi5-rc CK_PI5_RC=1 CK_OWNER_PUBKEYS="${okown}"
expect_refused "owner files stay refused outside the RC and staging" "only used with CK_HARDWARE_STAGING=1 or" full \
    CK_OWNER_PUBKEYS="${okown}" OUT="${T}/n"
expect_refused "lifecycle image still refuses owner files without the RC" "only used with CK_HARDWARE_STAGING=1 or" full \
    CK_PI5_UNLOCK=1 CK_PI5_UNLOCK_CARD=1 CK_PI5_LIFECYCLE=1 CK_INFER_LIB="${T}/no-lib.a" CK_OWNER_PUBKEYS="${okown}" OUT="${T}/n"

echo "== layer 2: owner file =="
expect_refused "RC without CK_OWNER_PUBKEYS" "needs CK_OWNER_PUBKEYS" full "${RC[@]}"
expect_refused "make pi5-rc without CK_OWNER_PUBKEYS" "needs CK_OWNER_PUBKEYS" pi5-rc
expect_refused "RC with a missing owner file" "file not found" full "${RC[@]}" CK_OWNER_PUBKEYS="${T}/none.txt"
expect_refused "RC with the TEST-fixture owner file" "is not the PI5-8 ceremony owner file" full "${RC[@]}" CK_OWNER_PUBKEYS="${fix}"
printf 'owner_root_ed25519 d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a\n' > "${T}/t1.txt"
expect_refused "RC with the RFC 8032 TEST 1 key as owner" "is not the PI5-8 ceremony owner file" full "${RC[@]}" CK_OWNER_PUBKEYS="${T}/t1.txt"
if [ "${have_owner}" = 1 ]; then
    sed 's/9e76$/9e77/' "${own}" > "${T}/flip.txt"
    expect_refused "RC with the ceremony file changed in one hex digit" "is not the PI5-8 ceremony owner file" full "${RC[@]}" CK_OWNER_PUBKEYS="${T}/flip.txt"
    { cat "${own}"; echo; } > "${T}/extra.txt"
    expect_refused "RC with the ceremony file plus one blank line" "is not the PI5-8 ceremony owner file" full "${RC[@]}" CK_OWNER_PUBKEYS="${T}/extra.txt"
else
    skip "one-digit and one-line changes of the ceremony file (no ceremony file)"
fi
make -s -C "${kdir}" owner-gen OUT="${T}/h" >/dev/null
gen="${T}/h/host/ck_owner_gen"
printf 'owner_root_ed25519 3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c\n' > "${T}/t2.txt"
printf 'owner_root_ed25519 fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025\n' > "${T}/t3.txt"
printf 'owner_root_ed25519 %064d\n' 0 > "${T}/zero.txt"
printf '# no key\n' > "${T}/empty.txt"
printf 'owner_root_ed25519 578ada3c\n' > "${T}/short.txt"
for f in t1 t2 t3 zero empty short; do
    if "${gen}" --anchor "${T}/${f}.txt" "${T}/${f}.h" >/dev/null 2>&1 || [ -e "${T}/${f}.h" ]; then
        bad "ck_owner_gen --anchor accepted ${f}"
    else pass "ck_owner_gen --anchor refuses ${f} and writes no header"; fi
done

echo "== layer 3: compile fence =="
guard="${kdir}/include/ck_pi5_rc_guard.h"
base=(-DCK_PI5_RC=1 -DCK_PI5=1 -DCK_PI5_UNLOCK=1 -DCK_PI5_UNLOCK_CARD=1 -DCK_PI5_LIFECYCLE=1)
if cc -E "${base[@]}" -include "${guard}" -x c /dev/null >/dev/null 2>&1; then pass "guard admits the RC flag set"; else bad "guard refuses the plain RC flag set"; fi
if cc -E -DCK_PI5=1 -DCK_PI5_UNLOCK=1 -DCK_PI5_UNLOCK_CARD=1 -DCK_PI5_LIFECYCLE=1 -include "${guard}" -x c /dev/null >/dev/null 2>&1; then
    bad "guard admits a unit without CK_PI5_RC"
else pass "guard refuses a unit without CK_PI5_RC"; fi
n=0
nbad=0
for m in $(sed -n 's/^#ifdef \([A-Z0-9_]*\)$/\1/p' "${guard}"); do
    n=$((n + 1))
    out="$(cc -E "${base[@]}" "-D${m}=1" -include "${guard}" -x c /dev/null 2>&1 >/dev/null || true)"
    grep -qF "Pi 5 release candidate refuses ${m}" <<<"${out}" || { bad "guard does not stop ${m}"; nbad=$((nbad + 1)); }
done
[ "${nbad}" = 0 ] && pass "guard stops each of its ${n} switches"
missing="$(cd "${root}/native" && grep -rhoE "\b(CK|CC|CM|CR|CS|RC)_[A-Z0-9_]*(MUTANT|MUTATION|TEST|UNSAFE|BYPASS|SELFTEST|STAGING|PROBE)[A-Z0-9_]*\b" \
    --include=*.c --include=*.h --include=*.S . | grep -v '_H$' | grep -vx CK_INFER_PROBE | sort -u |
    while read -r m; do grep -qx "#ifdef ${m}" "${guard}" || echo "${m}"; done | tr '\n' ' ')"
if [ -z "${missing}" ]; then pass "every test-class name in native/ sources is in the guard"; else bad "guard lacks: ${missing}"; fi
if [ "${have_owner}" = 1 ]; then
    printf 'CK_EXTRA_CFLAGS := -DCK_FPU_PROBE=1\n' > "${T}/smuggle-probe.mk"
    if out="$(make -C "${kdir}" -f "${T}/smuggle-probe.mk" -f Makefile "${RC[@]}" OUT="${T}/g" CK_OWNER_PUBKEYS="${own}" \
        "${T}/g/obj/kernel/core/fmt.o" 2>&1)"; then
        bad "a switch smuggled in by an extra makefile compiled"
    elif grep -qF "Pi 5 release candidate refuses CK_FPU_PROBE" <<<"${out}"; then pass "a switch smuggled past layer 1 is stopped by the guard at compile time"
    else bad "smuggled switch refused for another reason: $(grep -m1 -E 'error|\*\*\*' <<<"${out}")"; fi
else
    skip "smuggled-switch compile test (no ceremony file)"
fi

echo "== layer 4: flag scan =="
if [ "${have_owner}" = 1 ]; then
    for d in -DCK_TEST_ROLLBACK=1 -DCK_HARDWARE_STAGING=1 -DCK_QEMU_UNSAFE_DMA=1 -DCK_TEST_FWCFG_ARTIFACTS=1 -DCK_SEED0B_TEST_ANCHOR=1 -DCC_MUTANT_SKIP_D1=1; do
        printf 'CK_EXTRA_CFLAGS := %s\n' "${d}" > "${T}/smuggle.mk"
        if out="$(make -n -C "${kdir}" -f "${T}/smuggle.mk" -f Makefile full "${RC[@]}" CK_OWNER_PUBKEYS="${own}" 2>&1)"; then
            bad "flag scan accepted ${d}"
        elif grep -qF "refuses test-class compile switches: ${d}" <<<"${out}"; then pass "flag scan refuses ${d}"
        else bad "flag scan: ${d} refused for another reason: $(grep -m1 -E '\*\*\*' <<<"${out}")"; fi
    done
else
    skip "flag scan (no ceremony file)"
fi

echo "== build: RC image (isolated OUT) =="
icheck="${root}/scripts/ck_pi5_rc_image_check.sh"
if [ "${have_owner}" = 1 ]; then
    if out="$(make -n -C "${kdir}" -B full "${RC[@]}" CK_OWNER_PUBKEYS="${own}" 2>&1)"; then
        c="$(grep -cE '^(gcc|aarch64-linux-gnu-gcc) .* -c ' <<<"${out}" || true)"
        g="$(grep -E '^(gcc|aarch64-linux-gnu-gcc) .* -c ' <<<"${out}" | grep -F -- "-include ${kdir}/include/ck_pi5_rc_guard.h" | grep -cF -- '-DCK_PI5_RC=1' || true)"
        if [ "${c}" -gt 0 ] && [ "${c}" = "${g}" ]; then pass "all ${c} kernel compile lines carry -DCK_PI5_RC=1 and the guard"; else bad "guard on ${g} of ${c} compile lines"; fi
    else
        bad "RC dry run refused: $(grep -m1 -F '***' <<<"${out}")"
    fi
    for k in 1 2; do
        if make -s -C "${kdir}" -j"$(nproc)" pi5-rc CK_OWNER_PUBKEYS="${own}" OUT="${T}/rc${k}" > "${T}/build${k}.log" 2>&1; then
            pass "make pi5-rc build ${k} (OUT ${T}/rc${k})"
        else
            bad "make pi5-rc build ${k} failed: $(grep -m3 -E 'FAIL|Error|error' "${T}/build${k}.log" | tr '\n' ' ')"
        fi
    done
    dir1="${T}/rc1/full-pi5-unlock-card-lifecycle-rc"
    dir2="${T}/rc2/full-pi5-unlock-card-lifecycle-rc"
    echo "== layer 5: image check and controls =="
    for img in "${dir1}/aienos-ck.elf" "${dir1}/BOOTAA64.EFI"; do
        if bash "${icheck}" "${img}" "${own}" > "${T}/ic.log" 2>&1; then pass "image check ${img##*/}"; else bad "image check ${img##*/}: $(grep FAIL "${T}/ic.log" | tr '\n' ' ')"; fi
    done
    key="$(awk '$1=="owner_root_ed25519" {print $2}' "${own}")"
    got="$(od -An -v -tx1 "${dir1}/BOOTAA64.EFI" | tr -d ' \n' | grep -o "${key}" | head -1 || true)"
    [ "${got}" = "${key}" ] && pass "BOOTAA64.EFI holds owner_root_ed25519 ${key}, byte for byte the ceremony file's"
    if [ -f "${dir1}/BOOTAA64.EFI" ] && [ -f "${dir2}/BOOTAA64.EFI" ] && cmp -s "${dir1}/BOOTAA64.EFI" "${dir2}/BOOTAA64.EFI"; then
        pass "two RC builds give the same BOOTAA64.EFI sha256 $(sha256sum "${dir1}/BOOTAA64.EFI" | cut -c1-64)"
    else
        bad "the two RC builds differ"
    fi
    # Controls that must FAIL the image check.
    make -s -C "${kdir}" -j"$(nproc)" pi5-lifecycle OUT="${T}/lc" > "${T}/lc.log" 2>&1 || bad "plain lifecycle control build failed"
    if bash "${icheck}" "${T}/lc/full-pi5-unlock-card-lifecycle/aienos-ck.elf" "${own}" > "${T}/c1.log" 2>&1; then
        bad "control: the plain lifecycle image passed the RC image check"
    else pass "control: the plain lifecycle image FAILS the RC image check ($(grep -c '^FAIL' "${T}/c1.log") findings)"; fi
    off="$(od -An -v -tx1 "${dir1}/BOOTAA64.EFI" | tr -d ' \n' | grep -ob "${key}" | awk -F: '$1 % 2 == 0 {print $1 / 2; exit}')"
    cp "${dir1}/BOOTAA64.EFI" "${T}/flip.efi"
    printf '\x00' | dd of="${T}/flip.efi" bs=1 seek="${off}" conv=notrunc status=none
    if bash "${icheck}" "${T}/flip.efi" "${own}" >/dev/null 2>&1; then bad "control: an image with one owner key byte changed passed"
    else pass "control: an image with one owner key byte changed FAILS"; fi
    if bash "${icheck}" "${dir1}/BOOTAA64.EFI" "${fix}" >/dev/null 2>&1; then bad "control: the RC image passed against the fixture owner file"
    else pass "control: the RC image FAILS against another owner file"; fi
    echo "rc_efi_sha256 $(sha256sum "${dir1}/BOOTAA64.EFI" | cut -c1-64)"
    echo "rc_elf_sha256 $(sha256sum "${dir1}/aienos-ck.elf" | cut -c1-64)"
else
    skip "RC build, image check, controls and repeat build (no ceremony file)"
fi

echo "== owner-key verification path (host) =="
if make -s -C "${kdir}" test-pi5-rc-anchor OUT="${T}/h" PI5_RC_TEST_OUT="${T}/h/fixture" > "${T}/a1.log" 2>&1; then
    pass "$(tail -1 "${T}/a1.log") with the TEST-fixture key as owner"
else bad "anchor test with the fixture key: $(tail -1 "${T}/a1.log")"; fi
if [ "${have_owner}" = 1 ]; then
    if make -s -C "${kdir}" test-pi5-rc-anchor OUT="${T}/h" PI5_RC_TEST_OWNER="${own}" PI5_RC_TEST_OUT="${T}/h/real" > "${T}/a2.log" 2>&1; then
        pass "$(tail -1 "${T}/a2.log") with the ceremony owner key (throwaway, TEST 1, TEST 3 and forged artifacts refused)"
    else bad "anchor test with the ceremony key: $(tail -1 "${T}/a2.log")"; fi
else
    skip "anchor test with the ceremony key (no ceremony file)"
fi
"${T}/h/fixture/test_pi5_rc_anchor" --print-throwaway-key > "${T}/throwaway.txt"
if make -s -C "${kdir}" test-pi5-rc-anchor OUT="${T}/h" PI5_RC_TEST_OWNER="${T}/throwaway.txt" PI5_RC_TEST_OUT="${T}/h/red" > "${T}/a3.log" 2>&1; then
    bad "red control: the anchor test passed with the throwaway key as owner"
else pass "red control: with the throwaway key as owner the anchor test FAILS ($(grep -c '  FAIL' "${T}/a3.log") checks)"; fi

if [ "${skips}" != 0 ] && [ "${AIENOS_STRICT:-0}" = 1 ]; then bad "${skips} step(s) skipped under AIENOS_STRICT=1"; fi
if [ "${fail}" = 0 ]; then echo "PI5_RC_CHECK: PASS (${skips} skipped)"; else echo "PI5_RC_CHECK: FAIL"; exit 1; fi
