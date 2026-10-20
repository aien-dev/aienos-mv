Title: Track: AienOS on the 2017 iMac (all devices, model run, network role)

Map: `docs/IMAC_AIENOS_MAP.md` (L210) and `docs/PROGRAM.md` section 2.2. The iMac is the furthest machine on real hardware. Most of its work is in open pull requests (#412, #426, #414, #415, #418, #421, #424); `main` holds #404 to #408. Role on the network: a leaf and a test bench for the AMD GPU path and ALLEN's display, not the big-model machine.

Rungs (row ids IM1 to IM28 are in the map):
- [x] 1 boot from a stick (hardware 2026-10-16, 10-17); [x] signed netboot fetch (hardware 10-17, #414); [ ] own-disk boot with no stick (#418, second power-on result unknown)
- [x] 2 CPU: cores, APIC timer (hardware); [ ] idle power and RAPL watts on hardware (#424); [ ] RTC
- [x] 3 memory protection: W^X (hardware, #426 open); [ ] VT-d DMA confinement (IM4, unowned)
- [x] 4 disk: SATA and NVMe read-only (hardware); [ ] write path and Store (unowned)
- [x] 5 network: report send (hardware); [ ] receive in the default image; [ ] Wi-Fi, Bluetooth, Thunderbolt, SD (unowned, ids UNKNOWN until a read-only lab boot)
- [x] 6 input: keyboard worked on hardware (#415); [ ] replug fix on hardware (#421)
- [x] 7 display: CPU-drawn 4K, ALLEN window, sound (hardware); [ ] native 5K and GPU-drawn frame (L143)
- [ ] 8 trust: TPM or secure element UNKNOWN
- [ ] 9 GPU firmware: Polaris 10 (`1002:67df`) driver on the chip; chip naming fix L213; AMD blobs need Drake's go
- [ ] 10 GPU compute: GCN 4 encoder byte-exact on host (L144, L145); [ ] on the chip
- [ ] 11 model run: x86 AVX2 inference path (IM26), none today
- [x] 12 service API: ALLEN answers through the Spark's model over UDP (L039); native role is a leaf
- [ ] Fan and temperature question (IM20), listening test by Drake

On hold, do not retry: firmware replacement (IM24).
