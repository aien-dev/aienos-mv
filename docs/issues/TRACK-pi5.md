Title: Track: AienOS on the Raspberry Pi 5 (hub), and its role as gateway

Map: `docs/PI5_AIENOS_MAP.md` (L210; pending when this body was drafted) and `docs/PROGRAM.md` section 2.3.

Goal: the Pi runs Aienux with network receive and a production unlock tier. Open conflict: the Pi is also the always-on Linux hub that can serve as the gateway (L203). The Pi goes Linux-fully-off only after another gateway exists.

Rungs:
- [x] 1 boot, 2 CPU, 4 disk (microSD): hardware 2026-10-13
- [ ] 3 memory protection: UNKNOWN until the map answers
- [ ] 5 network: send proven (loopback PASS, wire TX PASS); receive not seen in 400 ms
- [ ] 6 input and 7 display: decide whether needed for a headless hub
- [ ] 8 trust: no TPM; qualification-tier unlock proven; production tier open; owner key signing on the Mac (PIN and touch)
- [ ] 9 and 10 GPU: no path; decide whether one is wanted
- [x] 11 model run: Llama-3.2-1B Q4_K_M at 1.20 tok/s (scalar 0.22); [ ] speed against the Pi memory ceiling (not measured)
- [x] 12 service API: ALLEN REFLEX and the agent image on hardware
- [ ] Linux off: after the gateway moves
