#!/usr/bin/env bash
# L332 rung 2 window 0, part 1 (before the reboot). Run ONLY inside the coordinator-scheduled window, after the GPU users
# on the coordinator's list are stopped. Records the box, checks the module, writes a temporary "do not load NVIDIA"
# file (removed by part 2 as its first action) and reboots. Usage: window0-pre.sh [--go]  (without --go: checks only).
set -u
here=$(cd "$(dirname "$0")" && pwd); repo=$(cd "$here/../../.." && pwd)
log=$here/window0-$(date -u +%Y%m%dT%H%M%SZ).log; ln -sfn "$(basename "$log")" "$here/window0-current.log"
ts() { echo "$(date -u +%FT%T.%NZ) $*" | tee -a "$log"; }
fail() { ts "PRE FAIL: $*"; exit 1; }
KO=$repo/native/linux/aien-gpu/aien_gpu.ko
BLK=/etc/modprobe.d/zz-aien-gpu-window0.conf
ts "pre start, commit $(git -C "$repo" rev-parse HEAD), kernel $(uname -r)"
{ lsmod | grep -E '^nvidia'; nvidia-smi -L; cat /proc/cmdline; sha256sum "$KO"; } >> "$log" 2>&1
[ -f "$KO" ] || fail "no $KO (make -C native/linux/aien-gpu)"
[ "$(modinfo -F vermagic "$KO" | cut -d' ' -f1)" = "$(uname -r)" ] || fail "aien_gpu.ko vermagic does not match $(uname -r)"
[ "$(mokutil --sb-state 2>/dev/null)" = "SecureBoot disabled" ] || fail "Secure Boot state changed: unsigned module would be refused"
lsinitramfs "/boot/initrd.img-$(uname -r)" | grep -qE '/nvidia(_uvm|_modeset|_drm)?\.ko' && fail "nvidia modules in initramfs: the modprobe.d file would not hold them back"
holders=$(sudo fuser /dev/nvidia* 2>/dev/null | tr -s ' ' '\n' | grep -E '^[0-9]+$' | sort -u)
for p in $holders; do ts "holder pid $p $(ps -o user=,comm= -p "$p")"; done
other=$(for p in $holders; do [ "$(ps -o comm= -p "$p")" = nvidia-persiste ] || echo "$p"; done)
[ -z "$other" ] || { [ "${1:-}" = --go ] && fail "GPU still held by: $other (coordinator stops them first)"; ts "WARN still held by: $other"; }
[ "${1:-}" = --go ] || { ts "checks only (no --go): PASS"; exit 0; }
printf '# L332 window 0, temporary: removed by window0-post.sh as its first step. If you find this file, delete it.\ninstall nvidia /bin/false\ninstall nvidia_uvm /bin/false\ninstall nvidia_modeset /bin/false\ninstall nvidia_drm /bin/false\ninstall nvidiafb /bin/false\n' | sudo tee "$BLK" > /dev/null
sync; ts "blacklist written $BLK; rebooting (part 2: window0-post.sh after ssh is back)"
sudo systemctl reboot
