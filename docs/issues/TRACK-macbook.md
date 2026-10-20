Title: Track: AienOS on the MacBook Air M2 (Apple Silicon)

Map: `docs/MACBOOK_AIENOS_MAP.md` (L208) and `docs/PROGRAM.md` section 2.4. Nothing has run on this machine. Plan: tethered boot (the Spark sends the kernel over USB-C into RAM each time, nothing stored on the Mac), one-time recovery setup chosen by Drake. This Mac is the owner-key ceremony machine with Activation Lock on: Drake decides whether to use it or a spare Apple-silicon Mac. Reverse engineering of the GPU runs as L214.

Rungs (rows D1 to D20, G1 to G4, F1 to F8 are in the map):
- [ ] platform layer on host models: device-tree parser, raw Image entry, AIC2, RTKit client, DART and SART (D1 to D6)
- [ ] boot #1 proxy read-only; boot #2 `kernel: alive`; boot #3 framebuffer; boot #4 disk read-only
- [ ] ANS2 NVMe, SMC and RTC, MTP keyboard, USB-C and DWC3, USB Ethernet (D4, D7 to D10)
- [ ] trust: no TPM; SEP or owner-held counter UNKNOWN
- [ ] GPU: record, do not send; UAT; firmware structures; integer vector add (G1 to G4, L214); compute path is DOCS SILENT
- [ ] firmware blobs from macOS for Wi-Fi, Bluetooth, GPU: a firmware load, Drake's decision (F4)
- [ ] standalone boot only after a soak of tethered boots

Not a replacement for any Spark function.
