Title: Track: AienOS on the DGX Spark (Linux off, model served natively)

Map: `docs/SPARK_REPLACE_LINUX.md` (L191) and the whole-program page `docs/PROGRAM.md` section 2.1.

Goal: the Spark boots Aienux from its own disk and serves the 27B model with no Linux under it. Everything that needs Linux programs (agents, git, compilers, Docker, Tailscale, the judge) moves to another machine first.

Rungs (each closes only with its gate, on the real Spark unless stated):
- [ ] 1 boot: attended boots 1 to 3, `M0_ROLLBACK` for the C loader (L197)
- [ ] 1 boot: installer and A/B slots, `DISK_LAYOUT` (unowned)
- [ ] 2 CPU: `SMP` physical, power and thermal (L201, unowned)
- [ ] 2 CPU: clock, RTC, watchdog (L200)
- [ ] 3 memory protection: SMMU Spark-shape case (L201); GB-scale memory (unowned)
- [ ] 4 disk: NVMe queue depth and direct DMA (L199); data off ext4 (unowned)
- [ ] 5 network: RTL8127 driver and UDP report (L198); ConnectX present? (L204); Wi-Fi (L206); Bluetooth and audio (L207)
- [ ] 6 input: xHCI keyboard (unowned); serial (L204)
- [ ] 7 display: console on the HDMI screen (L197 attended boot)
- [ ] 8 trust: TPM2 CRB (L200); owner keys, Secure Boot on (Drake)
- [ ] 9 GPU firmware: GSP boot (L192); kernel owns BAR0 (unowned); GPU interrupts (unowned)
- [ ] 10 GPU compute: GPU MMU and channels (L193); Omega cut A (L202); Omega cut E (unowned)
- [ ] 11 model run: native CPU inference; 27B token stream (unowned)
- [ ] 12 service API: gateway and request path (unowned)
- [ ] Linux off: services moved (L203), cutover, soak, wipe last

Decisions waiting on Drake: L203 (where build, judge, git and agents live), L204 (look at the unit).

Status vocabulary: PASS, FAIL, NOT_RUN, BLOCKED_HARDWARE, BLOCKED_OPERATOR, MISSING_IMPLEMENTATION. Every claim is tagged OBSERVED, INFERRED or UNKNOWN.
