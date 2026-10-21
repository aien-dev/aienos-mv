#!/usr/bin/env bash
# L332 rung 2 window 1, part 1 (before the reboot). Run ONLY inside the coordinator-scheduled window 1, after window 0
# passed and the GPU users on the coordinator's list are stopped. Checks everything part 2 needs, records the box, writes
# a temporary "do not load NVIDIA" file (removed by part 2 as its first action) and reboots.
# Usage: window1-pre.sh PKG_DIR [--go]   (without --go: checks only; PKG_DIR from window1-package.sh)
set -u
here=$(cd "$(dirname "$0")" && pwd); repo=$(cd "$here/../../.." && pwd)
PKG=${1:?usage: window1-pre.sh PKG_DIR [--go]}; PKG=$(cd "$PKG" && pwd)
log=$here/window1-$(date -u +%Y%m%dT%H%M%SZ).log; ln -sfn "$(basename "$log")" "$here/window1-current.log"
ts() { echo "$(date -u +%FT%T.%NZ) $*" | tee -a "$log"; }
fail() { ts "PRE FAIL: $*"; exit 1; }
SHIM=$repo/native/linux/aien-gpu
KO=$SHIM/aien_gpu.ko BOOT=$SHIM/build/aien_gsp_boot
BLK=/etc/modprobe.d/zz-aien-gpu-window1.conf
ts "pre start, commit $(git -C "$repo" rev-parse HEAD), kernel $(uname -r), package $PKG"
{ lsmod | grep -E '^nvidia'; nvidia-smi -L; cat /proc/cmdline; sha256sum "$KO" "$BOOT"; cat "$PKG/MANIFEST.txt"; } >> "$log" 2>&1
ls "$here"/window0-*.log 2>/dev/null | xargs -r grep -l 'WINDOW0 PASS' | grep -q . || fail "no window-0 log with WINDOW0 PASS: window 0 comes first"
[ -f "$KO" ] && [ -x "$BOOT" ] || fail "build first: make -C native/linux/aien-gpu all tools"
[ "$(modinfo -F vermagic "$KO" | cut -d' ' -f1)" = "$(uname -r)" ] || fail "aien_gpu.ko vermagic does not match $(uname -r)"
modinfo -F parm "$KO" | grep -q '^allow_boot:' || fail "aien_gpu.ko has no allow_boot (old build)"
for f in rm_elf rm_sig bootloader fmc fmc_hash fmc_pkey fmc_sig system_info; do [ -s "$PKG/$f.bin" ] || fail "package lacks $f.bin"; done
[ -s "$PKG/app_version.txt" ] || fail "package lacks app_version.txt"
[ "$(mokutil --sb-state 2>/dev/null)" = "SecureBoot disabled" ] || fail "Secure Boot state changed: unsigned module would be refused"
lsinitramfs "/boot/initrd.img-$(uname -r)" | grep -qE '/nvidia(_uvm|_modeset|_drm)?\.ko' && fail "nvidia modules in initramfs: the modprobe.d file would not hold them back"
holders=$(sudo fuser /dev/nvidia* 2>/dev/null | tr -s ' ' '\n' | grep -E '^[0-9]+$' | sort -u)
for p in $holders; do ts "holder pid $p $(ps -o user=,comm= -p "$p")"; done
other=$(for p in $holders; do [ "$(ps -o comm= -p "$p")" = nvidia-persiste ] || echo "$p"; done)
[ -z "$other" ] || { [ "${2:-}" = --go ] && fail "GPU still held by: $other (coordinator stops them first)"; ts "WARN still held by: $other"; }
[ "${2:-}" = --go ] || { ts "checks only (no --go): PASS"; exit 0; }
echo "$PKG" > "$here/window1-package.path"
printf '# L332 window 1, temporary: removed by window1-post.sh as its first step. If you find this file, delete it.\ninstall nvidia /bin/false\ninstall nvidia_uvm /bin/false\ninstall nvidia_modeset /bin/false\ninstall nvidia_drm /bin/false\ninstall nvidiafb /bin/false\n' | sudo tee "$BLK" > /dev/null
sync; ts "blacklist written $BLK; rebooting (part 2: window1-post.sh after ssh is back)"
sudo systemctl reboot
