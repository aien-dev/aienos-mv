#!/bin/sh
# apple_dt_fixtures.sh -- rebuild the pinned Apple device-tree test fixtures
# (native/kernel/tests/fixtures/apple, see SOURCES.md there). TEST INPUT ONLY:
# these "GPL-2.0+ OR MIT" trees are never compiled into the kernel.
#
#   scripts/apple_dt_fixtures.sh <AsahiLinux/linux checkout at 94fb2334> <dtc>
#
# The Asahi trees use floating-point cells, so <dtc> must be the dtc from the
# same Asahi tree (scripts/dtc, built on the host); Ubuntu's dtc 1.7.0 refuses
# them. Prints the sha256 of every file; compare with SOURCES.md.
set -eu
src=${1:?Asahi linux checkout}
dtc=${2:?dtc binary}
out=$(cd "$(dirname "$0")/../native/kernel/tests/fixtures/apple" && pwd)
dts=$src/arch/arm64/boot/dts/apple
tmp=$(mktemp -d)
for b in t8103-j313 t8112-j413 t8122-j613 t8132-j713 t6000-j314s t6020-j414s t6031-j514c; do
    cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I "$src/include" -I "$dts" "$dts/$b.dts" -o "$tmp/$b.dts"
    "$dtc" -q -I dts -O dtb -o "$out/$b.dtb" "$tmp/$b.dts"
done
# QEMU's own virt tree (the board the CK_APPLE=1 Image boots in the QEMU gate).
qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,dumpdtb="$tmp/virt.dtb" -cpu cortex-a76 -smp 4 -m 1024 \
    -nographic >/dev/null 2>&1 || true
"$dtc" -q -I dtb -O dtb -o "$out/qemu-virt.dtb" "$tmp/virt.dtb"
rm -f "$tmp"/*.dts "$tmp"/virt.dtb
rmdir "$tmp"
(cd "$out" && sha256sum ./*.dtb)
