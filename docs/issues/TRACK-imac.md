Title: Track: AienOS on the 2017 iMac (all devices, model run, network role)

Map: `docs/IMAC_AIENOS_MAP.md` (L210; pending when this body was drafted) and `docs/PROGRAM.md` section 2.2.

Goal: the iMac runs Aienux from its own disk with every device driven, ALLEN resident, and a stated role on the network. The iMac is the furthest machine on real hardware today.

Rungs:
- [x] 1 boot from the stick (hardware 2026-10-16, 10-17); [ ] boot from its own disk (#418, fix `1c91bb2` awaiting a physical run)
- [x] 2 CPU: cores, APIC timer (hardware)
- [x] 3 memory protection: W^X (hardware); [ ] VT-d (#411)
- [x] 4 disk: AHCI and NVMe read-only (hardware); [ ] write path and Store
- [x] 5 network: BCM57766 transmit and report (hardware); [ ] receive, live kernel swap, signed console (L142)
- [ ] 6 input: USB keyboard (#415)
- [x] 7 display: 4K framebuffer, ALLEN window (hardware)
- [ ] 8 trust: UNKNOWN until the map answers (TPM likely absent, not checked)
- [ ] 9 GPU firmware: Polaris driver on the chip (L143, L187)
- [ ] 10 GPU compute: GCN 4 kernels on the chip (L144, L145 are host only)
- [ ] 11 model run: native small model on the CPU (pending map)
- [ ] 12 service API: ALLEN through the Spark's model works in QEMU (L039); [ ] on the signed transport

Out of scope and on hold: firmware replacement (blocked twice by the automated cyber safeguard, do not retry).
