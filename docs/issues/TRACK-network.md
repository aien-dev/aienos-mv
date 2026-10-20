Title: Track: the network between AienOS machines (signed transport, roster, gateway, Tailscale replacement)

Map: `docs/PROGRAM.md` section 4. Today: UDP stack, reliable channel and a signed, encrypted transport pass in QEMU with TEST keys and a roster of one. The Pi hub runs Headscale and the tailnet; AienOS has no TCP or TLS. The Pi hub stays the Linux gateway until a second always-on Linux machine exists (decision L203).

- [ ] roster of N peers: add, revoke, rotate (unowned)
- [ ] production identity, TEST keys refused (unowned, needs TRUST-1)
- [ ] gateway: HTTP to signed transport (unowned)
- [ ] TCP, then TLS 1.3 with vetted primitives, then HTTP (unowned)
- [ ] near term: hub advertises the LAN so Linux machines reach AienOS machines by LAN address; later: native tailnet node or a written decision that this is permanent
- [ ] first native hub service: DNS over UDP on the Pi
- [ ] network time over the transport
- [ ] signed control console on every machine (pattern: iMac port 47035)
- [ ] off-LAN reach: relay or hub-held Tailscale (Drake)
