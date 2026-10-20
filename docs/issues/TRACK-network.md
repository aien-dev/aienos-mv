Title: Track: the network between AienOS machines (signed transport, roster, gateway, Tailscale replacement)

Map: `docs/PROGRAM.md` section 4.

Today: UDP stack, reliable channel and a signed, encrypted transport exist and pass in QEMU with TEST keys and a roster of one. Drivers: Pi and iMac can send, the Spark has none (L198).

To do:
- [ ] roster of N peers: add, revoke, rotate (unowned)
- [ ] production identity, TEST keys refused (unowned, needs TRUST-1)
- [ ] gateway: HTTP to signed transport, and who hosts it (L203)
- [ ] network time over the transport (unowned)
- [ ] signed control console on every machine (unowned)
- [ ] off-LAN reach: relay or gateway-held Tailscale (decision, Drake)
