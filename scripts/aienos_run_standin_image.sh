#!/usr/bin/env bash
# aienos_run_standin_image.sh -- wrap an x86_64 AienOS UEFI kernel (BOOTX64.EFI, e.g.
# IMACKERN.EFI from `make -C native/kernel imac-kernel`) into a UEFI-bootable qcow2 disk and
# a manifest, the stand-in aienos-run (ledger L261) boots until the PC edition image (L260,
# aienos-pc-0.1.0-vm.qcow2) is published.
#
#     scripts/aienos_run_standin_image.sh KERNEL.EFI OUTDIR
#       -> OUTDIR/aienos-standin-x86_64.qcow2   (FAT ESP with EFI/BOOT/BOOTX64.EFI)
#       -> OUTDIR/manifest.txt                  (aienos-run-manifest 1, unsigned)
#
# Only qemu-img is needed: it reads a folder as a FAT disk (QEMU's vvfat driver, MBR with
# one FAT16 partition, which OVMF boots as removable media) and writes qcow2. File times
# are pinned so two runs give the same bytes. Last line:
#     AIENOS_RUN_STANDIN: PASS <sha256> <bytes>   exit 0, or FAIL (...) exit 1
set -euo pipefail
tag="AIENOS_RUN_STANDIN"
fail() { echo "${tag}: FAIL ($*)"; exit 1; }
efi="${1:?usage: $0 KERNEL.EFI OUTDIR}"
out="${2:?usage: $0 KERNEL.EFI OUTDIR}"
[[ -f "${efi}" ]] || fail "no such file ${efi}"
command -v qemu-img >/dev/null || fail "qemu-img missing"
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
# "MZ" + PE: refuse anything that is not a PE image.
[[ "$(head -c 2 "${efi}")" == "MZ" ]] || fail "${efi} is not a PE/UEFI image"

mkdir -p "${out}"
out="$(cd "${out}" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
mkdir -p "${work}/esp/EFI/BOOT"
cp "${efi}" "${work}/esp/EFI/BOOT/BOOTX64.EFI"
find "${work}/esp" -exec touch -h -t 202601010000 {} +

img="${out}/aienos-standin-x86_64.qcow2"
rm -f "${img}"
qemu-img convert -f vvfat -O qcow2 "fat:16:${work}/esp" "${img}" || fail "qemu-img convert"
size="$(wc -c <"${img}" | tr -d ' ')"
sum="$(sha "${img}")"
{
    echo "aienos-run-manifest 1"
    echo "# stand-in: $(basename "${efi}") sha256 $(sha "${efi}") on a FAT ESP (not the PC edition)"
    echo "channel standin"
    echo "image x86_64 disk-qcow2 aienos-standin-x86_64.qcow2 ${size} ${sum} aienos-standin-x86_64.qcow2"
} >"${out}/manifest.txt"
echo "${tag}: PASS ${sum} ${size}"
