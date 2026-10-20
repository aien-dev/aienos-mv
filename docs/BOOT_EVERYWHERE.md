# The boot sequence everywhere (L266)

What a person sees and hears when AienOS starts, on every machine: the night sky, two black holes circling and
merging, a short rising chirp at the merger, then Allen. One module, the same timing, the same sound. This page
says where it runs today, how fast, and what is still missing. Every number carries its command and exit code;
claims are tagged OBSERVED (seen in a run), INFERRED (follows from code or a run, not seen directly) or UNKNOWN.

## 1. One module

| Part | File | Source |
|---|---|---|
| The night (stars, Milky Way, galaxies, the two black holes, the ripple) | `native/kernel/ui/night.c` | byte-identical to the iMac build `0930240` |
| Allen's home on the screen (draws the night, then Allen) | `native/kernel/ui/allen_ui.c`, `mind.c`, `paint.c`, `allen_font.c` | same |
| The chirp | `native/kernel/ui/chirp.c` | same |
| Frame receipt and the night's clock for the sound | `native/kernel/ui/boot_show.c` | new (L266) |
| aarch64 runner (Spark, Pi 5, the aarch64 VM) | `native/kernel/core/boot_screen.c` | new |
| x86 runner (iMac, PC, the x86 VM) | `native/kernel/x86/boot_x86.c` | new |
| VM screen (virtio-gpu 2D) | `native/kernel/dev/virtio_gpu.c` | new |
| VM speakers (virtio-sound) | `native/kernel/dev/virtio_snd.c` | new, shared by both kernels |
| Request core and the aarch64 PCI binding | `native/kernel/dev/virtio_q.c`, `virtio_bind.c` | new |

Timing lives in two headers and nowhere else: `night.h` (`CK_NIGHT_*`: stars out by 6.8 s, holes at 2.6 s,
spiral from 6.0 s, merger at 12.0 s, gone at 15.2 s) and `chirp.h` (`CK_CHIRP_*`: 600 ms sweep from 200 Hz to
1000 Hz ending at the merger, 300 ms ring).

**The sound file and its licence.** There is no sound file. The chirp is computed at boot from the shape of a
real gravitational-wave chirp (pitch rising as t^(-3/8), loudness as pitch^(2/3)), integer math, in
`ui/chirp.c`. No recorded audio and no third-party material: its licence is this repository's. The same
formula runs on every machine, so every machine plays the same samples (checked below: the sound a VM played
matches the formula sample for sample).

**How every image builds it in.** The ui files are compiled into every full aarch64 image and, with
`CK_BOOT_SHOW=1`, into the x86 image. The night plays when the image is built with `CK_BOOT_SHOW=1`:
`make -C native/kernel full CK_BOOT_SHOW=1` (aarch64, `$(OUT)/full-boot-show/BOOTAA64.EFI`) and
`make -C native/kernel imac-kernel CK_BOOT_SHOW=1` (x86, `$(OUT)/IMACKERN-BOOT-SHOW.EFI`). The default stays
0 so the 32 emulator gates that use a screen keep their timing. The PC images (L260) and the aienos-run images
(L261, L262, L265) should build with `CK_BOOT_SHOW=1` (ledger notes sent).

**What the runner (aienos-run) must give the VM** so its window shows the real boot screen, not a web
imitation:

| | aarch64 VM | x86_64 VM |
|---|---|---|
| Screen | `-device virtio-gpu-pci,disable-legacy=on,iommu_platform=on` | `-device virtio-vga` |
| Speakers | `-device virtio-sound-pci,audiodev=...,disable-legacy=on,iommu_platform=on` | `-device virtio-sound-pci,audiodev=...` |
| IOMMU | `-M virt,iommu=smmuv3` (the kernel refuses device DMA it cannot confine) | none (that kernel has no IOMMU support yet; it says so) |
| Window | QEMU's own display (`-display cocoa`/`gtk`/`sdl`) or VNC of that screen | same |

A runner with its own virtual machine monitor (L262 KVM, L265 Virtualization.framework) needs a virtio-gpu and
a virtio-sound device model with the same commands (section 3 lists exactly which).

## 2. Measured

RESULTS PENDING (filled from the gate runs below).

## 3. What the drivers use

virtio-gpu (virtio 1.x 5.7): GET_DISPLAY_INFO, RESOURCE_CREATE_2D (B8G8R8X8), RESOURCE_ATTACH_BACKING (one
entry), SET_SCANOUT, then per frame TRANSFER_TO_HOST_2D and RESOURCE_FLUSH of the whole picture. virtio-sound
(5.14): PCM_INFO, PCM_SET_PARAMS (stereo, S16, 48 kHz, 100 ms periods, 400 ms buffer), PCM_PREPARE,
PCM_START 150 ms before the chirp comes in, 100 ms transfers fed from the night's clock, PCM_STOP and
PCM_RELEASE after 100 ms of quiet. Every command layout is checked against the virtio spec's Linux UAPI copy
(`scripts/ck_virtio_av_test.sh`).

## 4. The GPU path

Today the night is drawn by the CPU: every pixel worked out from its position and the time, integer math,
in strips (the iMac kernel splits a frame across its cores). What drawing it on a graphics chip would need:

1. **A way to run work on the chip.** On the iMac's AMD Radeon Pro 580 the branch `feat/imac-gpu-shader`
   (5b56323) has a GCN instruction encoder, PM4 command packets and four compute kernels (fill, blit, star
   splat, and `bh_bend.c`, the black holes' light bending written to match `night.c` exactly), checked on a
   host GCN interpreter only; nothing has run on the chip yet (INFERRED from its commit message and tests).
   Missing: the ring buffer and doorbell bring-up, memory the chip can see, firmware loading for the compute
   engine. On the Spark's GB10 the same work is the L198 to L207 driver lanes plus the GB10 launcher.
2. **The split.** The stars, galaxies and Milky Way are the base layer and change slowly; the costly part per
   frame is the light bending around the two holes (each pixel near a hole is moved by the deflection
   formula) and the ripple ring. Those two map one pixel to one GPU thread with no communication between
   pixels: the shape a GPU is built for. The CPU keeps the clock, the sound and the text.
3. **The VM.** Inside aienos-run the guest has no real GPU. A virtio-gpu with 3D (virgl or Venus) would let
   the guest hand shaders to the host's GPU; that is a separate driver (virtio-gpu 3D contexts) and is not
   started.
4. **Is it needed?** On the iMac the CPU path already holds 60 Hz on average at 3840x2160 (section 2). A GPU path
   is worth it for 5K panels, for doing more each frame, and for battery; it is not needed for the boot as it
   is today.
