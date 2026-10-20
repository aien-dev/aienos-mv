# DGX Spark Bluetooth (USB) and HD Audio drivers for the C kernel (L207)

Branch `feat/spark-bt-hda`, base `origin/main` 18e2765. Kernel = Aienux, OS = AienOS.
Scope: two spec-defined drivers inside the existing device and DMA-fence model. No
reboot, no boot-order change, no firmware load and no live device access beyond
read-only inventory were used to produce this document. QEMU is not hardware: every
PASS below is QEMU or a host simulator, and every hardware row is NOT_RUN.

Tags: OBSERVED (a command on this machine showed it), INFERRED (follows from a spec or
from OBSERVED facts, nothing ran), UNKNOWN (with the experiment that would resolve it).

## 1. What exists

| Part | Files | Proof |
|---|---|---|
| HD Audio controller driver: reset, CORB/RIRB, codec and widget walk, output path, one cyclic stream, integer sine | `native/kernel/dev/hda.[ch]` | host model `dev/tests/hda_test.c`, `make -f stage.mk hda-test`: 3153 checks, 4 mutants caught |
| HD Audio DMA fence (PCI class 0x040300 or ACPI `NVDA2014`; SMMU-confined or denied; release in NVMe halt order) | `dev/hda_fence.[ch]`, `dev/devices.c` | QEMU gate `scripts/qemu_ck_hda_test.sh` (rows H1-H11, `--mutation`) |
| Tone capture checker (measures QEMU's wav output from outside the guest) | `native/kernel/tools/ck_wav_check.c` | used by the HDA gate, row H8 |
| Bluetooth HCI codec and host state machine (Reset, Read Local Version, Read BD_ADDR, Set Event Mask, Inquiry, LE scan, pairing-less LE Create Connection, Disconnect, Command Complete/Status credit flow, LE Meta events) | `dev/bt_hci.[ch]` | host controller simulator `dev/tests/bt_test.c`, `make -f stage.mk bt-test`, 4 mutants caught |
| Bluetooth over USB: descriptor walk, event reassembly across 16-byte interrupt packets, Bulk-Only packets | `dev/usb_bt.[ch]` | `dev/tests/usb_bt_test.c` on the real Spark descriptor bytes, `make -f stage.mk usb-bt-test`, 3 mutants caught |
| Generic xHCI control, bulk and interrupt pipes (usb_kbd.c untouched) | `dev/xhci_pipe.[ch]` | QEMU gate `scripts/qemu_ck_bt_test.sh` (rows B1-B8, `--mutation`) |
| Bluetooth step inside the xHCI fence, after the keyboard phase | `dev/usb_bt_phase.[ch]`, `dev/xhci_fence.c` | same gate; default image links it as an empty function |

Build flags (all in `native/kernel/Makefile`):
`CK_HDA_TONE=1` (QEMU tone image), `CK_BT=1` (inventory + live HCI bring-up, not run on
the Spark, refused with `CK_HARDWARE_STAGING`), `CK_BT_DISCOVER=1` (also inquiry and LE
scan), `CK_BT_TEST=1` (QEMU transfer proof). The default image brings the HD Audio
controller up, lists the codecs and releases it (no tone), and sends nothing to any
Bluetooth adapter.

## 2. Hardware facts (this Spark, read-only, 2026-10-20)

| # | Fact | Tag | Source |
|---|---|---|---|
| F1 | Bluetooth is USB `13d3:3630` "MediaTek Inc." / "Wireless_Device", bcdDevice 0100, bus 11 port 2, high speed (480), device class ef/02/01 (composite, interface association) | OBSERVED | `/sys/bus/usb/devices/11-2/{idVendor,idProduct,manufacturer,product,bcdDevice}` |
| F2 | The 272 descriptor bytes (18 device + 254 configuration) are the fixture of `usb_bt_test.c`; copied with `head -c 272 /sys/bus/usb/devices/11-2/descriptors \| xxd -i`, the kernel's cached copy, so no request reached the adapter | OBSERVED | same file |
| F3 | Interface 0 (class e0/01/01): interrupt IN 0x81 (max packet 16, interval 1), bulk IN 0x82 (512), bulk OUT 0x02 (512). Interface 1 (e0/01/01), alternate settings 0-6: ISO IN 0x83 / ISO OUT 0x03 (SCO voice, not driven). Interface 2 (also class e0/01/01, alternate settings 0 and 1): interrupt IN 0x8a and interrupt OUT 0x0a (64 bytes alt 0, 512 alt 1). The driver picks the first e0/01/01 interface with an interrupt IN plus bulk IN and OUT, which is interface 0. (An earlier note called 0x8a/0x0a bulk; the descriptor says interrupt, attribute 0x03.) | OBSERVED | descriptor bytes above, parsed by `ck_usb_bt_parse` |
| F4 | The USB bus 11 (high speed) and 12 (super speed) belong to the ACPI platform xHCI `NVDA8000:04`; `NVDA8000` is in the fence's ACPI id list (`xhci_acpi_fmt.h`) | OBSERVED | `readlink -f /sys/bus/usb/devices/usb11`; source |
| F5 | The Bluetooth firmware file on disk is `/lib/firmware/mediatek/mt7925/BT_RAM_CODE_MT7925_1_1_hdr.bin.zst`, 446856 bytes compressed (sha256 `61f80fa8...16ec4`), 446831 bytes decompressed (sha256 `d6423564...f7b53`). Package `linux-firmware-mediatek` 20240318.git3b128b60-0ubuntu3.1. Its first 14 bytes are the ASCII build stamp `20260605184935`, then `ALPS`, then zeros and little-endian words (meaning of those words UNKNOWN). The bytes after the header are not text. | OBSERVED | `dpkg -S`, `zstd -dc`, `xxd`, `sha256sum` (decompressed copy in `/tmp` only, not committed) |
| F6 | HD Audio is ACPI `NVDA2014:00` (`\_SB_.HDA0`), MMIO 0x36078000, irq 246, Linux driver `hda-acpi`, ALSA card 0 "NVIDIA HDA Controller" | OBSERVED | `/proc/asound/cards`, sysfs |
| F7 | One codec, "Nvidia SoC 33 HDMI/DP", vendor 0x10de0033, subsystem 0x10de0101, revision 0x100100, AFG node 1. Pins 0x04-0x07 (wcaps 0x407381, pincap 0x09000094, default config 0x185600f0, connections 0x08-0x0b). Converters 0x08-0x0b (wcaps 0x62b1, rates 0x7f0, bits 0xe = 16/20/24-bit, formats PCM and AC3) | OBSERVED | `/proc/asound/card0/codec#0`; the model in `hda_test.c` copies these numbers |
| F8 | 16 ELD entries exist (4 pins x 4 device ids) and all 16 report `monitor_present 0`, `eld_valid 0`: no HDMI/DP sink is attached to this Spark right now | OBSERVED | `cat /proc/asound/card0/eld#*` |
| F9 | Only the HDMI/DP codec exists: there is no analogue line-out or speaker pin on this machine | OBSERVED | codec dump (F7) |

## 3. Claims and what each rests on

| # | Claim | Tag | Evidence or resolving experiment |
|---|---|---|---|
| C1 | HCI commands travel on endpoint 0 as class requests (0x20 device, bRequest 0, wValue 0, wIndex 0; 0x21 and wIndex = interface for a controller inside a composite device); events on the interrupt IN endpoint; ACL on bulk. Class e0/01/01 | INFERRED from Bluetooth Core 5.4 Vol 4 Part B 2.1-3.1 (docs-read recorded) | spec text quoted in the handoff |
| C2 | Opcodes, parameter layouts and event formats used by `bt_hci.c` match the spec | INFERRED from Vol 4 Part E 5.4, 7.1.1, 7.1.6, 7.3.1, 7.3.2, 7.4.1, 7.4.6, 7.7.2, 7.7.5, 7.7.14, 7.7.15, 7.7.65.1-2, 7.8.10-12 | docs-read recorded; simulator follows the same text, so a shared misreading would not be caught (UNKNOWN until a real controller answers) |
| C3 | The host state machine is correct against a spec-following controller: credit flow, Command Status vs Complete, de-duplication, malformed events dropped | OBSERVED (host) | `make -f stage.mk bt-test` and its 4 mutants |
| C4 | The descriptor walk finds the HCI interface and its three endpoints on the Spark adapter's real descriptors, and refuses truncated ones | OBSERVED (host) | `usb-bt-test` |
| C5 | Control, bulk (multi-packet, both directions) and interrupt transfers work through `xhci_pipe.c` on an emulated xHCI | OBSERVED (QEMU) once `qemu_ck_bt_test.sh` passes (section 6) | gate rows B1-B5 |
| C6 | The HD Audio driver follows the Intel HD Audio spec 1.0a for reset, rings, verbs, stream setup | INFERRED + host model; QEMU intel-hda + hda-output is the check against an independent implementation | gate rows H1-H9 (section 6) |
| C7 | A 1000 Hz tone reaches the QEMU codec | OBSERVED (QEMU wav capture, measured outside the guest) once the HDA gate passes | gate row H8 |
| U1 | Whether the MT7925 Bluetooth function answers HCI Reset / Read Local Version **before** its RAM code is loaded | UNKNOWN | Experiment: boot a `CK_BT=1` image on the Spark and read the `bt:` lines, or capture the first control transfers with usbmon while Linux probes. Both touch the live adapter, so they need an explicit go-ahead |
| U2 | How the RAM code is loaded (vendor-specific HCI commands, section framing of the file in F5) | UNKNOWN, DOCS SILENT: no MediaTek document is available locally. Read-only references: Linux `drivers/bluetooth/btusb.c` and `btmtk.c` (GPL-2.0, read for behaviour only, nothing copied). Firmware is not loaded and not shipped here | Experiment: usbmon capture of a Linux probe plus reading btmtk.c; then a clean-room description before any code |
| U3 | Whether the MT7925 wants the interface-addressed (0x21) or the device-addressed (0x20) command form | UNKNOWN. The spec says the device must accept both; the driver tries 0x21 first on composite devices and falls back once to 0x20 on a STALL | Experiment: U1's run, watching which form completes |
| U4 | What the Spark's HD Audio controller looks like before our reset (left running by firmware or Linux?) | UNKNOWN | Experiment: a read-only register snapshot taken by a `CK_HDA` image before its first write |
| U5 | What an HDMI/DP sink needs before audio flows: ELD, audio infoframe, channel allocation, N/CTS clock regeneration, possibly NVIDIA vendor verbs | UNKNOWN. F8 shows no sink is attached, so the real path cannot be exercised here at all | Experiment: attach a monitor, read `eld#*` (read-only) to see the sink's ELD, then diff Linux's verb log (`snd_hda_intel` verbose or `hda-verb`) against ours |
| U6 | Devices behind USB hubs are not reached by `xhci_pipe` (no hub driver). The Spark adapter is on a root port (F1, bus 11 port 2) | INFERRED OK for this adapter | none needed unless a hub appears |
| U7 | LE connect and scan against a real advertiser; Inquiry against a real BR/EDR device | UNKNOWN (simulator only) | needs U1, U2 first |
| U8 | SCO/ISO voice (interface 1 alternate settings) | UNKNOWN, not implemented | out of scope for the brief |
| U9 | IOMMU stream for the HD Audio controller on the Spark: whether the platform path `ck_dma_confine_named("NVDA2014")` finds an IORT mapping | UNKNOWN | the existing IORT decode (`docs/GB10_IORT_DECODE.md`) lists xHCI and NVMe streams; check it for NVDA2014 |

## 4. MediaTek firmware inventory (no firmware load)

| Item | Value |
|---|---|
| File | `BT_RAM_CODE_MT7925_1_1_hdr.bin.zst` under `/lib/firmware/mediatek/mt7925/` (siblings: two Wi-Fi files, out of scope) |
| Size | 446856 bytes compressed, 446831 bytes after `zstd -dc` |
| Package | `linux-firmware-mediatek` 20240318.git3b128b60-0ubuntu3.1 (Ubuntu Kernel Team) |
| Header | build stamp `20260605184935`, `ALPS`, then little-endian words |
| Licence (from `/usr/share/doc/linux-firmware-mediatek/copyright`, stanza `Files: mediatek/*`, `Copyright: MediaTek Inc.`, `License: Redistributable-mediatek`) | "MediaTek Inc. grants permission to use and redistribute aforementioned firmware files for the use with devices containing MediaTek chipsets, but not as part of the Linux kernel or in any other form which would require these files themselves to be covered by the terms of the GNU General Public License or the GNU Lesser General Public License." Provided WITHOUT ANY WARRANTY. |
| Reading of that licence | INFERRED: redistribution is allowed for use with MediaTek devices and must not be combined so that the file itself falls under the GPL/LGPL. It is not an open licence: this repository must not contain the file; the install path is a separate-file load at boot from a partition, never a build input. The upstream `WHENCE` file was not on this machine, so the package copyright is the only licence text read |
| Loaded by this work | no. No byte of the file is read by the driver, shipped or embedded |
| Load protocol | UNKNOWN (U2) |
| Whether HCI works without it | UNKNOWN (U1) |

## 5. Reuse search (what existed, what was reused)

- `gh search code --owner aien-dev|aien-spark` and `grep -ril` over `~/workspace` main clones: no Bluetooth code anywhere; no MediaTek BT firmware parser (a Wi-Fi MT7925 firmware parser lives in another lane's worktree, `aienos-spark-rtl8127-net`, `native/net`: Wi-Fi files only, not touched).
- HD Audio: only `native/kernel/x86/hda_x86.c` (iMac Cirrus codec on x86, other branches). It targets a different codec and the x86 DMA model, so only its register constants were cross-checked; the controller driver here is generic and fenced.
- `origin/feat/85-audio-output` is a USB audio lane (different device class).
- Reused unchanged: `xhci_fence.c` (fence, ACPI platform discovery, `ck_dma_confine_named`), `usb_hid.c` (boot keyboard descriptor finder used by the transfer proof), `stage.mk` test pattern, `lib_gate_hold.sh` and `lib_ck_m1_checks.sh` for the QEMU gates.
- Why `usb_kbd.c` was not extended: its xHCI code is global-state and keyboard-shaped (one interrupt endpoint, fixed pages). `xhci_pipe.c` copies its ring, event and context code in the same style as an instance with control, bulk and interrupt, and leaves the keyboard driver (and the keyboard gate) untouched.

## 6. Gate results

Filled in by the runs recorded in `~/handoffs/2026-10-20-spark-bt-hda.md`. Rows:

HD Audio (`scripts/qemu_ck_hda_test.sh`): H1 controller found, H2 bus master off before the gate, H3 SMMU-confined grant only, H4 controller up, H5 codec and widgets listed, H6 output path chosen, H7 stream ran for 1.5 s, H8 captured wav is a 1000 Hz tone at peak 6000 (+-10 %) with equal channels, H9 release order (stream stopped < bus master revoked < read-back < stream abort), H10 final report and M1 checks, H11 fail-closed without an SMMU.

Bluetooth transport (`scripts/qemu_ck_bt_test.sh`): B1 pipes start, B2 bulk Bulk-Only INQUIRY, B3 4096-byte bulk IN against a pattern, B4 bulk OUT round trip, B5 interrupt IN report from a typed key, B6 "no Bluetooth controller" line, B7 release order and final report, B8 default image has no Bluetooth code.

## 7. Limits

- No Bluetooth controller exists in this QEMU (8.2.2 has no `usb-bt-dongle`), so no HCI byte crossed USB in any test. HCI is proven against a simulator written from the same spec text.
- The sine tone cannot be heard or captured on the Spark: no sink is attached (F8) and the HDMI audio path needs more than a stream (U5).
- Nothing here touched the Spark's Bluetooth or HD Audio hardware.
