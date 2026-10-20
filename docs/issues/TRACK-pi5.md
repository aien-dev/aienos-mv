Title: Track: AienOS on the Raspberry Pi 5 (hub) and its role as gateway

Map: `docs/PI5_AIENOS_MAP.md` (L210) and `docs/PROGRAM.md` section 2.3. The Pi is also the always-on Linux hub (Headscale, Pi-hole, nginx, Forgejo, vault, bots). Recommendation: it stays the Linux hub and gateway until a second always-on Linux machine exists; native runs are short windows because a window takes the tailnet coordination down. Decision: L203.

Rungs (row ids PI1 to PI21, PI-M1 to PI-M4 are in the map):
- [x] 1 boot, 2 CPU watchdog, 4 disk (microSD), sealed Store: hardware; [ ] whether cores 2 to 4 start (lab image)
- [ ] 2 clock RTC, fan and thermal rule (unowned)
- [ ] 3 DMA confinement: confinement A BLOCKED, B inferred only (no IOMMU on the Pi)
- [ ] 5 network: send proven (loopback PASS twice, wire TX PASS); receive not seen in 400 ms; Wi-Fi, Bluetooth none
- [ ] 6 input: UART input and RP1 xHCI keyboard so a person can type the unlock secret
- [ ] 7 display: HDMI native (docs to be searched)
- [ ] 8 trust: no TPM; qualification-tier unlock proven; production tier open
- [ ] 9 and 10 GPU: VideoCore not planned
- [x] 11 model run: Llama-3.2-1B Q4_K_M at 1.20 tok/s on hardware; [ ] multi-core run; [ ] measured memory ceiling (the 21.13 tok/s figure is from an unverified vendor number)
- [x] 12 service API: signed units, agents, ALLEN REFLEX on hardware
- [ ] first native hub service: DNS over UDP
- [ ] Linux off: last, after the gateway has another home
