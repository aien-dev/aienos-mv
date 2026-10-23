#!/bin/sh
# External development fixture construction from frozen legacy byte layout.
set -eu
out=$1
mkdir -p "$out"
dd if=/dev/zero of="$out/memory-golden.bin" bs=4096 count=1 status=none
printf 'AIENALM1\004\000\000\000\114\000\000\000\001\000\000\000\000\000\000\000' | dd of="$out/memory-golden.bin" conv=notrunc status=none
printf '\003' | dd of="$out/memory-golden.bin" bs=1 seek=64 conv=notrunc status=none
printf 'Allen' | dd of="$out/memory-golden.bin" bs=1 seek=104 conv=notrunc status=none
{
 dd if="$out/memory-golden.bin" bs=1 count=32 status=none
 dd if="$out/memory-golden.bin" bs=1 skip=64 count=76 status=none
} > "$out/hash-input.bin"
sha256sum "$out/hash-input.bin" | cut -d' ' -f1 | xxd -r -p | dd of="$out/memory-golden.bin" bs=1 seek=32 conv=notrunc status=none
