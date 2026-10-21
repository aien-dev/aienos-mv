# L332 rung 2, window 0: give the GPU to our driver for a few minutes, read it, give it back

Purpose: the smallest real step. Prove we can take the GB10 away from NVIDIA's driver and give it back, measure how long
that takes (nvidia.ko has never been reloaded on this box since boot, so nobody knows), and let our module `aien-gpu`
read the chip's identity and set up its memory area. Our module writes nothing to the GPU in window 0.

Scheduled by: the coordinator (ledger line "L332 window 0"). Run only inside that window. Hands-off: nothing needs a
person at the machine unless the box fails to come back from a reboot (see Recovery).

## What goes down, and for how long
Everything that uses the GPU, from part 1 until the restore reboot at the end of part 2. The four NVIDIA modules are
nvidia_drm, nvidia_modeset, nvidia_uvm, nvidia (lsmod). Holders of /dev/nvidia* on 2026-10-21 22:0xZ (`sudo fuser -v`):
- aien-serve-http pid 2587647 (the 35B on 18091, owner L306/L387 server lane),
- qwen36_stroke_g pid 2613825 (motor stroke work, L312 lane),
- nvidia-persistenced pid 1816 (root service, enabled; fails harmlessly while the modules are held back).
Plus anything the judge or a window starts meanwhile. Stop order: the coordinator stops the chip jobs and the 35B server
(owners restart them after the restore reboot); persistenced is left alone (the reboot handles it).
Expected time down: two reboots plus about 2 minutes of steps. Reboot-to-ssh time on this box is not measured (journal
gaps are not clean reboots); part 2 logs boot time and uptime, so this window measures it.

## Why a reboot and not "unload the driver"
NVIDIA's display part has a worker that spins forever on this headless box (L158: nvEvoWaitForCoreNotifier). Unloading
`nvidia_modeset` waits for that worker, so the unload would most likely hang with the GPU half torn down (INFERRED from
~/handoffs/fixer-modeset-kthread.md item 2; not tested). So the GPU is freed by booting once with NVIDIA's modules held
back. The boot entry and GRUB are NOT changed (GRUB_DEFAULT=0, no grub-reboot); a temporary file in /etc/modprobe.d holds
the modules back for one boot and part 2 deletes it first thing. The NVIDIA modules are not in the initramfs (checked by
part 1), so that file is enough. Alternative not chosen: kexec with `modprobe.blacklist=` on the command line (no file,
faster) because the GPU state after a kexec without a firmware reset is UNKNOWN on GB10.

## Facts checked on 2026-10-21 (read-only)
- Secure Boot disabled, lockdown none, module signatures not enforced: the unsigned aien_gpu.ko loads (taints).
- aien_gpu.ko vermagic 7.0.0-1019-nvidia = running kernel.
- kernel.panic = 0: a kernel panic would freeze the box until power-cycled. Part 2 sets kernel.panic=10 and
  panic_on_oops=1 (runtime only, gone at the next boot) before our module touches the chip, so a bad register read
  reboots the box on its own.
- GB10 = 000f:01:00.0, 10de:2e12, BAR0 64 MiB at 0x24000000.

## Steps
Scripts in this directory, each line timed into window0-<UTC>.log (window0-current.log points at it).
0. Any time before the window: `./window0-pre.sh` (no --go) checks the module, Secure Boot, initramfs and lists holders.
1. Window opens; coordinator stops the GPU users; `./window0-pre.sh --go`: records lsmod, nvidia-smi, cmdline, module
   hash, refuses if anything but persistenced still holds the GPU, writes /etc/modprobe.d/zz-aien-gpu-window0.conf and
   reboots.
2. After ssh is back: `./window0-post.sh`: deletes the file; logs boot time; checks no nvidia module and no driver on the
   GPU; sets panic auto-reboot; insmod aien_gpu allow_bind=1; aien_gpu_check (identity, firmware size, arena mmap
   pattern); rmmod; timed `modprobe nvidia nvidia_uvm` + first nvidia-smi answer (NVIDIA's load-to-ready baseline);
   Omega gpu_launch_cost_test --reps 3 (first open after load, timed); prints WINDOW0 PASS or FAIL; normal reboot.
3. After the restore reboot: `lsmod | grep -c ^nvidia` = 4, `nvidia-smi -L` lists the GPU, no file
   /etc/modprobe.d/zz-aien-gpu-window0.conf; owners restart their servers; commit the log; window closes.

## Recovery
- Anything odd during part 2: `sudo rmmod aien_gpu` if loaded, then `sudo reboot` (the file is already gone, so the
  normal boot is today's boot).
- Box does not come back within 10 minutes after part 1's reboot: it is most likely booting with NVIDIA held back but
  without network-facing trouble; if ssh never answers, a power cycle at the wall is the only step left (Drake, physical).
  After any boot, delete /etc/modprobe.d/zz-aien-gpu-window0.conf if it is still there, then reboot once more.
- Kernel panic while our module reads the chip: auto-reboot after 10 s (set by part 2), file already deleted, so the
  box comes back as today.

## Gate (window 0 PASS when all hold)
- aien-gpu: PMC_BOOT_0 read (not 0, not all-ones), firmware 75,012,080 bytes, arena mmap pattern PASS, rmmod clean.
- After the reload: nvidia-smi lists the GPU; Omega launch-cost test exit 0.
- After the restore reboot: four modules back, file gone.
- Log committed under evidence/own-gpu-driver/rung2/, pass or fail.

## Later windows (each needs its own prepared step and its own request)
1. GSP boot through FSP from aien-gpu (L192 builder + the BAR0 MMIO sequence, L346).
2. The RPC subset from docs/numeric/DRIVER_FLOOR.md and one channel (gpu/contract/gb10_chan.devc).
3. Omega on aien-gpu through L202's typed handles: golden tokens identical to nvidia.ko, per-word timing and start time side by side.
