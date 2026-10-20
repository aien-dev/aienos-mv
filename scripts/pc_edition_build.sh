#!/usr/bin/env bash
# pc_edition_build.sh -- build the AienOS PC edition release files from one commit
# (ledger L260, docs/PC_EDITION.md). Host only: writes files under the work and
# output folders, never a device, a firmware variable or a key.
#
# Usage: bash scripts/pc_edition_build.sh [--out DIR] [--work DIR]
#   --out DIR   where the release files go (default dist/)
#   --work DIR  build folder (default target/pc-edition); scripts/pc_edition_repro.sh
#               uses two of them to check the build is reproducible
# Run it through quiet-build on the Spark: quiet-build bash scripts/pc_edition_build.sh
#
# Output (VERSION below; names are the contract with aienos-run, ledger L261):
#   aienos-pc-live-x86_64.iso    live image: one file for a USB stick (written byte
#                                for byte) or a DVD; UEFI boots EFI/BOOT/BOOTX64.EFI
#   aienos-pc-vm-x86_64.img      VM disk, raw GPT (QEMU, or the source of the others)
#   aienos-pc-vm-x86_64.qcow2    VM disk for aienos-run (QEMU + WHPX)
#   aienos-pc-vm-x86_64.vhdx     VM disk for Hyper-V (generation 2, Secure Boot off)
#   aienos-pc-manifest.json      what was built: sha256 and size of each file, tree
#                                commit, toolchain versions
#   aienos-pc-manifest.json.sig  Ed25519 signature (64 bytes) over the statement that
#                                binds the manifest (native/kernel/tools/ck_manifest_sig.c,
#                                purpose aienos-pc-release-manifest). There is no release
#                                key yet: signed with the public RFC 8032 TEST 1 key and
#                                the manifest says so.
#   SHA256SUMS                   sha256sum format, every file above
#
# Every output is a function of the commit: the kernel embeds the commit, the FAT
# image takes its times and serial from the commit time, and the image writer
# derives its "unique" ids from the bytes (native/kernel/tools/ck_pc_image.c).
# Final line: PC_EDITION_BUILD: PASS <tag> commit=<sha> or PC_EDITION_BUILD: FAIL <why>.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

VERSION=0.1.0
TAG="aienos-pc-v${VERSION}"
PURPOSE=aienos-pc-release-manifest
TEST1_PK=d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
ESP_MIB=16
VM_MIB=64

out=dist work=target/pc-edition
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) out="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        *) echo "usage: $0 [--out DIR] [--work DIR]"; exit 2 ;;
    esac
done
fail() { echo "PC_EDITION_BUILD: FAIL $*"; exit 1; }

cross="${CROSS:-x86_64-linux-gnu-}"
for t in "${cross}gcc" "${cross}ld" "${cross}objcopy" cc mformat mmd mcopy sha256sum qemu-img sgdisk xorriso git; do
    command -v "$t" >/dev/null || fail "missing tool $t"
done

commit="$(git rev-parse HEAD)"
epoch="$(git log -1 --format=%ct HEAD)"
dirty=false
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    [[ "${AIENOS_PC_ALLOW_DIRTY:-0}" == 1 ]] || fail "the tree has uncommitted changes (AIENOS_PC_ALLOW_DIRTY=1 builds anyway and says so)"
    dirty=true
fi

mkdir -p "${work}" "${out}"
work="$(cd "${work}" && pwd)" out="$(cd "${out}" && pwd)"
kout="${work}/kernel"
rm -rf "${work}/stage"
mkdir -p "${work}/stage"
st="${work}/stage"
log="${work}/build.log"
: > "${log}"

echo "== kernel and image writer (make OUT=${kout}) =="
make -s -C native/kernel OUT="${kout}" pc-kernel pc-image-tool >> "${log}" 2>&1 || { tail -20 "${log}"; fail "make"; }
img_tool="${kout}/host/ck_pc_image"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -Inative/sig -Inative/argus -o "${work}/ck_manifest_sig" \
    native/kernel/tools/ck_manifest_sig.c native/sig/ed25519.c native/sig/sha512.c native/sig/ct.c \
    native/argus/sha256.c >> "${log}" 2>&1 || fail "ck_manifest_sig build"
kernel="${kout}/PCKERN.EFI"

echo "== ESP (FAT, ${ESP_MIB} MiB, times and serial from the commit) =="
cat > "${st}/README.TXT" <<EOF
AienOS PC edition ${VERSION} (Aienux kernel), commit ${commit}
Boots on any x86_64 PC or virtual machine with UEFI. It reads disks and never writes them.
Guide: docs/PC_EDITION.md in https://github.com/aien-dev/aienos
EOF
sed -i 's/$/\r/' "${st}/README.TXT" # CR LF for Windows readers
cp "${kernel}" "${st}/BOOTX64.EFI"
touch -d "@${epoch}" "${st}/README.TXT" "${st}/BOOTX64.EFI"
esp="${st}/esp.img"
truncate -s "${ESP_MIB}M" "${esp}"
serial="$(printf '%08X' $(( epoch & 0xffffffff )))"
export SOURCE_DATE_EPOCH="${epoch}" TZ=UTC
mformat -i "${esp}" -N "${serial}" -v AIENOS :: >> "${log}" 2>&1 || fail "mformat"
mmd -i "${esp}" ::/EFI ::/EFI/BOOT >> "${log}" 2>&1 || fail "mmd"
mcopy -m -i "${esp}" "${st}/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI >> "${log}" 2>&1 || fail "mcopy kernel"
mcopy -m -i "${esp}" "${st}/README.TXT" ::/README.TXT >> "${log}" 2>&1 || fail "mcopy readme"

echo "== images =="
iso="${st}/aienos-pc-live-x86_64.iso" raw="${st}/aienos-pc-vm-x86_64.img"
qcow="${st}/aienos-pc-vm-x86_64.qcow2" vhdx="${st}/aienos-pc-vm-x86_64.vhdx"
"${img_tool}" iso "${esp}" "${st}/README.TXT" "${iso}" "${epoch}" | tee -a "${log}"
"${img_tool}" disk "${esp}" "${raw}" "${VM_MIB}" | tee -a "${log}"
"${img_tool}" qcow2 "${raw}" "${qcow}" | tee -a "${log}"
"${img_tool}" vhdx "${raw}" "${vhdx}" | tee -a "${log}"

echo "== independent checks =="
"${img_tool}" check-gpt "${raw}" | tee -a "${log}" | grep -q PASS || fail "kernel GPT parser on the VM disk"
"${img_tool}" check-gpt "${iso}" | tee -a "${log}" | grep -q PASS || fail "kernel GPT parser on the live image"
sgdisk -v "${raw}" 2>&1 | tee -a "${log}" | grep -q 'No problems found' || fail "sgdisk -v on the VM disk"
sgdisk -v "${iso}" 2>&1 | tee -a "${log}" | grep -q 'No problems found' || fail "sgdisk -v on the live image"
for f in "${qcow}" "${vhdx}"; do
    qemu-img check "${f}" >> "${log}" 2>&1 || fail "qemu-img check $(basename "${f}")"
    qemu-img compare -f raw "${raw}" "${f}" >> "${log}" 2>&1 || fail "qemu-img compare raw vs $(basename "${f}")"
done
xorriso -indev "${iso}" -report_el_torito plain -find / -type f -- >> "${log}" 2>&1 || fail "xorriso read"
grep -qE 'El Torito boot img :.*UEFI' "${log}" || fail "xorriso sees no UEFI El Torito entry"
mdir -i "${esp}" ::/EFI/BOOT >> "${log}" 2>&1 || fail "mdir"
echo "checks: kernel GPT parser, sgdisk, qemu-img check+compare, xorriso El Torito: PASS"

echo "== manifest =="
arts=("${iso}" "${raw}" "${qcow}" "${vhdx}")
ver() { "$@" 2>&1 | head -1; }
{
    printf '{\n  "schema": "aienos-pc-manifest-v1",\n  "version": "%s",\n  "tag": "%s",\n' "${VERSION}" "${TAG}"
    printf '  "tree_sha": "%s",\n  "tree_dirty": %s,\n  "source_date_epoch": %s,\n' "${commit}" "${dirty}" "${epoch}"
    printf '  "kernel": {"name": "PCKERN.EFI", "path_in_image": "EFI/BOOT/BOOTX64.EFI", "size": %s, "sha256": "%s"},\n' \
        "$(stat -c %s "${kernel}")" "$(sha256sum "${kernel}" | cut -c1-64)"
    printf '  "toolchain": {"cc_x86_64": "%s", "ld_x86_64": "%s", "objcopy_x86_64": "%s", "host_cc": "%s", "mtools": "%s"},\n' \
        "$(ver "${cross}gcc" --version)" "$(ver "${cross}ld" --version)" "$(ver "${cross}objcopy" --version)" \
        "$(ver cc --version)" "$(ver mformat --version)"
    printf '  "checked_with": {"qemu_img": "%s", "sgdisk": "%s", "xorriso": "%s"},\n' \
        "$(ver qemu-img --version)" "$(sgdisk --version 2>&1 | head -1)" "$(ver xorriso -version)"
    printf '  "signature": {"file": "aienos-pc-manifest.json.sig", "alg": "ed25519", "statement": "AIENOS-OWNER-STATEMENT-V1 purpose %s", ' "${PURPOSE}"
    printf '"key": "TEST", "public_key": "%s", "note": "RFC 8032 TEST 1 key: public, proves the format only; no release key exists yet"},\n' "${TEST1_PK}"
    printf '  "artifacts": [\n'
    for i in "${!arts[@]}"; do
        f="${arts[$i]}" n="$(basename "${arts[$i]}")"
        case "${n}" in
            *.iso) kind=live fmt=iso ;;
            *.img) kind=vm fmt=raw ;;
            *.qcow2) kind=vm fmt=qcow2 ;;
            *.vhdx) kind=vm fmt=vhdx ;;
        esac
        sep=","; [[ $i -eq $(( ${#arts[@]} - 1 )) ]] && sep=""
        printf '    {"name": "%s", "kind": "%s", "format": "%s", "size": %s, "sha256": "%s"}%s\n' \
            "${n}" "${kind}" "${fmt}" "$(stat -c %s "${f}")" "$(sha256sum "${f}" | cut -c1-64)" "${sep}"
    done
    printf '  ]\n}\n'
} > "${st}/aienos-pc-manifest.json"
man="${st}/aienos-pc-manifest.json"
"${work}/ck_manifest_sig" sign-test "${man}" "${man}.sig" "${PURPOSE}" | tee -a "${log}"
"${work}/ck_manifest_sig" verify "${man}" "${man}.sig" "${TEST1_PK}" "${PURPOSE}" | tee -a "${log}" | grep -q '^MANIFEST_SIG: PASS' \
    || fail "manifest signature does not verify"
(cd "${st}" && sha256sum "${arts[@]##*/}" aienos-pc-manifest.json aienos-pc-manifest.json.sig > SHA256SUMS)

for f in "${arts[@]##*/}" aienos-pc-manifest.json aienos-pc-manifest.json.sig SHA256SUMS; do
    cp "${st}/${f}" "${out}/${f}"
done
(cd "${out}" && sha256sum -c --quiet SHA256SUMS) || fail "SHA256SUMS does not match the copied files"
echo "== ${out} =="
(cd "${out}" && ls -l "${arts[@]##*/}" aienos-pc-manifest.json aienos-pc-manifest.json.sig SHA256SUMS | awk '{print $5, $9}')
cat "${out}/SHA256SUMS"
echo "PC_EDITION_BUILD: PASS ${TAG} commit=${commit} dirty=${dirty}"
