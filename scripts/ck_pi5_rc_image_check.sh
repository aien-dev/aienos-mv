#!/usr/bin/env bash
# ck_pi5_rc_image_check.sh -- byte-level check of one Pi 5 release candidate image (PI5-8 cut R).
#
#   bash scripts/ck_pi5_rc_image_check.sh <aienos-ck.elf or BOOTAA64.EFI> <owner pubkeys file> [nm tool]
#
# Run by native/kernel/Makefile after every RC link (the image is deleted when it fails) and by
# scripts/ck_pi5_rc_check.sh on the RC image and on control images that must fail. PUBLIC bytes only.
# Checks, all must hold (exit 0, last line "PI5_RC_IMAGE: PASS"), else exit 1 with "PI5_RC_IMAGE: FAIL":
#   1. the owner root public key of the owner file is in the image exactly once, as raw bytes;
#   2. none of these is in the image as raw bytes: the RFC 8032 section 7.1 TEST 1/2/3 public keys and secret
#      seeds (the SEED-0B TEST anchor, the TEST slot key, the OSC TEST anchor), the TEST-fixture owner key of
#      native/kernel/tests/fixtures/owner;
#   3. no TEST Store identity text (the hardware staging owner_check list), no TEST-ONLY / TEST ONLY banner,
#      no SEED-0B qualification banner, no fw_cfg artifact or model source text;
#   4. (ELF only, with nm) no TEST key, TEST Store key or fw_cfg symbol;
#   5. the owner anchor announcement of core/artifact_loader.c is present.
# No Python; od, grep, awk only.
set -euo pipefail
img="${1:?usage: ck_pi5_rc_image_check.sh <image> <owner pubkeys file> [nm]}"
own="${2:?usage: ck_pi5_rc_image_check.sh <image> <owner pubkeys file> [nm]}"
nm_tool="${3:-nm}"
fail=0
bad() { echo "FAIL $*"; fail=1; }
ok() { echo "PASS $*"; }

[ -s "${img}" ] || { echo "FAIL no image ${img}"; echo "PI5_RC_IMAGE: FAIL"; exit 1; }
key="$(awk '$1=="owner_root_ed25519" {print tolower($2)}' "${own}")"
if ! printf '%s' "${key}" | grep -qxE '[0-9a-f]{64}'; then
    echo "FAIL owner file ${own} has no single owner_root_ed25519 line of 64 hex digits"
    echo "PI5_RC_IMAGE: FAIL"
    exit 1
fi

# The image as one lowercase hex string; a byte match is a match at an even offset.
hexdump_file="$(mktemp)"
trap 'rm -f "${hexdump_file}"' EXIT
od -An -v -tx1 "${img}" | tr -d ' \n' > "${hexdump_file}"
count_bytes() {
    { grep -ob "$1" "${hexdump_file}" || true; } | awk -F: '$1 % 2 == 0 {n++} END {print n + 0}'
}

n="$(count_bytes "${key}")"
if [ "${n}" = 1 ]; then ok "owner root public key ${key} present once as raw bytes"; else bad "owner root public key found ${n} times (want exactly 1)"; fi

while read -r name hex; do
    n="$(count_bytes "${hex}")"
    if [ "${n}" = 0 ]; then ok "no ${name}"; else bad "image carries ${name} (${n} times)"; fi
done <<'EOF'
RFC8032-TEST1-public-key d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
RFC8032-TEST2-public-key 3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c
RFC8032-TEST3-public-key fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025
RFC8032-TEST1-secret-seed 9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60
RFC8032-TEST2-secret-seed 4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb
RFC8032-TEST3-secret-seed c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7
TEST-fixture-owner-key a980e270f6778c14e7a526ad997aa370b8001d24901042235215277ea7233125
EOF

s="$(grep -aoE "AIENOS-LANE18-TEST-KVOL-NOT-SECRET|AIEN-TEST-BOOT01|TEST identity, TEST keys|formatted TEST store|TEST machine id 0xA1|TEST-ONLY|TEST ONLY|seed0b-test qualification build|opt/aienos/artifacts|opt/aienos/model|fw_cfg|QEMU0002" "${img}" | sort -u | tr '\n' ' ' || true)"
if [ -z "${s}" ]; then ok "no TEST Store identity, TEST-ONLY banner, SEED-0B banner or fw_cfg text"; else bad "image carries: ${s}"; fi

if [ "$(head -c 4 "${img}" | od -An -tx1 | tr -d ' ')" = 7f454c46 ]; then
    y="$("${nm_tool}" "${img}" | awk '{print $NF}' | grep -xE "cka_test1_pk|cka_test2_pk|osc_unit_test1_pk|ck_store_test_keys|ck_store_test_uuid|fw_select|fw_read|fw_skip|fw_find|fw_dma_read|fwcfg_from_dsdt|fwcfg_source|fwcfg" | tr '\n' ' ' || true)"
    if [ -z "${y}" ]; then ok "no TEST key, TEST Store key or fw_cfg symbol"; else bad "image defines: ${y}"; fi
fi

if grep -aqF "artifact_trust: PI5-8 RC owner anchor only, test_anchors=0 owner_root_ed25519=%s" "${img}"; then
    ok "owner anchor announcement present"
else
    bad "no owner anchor announcement (not an RC image)"
fi

if [ "${fail}" = 0 ]; then echo "PI5_RC_IMAGE: PASS ${img}"; else echo "PI5_RC_IMAGE: FAIL ${img}"; exit 1; fi
