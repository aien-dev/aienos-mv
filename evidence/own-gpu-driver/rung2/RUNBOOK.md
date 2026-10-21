# L332 rung 2, window 0: give the GPU to our driver for a few minutes, read it, give it back

Purpose: the smallest real step. Prove we can take the GB10 away from NVIDIA's driver and give it back, measure how long
that takes (nobody has measured it on this box: nvidia.ko has not been reloaded since boot), and let our module
`aien-gpu` read the chip's identity and set up its memory area. Our module writes nothing to the GPU in window 0.

Scheduled by: the coordinator (ledger line "L332 window 0"). Run only inside that window.

## What goes down
Everything that uses the GPU: the model server on 18091 (35B, L306), the judge's GPU gates, every chip job and any
training run. `sudo fuser -v /dev/nvidia*` lists the holders at the start; on 2026-10-21 01:4xZ they were a python
training job (user) and nvidia-persistenced (root, pid 1816). How long it stays down: not measured; this window
measures it (every step below is timed into the log).

## Why a reboot and not "unload the driver"
NVIDIA's display part has a worker that spins forever on this headless box (L158: nvEvoWaitForCoreNotifier, one core
since boot). Unloading `nvidia_modeset` has to wait for that worker, so the unload would most likely hang with the GPU
half torn down (INFERRED, ~/handoffs/fixer-modeset-kthread.md item 2; not tested). So the GPU is freed by booting once
with NVIDIA's modules held back, and given back by loading them again (or by a normal reboot).

## Steps (each line timed: `date -u +%FT%T.%NZ` before and after, into evidence/own-gpu-driver/rung2/window0-<UTC>.log)
1. Before: `nvidia-smi -L`, `lsmod | grep nvidia`, record. Stop GPU users (coordinator's list).
2. Reboot with this added to the kernel command line FOR ONE BOOT ONLY (GRUB menu, `e` on the default entry, append,
   Ctrl-X; nothing is saved): `modprobe.blacklist=nvidia,nvidia_uvm,nvidia_modeset,nvidia_drm`
   After boot: `lsmod | grep nvidia` is empty; `lspci -k -s 000f:01:00.0` shows no driver in use.
3. `sudo insmod native/linux/aien-gpu/aien_gpu.ko allow_bind=1` then `sudo dmesg | grep aien-gpu`
   Expect: BAR0 67108864 bytes, PMC_BOOT_0 not 0 and not ffffffff, firmware ... ok 1, /dev/aien-gpu ready.
4. `sudo /path/aien_gpu_check` (built from native/linux/aien-gpu/aien_gpu_check.c). Expect exit 0, "arena pattern PASS".
5. `sudo rmmod aien_gpu`.
6. Give the GPU back, timed: `sudo modprobe nvidia && sudo modprobe nvidia_uvm`, then `nvidia-smi -L` (time to first
   answer = NVIDIA's load-to-ready, the baseline our driver has to beat).
7. Prove it works: Omega `gpu_launch_cost_test --reps 3` (all checks ok), then the rung 0 script in `run` mode for the
   start-time baseline.
8. Reboot normally (restores nvidia_modeset/nvidia_drm exactly as before, and the blacklist is gone because it was never saved).

## Recovery
- Anything odd at steps 3 to 5: `sudo rmmod aien_gpu` (if loaded) and go to step 6.
- Step 6 fails or `nvidia-smi` does not answer: reboot normally (step 8). The normal boot loads NVIDIA's driver as today.
- The machine does not come back from a reboot: power cycle at the wall; the boot entry was never changed.
Times of each recovery step: not measured yet; step 6 and step 8 measure them.

## Gate (window 0 PASS when all hold)
- aien-gpu: BOOT_0 read (not 0, not all-ones), firmware size 75,012,080 bytes, arena mmap pattern PASS, unload clean.
- After step 6: nvidia-smi lists the GPU; Omega launch-cost test all checks ok.
- Log committed under evidence/own-gpu-driver/rung2/, pass or fail.

## Later windows (each needs its own prepared step and its own request)
1. GSP boot through FSP from aien-gpu (L192 builder + the MMIO sequence, not written yet).
2. The RPC subset from docs/numeric/DRIVER_FLOOR.md and one channel (gpu/contract/gb10_chan.devc).
3. Omega on aien-gpu through L202's typed handles: golden tokens identical to nvidia.ko, per-word timing and start time side by side.
