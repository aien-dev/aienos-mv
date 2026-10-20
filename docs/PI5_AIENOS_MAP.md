# AienOS on the Raspberry Pi 5 ("hub"): gap map and ordered path (L210)

Status: read-only. Sources are the repo at `18e2765` (origin/main), `docs/native/PI5-*.md`, `native/kernel/dev/pi5_*`, `native/kernel/core/pi5*.c`, the Pi handoffs in `~/handoffs`, and a read-only look at the live Pi over ssh on 2026-10-20 (alias `hub`). Nothing was written to the Pi, no service was touched, no reboot, no boot-order or firmware change. Every claim is tagged OBSERVED, INFERRED (with confidence) or UNKNOWN (with the experiment that resolves it).

Names (Drake, 2026-10-20): Aienux is the kernel, AienOS is the system. This follows `docs/SPARK_REPLACE_LINUX.md` (L191) in layout; the iMac twin is `docs/IMAC_AIENOS_MAP.md`.

## 1. Plain-English summary

The Pi is two things at once. It is a small test computer where Aienux has run on real hardware and answered real questions: it reads its memory card, keeps a locked-and-signed store that survives power cuts, unlocks it with a recovery secret, runs a 1-billion-parameter model at 1.20 tokens per second (a token is roughly a word piece), loads signed programs, and sends one network frame. And it is the always-on machine ("hub") that keeps the private network running: the Headscale coordination service, the DNS filter, the web front door, a second git server, and the password vault.

Those two jobs cannot both be done by Aienux today. Aienux has no TCP, no TLS and no way to run Headscale. Every native test so far is a temporary window: the Pi boots Aienux once from its memory card, then goes back to Linux (OBSERVED, handoffs: "hub Linux is down then"). While a window is open the tailnet's coordination service is down.

"Everything" on the Pi means: finish the board's devices (USB, display, Wi-Fi, Bluetooth, GPIO, clock, fan), make the model run faster and use all four cores, and decide where the hub's network services live once the Pi is itself running Aienux. Section 6 gives the order.

## 2. The board, exactly

OBSERVED over ssh on 2026-10-20 (raw output and exit codes in `~/handoffs/imac-pi-inventory-20261020/`):

| Part | Value |
| --- | --- |
| Board | `Raspberry Pi 5 Model B Rev 1.0`, device tree `raspberrypi,5-model-b brcm,bcm2712`, serial present (not recorded here) |
| CPU | 4 cores, Arm implementer `0x41` part `0xd0b` (Cortex-A76), features include `asimddp` (dot product) and `fphp`; cpufreq 2.3 GHz now, 2.4 GHz max |
| Memory | 8,118,184 kB total |
| PCIe | `0002:00:00.0` Broadcom BCM2712 PCIe bridge `14e4:2712`; `0002:01:00.0` RP1 south bridge `1de4:0001` (shows as an Ethernet class). Nothing else on PCIe: the external x1 slot is empty or unlit |
| Storage | microSD `mmcblk0` 119.4 GB with three partitions: p1 512 MB, p2 4 GB, p3 784 MB (the AienOS card layout in `PI5-6-INFER-RUNS.md`); USB stick `sdb` 28.9 GB holding the live Linux root and `/boot/firmware`; second USB stick `sda` 960 MB |
| Ethernet | `eth0` up, 192.168.1.192/24 (RP1 Gigabit MAC with a BCM54213PE PHY, per the native runs) |
| Wi-Fi | `wlan0` (driver `brcmfmac`, SDIO host at `/axi/mmc@1100000`, compatible `brcm,bcm4329-fmac`), down, no carrier |
| Bluetooth | `hci0` present |
| Display | `card1-HDMI-A-1`, `card1-HDMI-A-2` both disconnected |
| Other device nodes | `gpiochip0`, `gpiochip10` to `13`, `i2c-1`, `i2c-13`, `i2c-14`, `spidev0.0`, `0.1`, `10.0`, `rtc0`, `hwrng`, `dri`, `video19` to `video28`, `ttyAMA0`, `ttyAMA10` |
| USB hosts | four buses on xhci (RP1) and one `dwc2` |
| Sensors | hwmon `cpu_thermal`, `rpi_volt`, `rp1_adc`, `pwmfan` (a fan1_input exists); CPU temperature 56.75 C when read |
| Tailnet | `tailscale0` 100.64.0.3; `wg-atlas` WireGuard 10.88.0.2/24; Docker bridges |
| Load | load average 6.78 on four cores at the time of reading |

Command exit codes (from `exits.txt`): `cat /proc/cpuinfo` 0, `lspci -nn` 0, `ip -br link` 0; `lsusb` and `lsblk` timed out at 25 s on the first try (124) while the Pi was loaded, then returned in 1 s each when retried with a 100 s limit (0); a single large command that ended in `tailscale status` hung for 100 s (124) with its other output intact; `tailscale status --peers=false` returned in 1 s. Cause of the stalls: UNKNOWN (load, ssh connection throttling and the peer listing in `tailscale status` are all candidates; not separated). `headscale nodes list` failed with `permission denied` reading `/etc/headscale/config.yaml` as user `drake` (exit 0 of the wrapper, command itself failed), so the tailnet peer list was not read.

### What the hub runs (OBSERVED, `systemctl` and `docker ps` and `ss -tulnH`)

Running services: `headscale`, `tailscaled`, `nginx` (80, 443), `pihole-FTL` (DNS 53), `docker` with container `atlas-forgejo` (127.0.0.1:3010 and :2222), `vault` (8200), `fail2ban`, `chrony`, `ssh`, `fwupd`, `snapd`, `ModemManager`, `cron`, `aien-shell`, `atlas-chat-router`, `atlas-dsh-tunnel`, `atlas-kvm8-pi-reverse-tunnel`, `readings-window`, five Discord bot services (`archer`, `ava`, `ryan`, `soren`, `atlas-librarian`), and a login getty on the Pi's serial port. Listening: 3478/udp (the relay and discovery port Headscale's DERP uses), 41641/udp (Tailscale), 100.64.0.3:8443, 9443, and several localhost-only ports.

## 3. What Aienux has on the Pi, with where it was proven

All "real Pi" rows are runs of the same physical board, booted one-time from the card with Linux returning afterwards. Evidence: `docs/native/PI5-*-RUNS.md`, `evidence/pi5_hardware_runs_2026-10-13/`, `scripts/pi5_receipts_verdicts.tsv` (72 rows). "Tier" in those files is QUALIFICATION with TEST keys unless said otherwise.

| # | Device | Aienux today | Missing | Reference (licence) | Host-only now? |
| --- | --- | --- | --- | --- | --- |
| PI1 | Firmware handoff, one-time boot | UEFI stub image `BOOTAA64.EFI` per image variant (`make pi5`, `pi5-store`, `pi5-unlock`, `pi5-infer`, `pi5-agent`, `pi5-lifecycle`, `pi5-artifact`, `pi5-pcie`, `pi5-net`; eleven images). Gate `scripts/ck_pi5_build_check.sh` (`CK_PI5_BUILD: PASS`). Real Pi: boots by tryboot, returns to Linux. 11 images build byte-identical twice (`ck_pi5_repro_check.sh`, PI5-8 Q1) | Boot without Linux ever involved: an installer and a first-boot path not depending on the USB stick | Raspberry Pi firmware and EEPROM bootloader documentation (public) | Yes for build; boot is attended |
| PI2 | Cores, interrupts, timer | GICv2 (GIC-400) chosen from the MADT (README); timer; EL2 to EL1. Real Pi: every run | Whether the other three cores are started on the Pi image: UNKNOWN (no Pi log line about it read). Resolve by one lab image that prints started cores | Arm Architecture Reference Manual; BCM2712 documents are not public in full: record DOCS SILENT per register block, quote Raspberry Pi's published parts | Yes in QEMU, then one attended run |
| PI3 | microSD (boot card, Store, model partition) | In-kernel SD host driver with PIO (`pi5_sd` in `core/pi5.c`): card ready, 4-bit, 25 MHz, read sector 0, GPT, Store view. Real Pi: `pi5-sd1a/b`, `pi5-store1..4` | Speed (25 MHz; model is read by the firmware instead, see PI-M1); DMA; write endurance policy | SD Host Controller spec (public), Linux sdhci (GPL) read-only | Partly |
| PI4 | Sealed Store on the card | Store v1 + M5 sealed envelopes; formats, reopens, bumps generation across boots. Real Pi: runs `pi5-store1..4`, generation=22 seen in NET-3 | Mid-commit power cut on the real card: QEMU only (Q7 limit in the ledger) | none | Yes |
| PI5 | Unlock with the recovery secret | `make pi5-unlock`, slot 1 recovery-secret path, wrong secret refused (`RecoverySecretRejected`), corrupt refused. Real Pi: unlock runs A to E | The secret arrives from a labelled card file in tests. No keyboard and no UART input on the Pi image, so a person cannot type it (OBSERVED, README and ledger). Needs USB keyboard (RP1 xHCI) or UART input | none | Yes after PI6 |
| PI6 | USB (RP1 xHCI) | **None.** Ledger: "USB host (xHCI) lane after GEM" | The whole lane: RP1 PCIe bring-up beyond the GEM, xHCI via RP1 BAR, keyboard class (`dev/usb_kbd.c`, `usb_hid.c` and `xhci_fence.c` exist for the Spark and iMac and are shared candidates) | RP1 datasheet (public, Raspberry Pi) cited in `pi5_gem.c`; Linux `xhci` (GPL) read-only | Model yes, chip no |
| PI7 | PCIe root complex and RP1 | `dev/pi5_pcie.c` (413 lines): finds `14e4:2712` and `1de4:0001`, programs inbound and outbound windows. Real Pi: `pi5-pcie` and NET-2 runs | Interrupts (polling only), other RP1 blocks | RP1 datasheet | Yes |
| PI8 | DMA confinement | ADR 0020 (`docs/adr/0020-pi5-software-only-dma-confinement.md`): bounce window of 2 MiB; hardware window `NOT_CONFINED` to `CONFINED` readback. Real Pi: NET-3 confinement A BLOCKED (canary 0 changed bytes); confinement B (read) INFERRED only | A wire capture settles B; the Pi has no IOMMU so this is weaker than the Spark (the run says so) | none | n/a |
| PI9 | Ethernet | `dev/pi5_gem.c` (1052 lines) + `pi5_net.c`: PHY link found (BCM54213PE addr 1, 1000 Mbit full), loopback PASS twice, one ARP frame TX PASS on the wire. Branch `pi5/net-398-wire` (link-drop steps, unmerged). host test `tests/test_pi5_gem.c` | **RX of an incoming frame: none seen in 400 ms.** No IP stack bring-up on the Pi image beyond one frame. TX reaching the partner is unproven (no capture) | RP1 datasheet; Linux `macb` (GPL) read-only | Model yes |
| PI10 | Random numbers | `dev/pi5_rng.c`: RNG200. Real Pi: `entropy: source=rng200` (pi5-rng1) | none | RP1/BCM2712 RNG200 docs | Done |
| PI11 | Watchdog | `dev/pi5_wdt.c`: PM watchdog, 15 s, petted at stages. Real Pi: every run; long jobs (SHA over 770 MiB) pet in pieces | Policy for long runs without code-level pet points | none | Done |
| PI12 | Serial console | UART output only (`core/pi5.c` comment: "reaches the screen and UART"). No UART input | UART input (needed by PI5 until USB keyboard lands); header cable is a physical step | BCM2712 UART documentation | Yes in QEMU |
| PI13 | Clock (RTC) | **None.** `rtc0` exists (OBSERVED) | Read and set the wall clock; battery connector state UNKNOWN | RP1 / PMIC documentation | Yes |
| PI14 | Fan, thermal, frequency | **None.** Firmware clocks are left as found (NET-3 line: `clk_eth: ALREADY_ON ... left as found`). Linux has `pwmfan`, `rpi_volt`, `cpu_thermal` | A temperature read and a fan rule; without them a long native run relies on firmware defaults. UNKNOWN whether the firmware keeps the fan under control when no OS runs. Resolve by a long run with a temperature read | Raspberry Pi documentation; Linux `pwm-fan` (GPL) read-only | Read path yes; answer needs hardware |
| PI15 | GPIO, I2C, SPI, ADC, PWM | **None** | The RP1 pin controller and the three buses. Needed for any board you wire up. The PHY reset uses GPIO 32 inside `pi5_net.c` as a one-off | RP1 datasheet | Yes (register model) |
| PI16 | HDMI (two ports) | **None.** The firmware framebuffer path prints console text where a screen is attached (INFERRED, medium, from the code comment) | Native display: Pi 5 uses its own display engine (HVS) and HDMI blocks; no public full documentation known to this map: DOCS to be searched, otherwise DOCS SILENT and Mesa `vc4` (MIT) as read-only reference | Mesa vc4 / Linux `vc4` | Model yes, chip no |
| PI17 | Wi-Fi | **None** | An SDIO host driver (a second controller, `mmc@1100000`, not the card's) plus Broadcom/Cypress firmware blobs. Licence of the firmware: not read. Linux `brcmfmac` (Dual BSD/GPL) is the read-only reference | brcmfmac | Model yes; firmware load needs Drake |
| PI18 | Bluetooth | **None** | UART transport + HCI + the same firmware family. UNKNOWN which UART (`ttyAMA10` is a candidate, not confirmed) | Bluetooth Core spec | Model yes |
| PI19 | Camera (CSI), display (DSI), video codec, VideoCore GPU (`dri`, `video19` to `28`) | **None** | Not on the critical path to a model run; listed so the map is complete | Mesa `v3d`, `vc4` (MIT) read-only | Later |
| PI20 | Real-time clock battery, PoE/power button, PCIe x1 slot, NVMe | **None**; slot empty on this unit (OBSERVED lspci) | Only if hardware is added | n/a | n/a |
| PI21 | Signed units and agents | EL0 artifact loader (`PI5-7-ARTIFACT-RUNS.md`), agent lifecycle (`PI5-7-LIFECYCLE-RUNS.md`), resident agent reply persisted in the sealed Store and recovered after reboot (`PI5-6-AGENT-RUNS.md`), Cortex memory survives cold boot (`PI5-5-CORTEX-RUNS.md`). Real Pi: all four | Production keys: the owner key ceremony was PERFORMED on 2026-10-14 (key in YubiKey PIV slot 9c; `PI5-8-OWNER-CEREMONY.md`) but the run tiers are still labelled QUALIFICATION with TEST keys | none | Done at that tier |

### Class M: the model run

| # | Item | Value |
| --- | --- | --- |
| PI-M1 | Measured | Llama-3.2-1B-Instruct Q4_K_M, 807,694,368 bytes, SDOT path (`PI5-6-INFER-RUNS.md`): `mean_tok_us=833129`, which is **1.20029 tokens/s** (calc:591b5dece54642aa0674f6f1024931b5516110a08ca5d5207e492da70f1fd399). Output equals the llama.cpp reference tokens ("The capital of France is Paris."). TinyLlama 1.1B Q4_K_M: 1.462 tok/s; h2o-danube3-500m: 3.227 tok/s (`PI5-8-MODEL-CHOICE.md`, one boot each). Model read through the firmware from a raw partition at 96.16 MB/s (807,694,368 B in 8.39966 s, calc:3cd091978f8c3653aa4788edb52c1c3e9ddca7d188f005ab047a4e87e2587970) |
| PI-M2 | Hardware ceiling | INFERRED from a vendor figure not in this repo (UNVERIFIED): the Pi 5 8 GB uses LPDDR4X-4267 on a 32-bit bus, 17,068 MB/s. A model whose weights are read once per token cannot exceed 17,068,000,000 / 807,694,368 = **21.13 tok/s** (calc:166ba5b3792e1780456161c95ebc868dd14cebf73c5cd20287c3fbc6db480ec8). The measured 1.20 is about 5.68% of that (calc:ae057014bbb594a6ccce93bb553428a2a43d92ad522a759635cb0d100847d6e5). The bandwidth figure must be measured on the board before it is used in a plan |
| PI-M3 | What bounds it today | UNKNOWN until profiled. Facts: the cut-3 run used one execution thread as far as the logs show (no multi-core line read: UNKNOWN); features seen: `dotprod=1 i8mm=0 bf16=0 fhm=0 fp16=1 sve=0` (OBSERVED, `PI5-6-INFER-RUNS.md`). Experiments: (a) a memory-copy loop for the real bandwidth; (b) per-layer timing in the existing session; (c) start the other three cores and split matrix rows |
| PI-M4 | GPU | The Pi 5 has a VideoCore GPU (`dri`, V3D). No Aienux driver, no plan in the repo. Mesa `v3d` (MIT) is the read-only reference. Whether it can beat four A76 cores on the same memory bus is UNKNOWN: it shares the 17 GB/s bus, so for a bandwidth-bound model the gain is bounded by the same ceiling. Not a priority |

## 4. The network role

Today (OBSERVED): the hub is the **tailnet coordination server (Headscale)**, a Tailscale node (100.64.0.3), the **DNS filter** (Pi-hole), the **web front door** (nginx), a **git server** (Forgejo in Docker), a **vault**, a WireGuard link to another site (`wg-atlas`), and the **Discord bots**. Its LAN address 192.168.1.192 is in the Spark map's tailnet list as a Linux peer.

Aienux side (OBSERVED in the repo): pairwise signed transport `native/net/aienos_sec.{h,c}` (SIGMA-style handshake, X25519, Ed25519, HKDF, AES-256-GCM-SIV; a pinned roster of one peer; TEST identities only), reliable channel `native/net/aienos_ctl.c`, UDP/IPv4/ARP/ICMP `aienos_net.c`. No TCP, no TLS, no DNS, no DHCP. The word "gateway" does not appear in `native/net`, `docs/adr` or `docs/*.md`.

What each hub service needs if the Pi itself runs Aienux (INFERRED, high, from the absence of TCP/TLS/HTTP in `native/`; "must move or be rebuilt natively"):

| Service | Why it needs more than Aienux has | Verdict |
| --- | --- | --- |
| Headscale (coordination for the tailnet) | A TCP/HTTPS server, TLS 1.3, HTTP/2, the control protocol (INFERRED from general knowledge of Tailscale, not read here), a database, and relay (DERP/STUN, 3478/udp) | **Must move to another Linux machine, or be rebuilt natively.** A native rebuild means TCP, TLS 1.3 and an HTTP server first; TLS and WireGuard primitives must come from vetted implementations, never hand-rolled (Drake's rule). It is a large build whose first step (a native TCP and TLS stack) is the same gate the Spark's model server needs. Rows are UNKNOWN, not rejected. Experiment: write the control-protocol spec down from Headscale's open source (licence not checked here) and count the parts that need TCP |
| Tailscale client (`tailscaled`) | WireGuard data plane plus the control protocol | Same as above for the client side; a LAN-side subnet router on the Linux hub is the near-term route for native machines (INFERRED, medium) |
| Pi-hole DNS | UDP DNS server + blocklists | Possible natively over UDP (no TCP needed for most queries); a good first native service. UNKNOWN: DNS over TCP for large replies |
| nginx front door | TCP/TLS | Moves with Headscale |
| Forgejo, Vault, bots, tunnels, WireGuard to `atlas` | Linux programs | Another machine |

Recommended role (INFERRED, medium): the Pi stays the **always-on Linux hub and gateway** until a second always-on Linux machine exists. Native Aienux on the Pi is a temporary window (as today), not the resident system. When Aienux gets TCP and TLS, move services onto it one at a time, DNS first. The reason is physical: an Aienux window takes down the tailnet coordination for its duration, and no second machine currently provides it. This also answers the mind-map idea "AIENOS on the iMac joins Tailscale" (2026-10-20): the near-term route is the Linux hub advertising the LAN so the iMac is reachable at its LAN address over the signed transport; a native tailnet node waits on TCP/TLS and a WireGuard implementation.

Gateway definition (needs Drake's decision; does not exist in code): a Linux machine that speaks HTTPS to clients and Aienux's signed UDP transport to the AienOS machines. Candidates: the hub (always on, already the tailnet anchor) or the MacBook (not always on). Roster gap: `aienos_sec` allows one peer; a gateway needs one peer per AienOS machine or a roster of many.

## 5. Ordered path

Notation as in the Spark map: **[H]** host or QEMU only, a builder can do it now; **[A]** needs an attended one-time boot of the Pi (Linux comes back); **[O]** Drake only.

1. [H] Land `pi5/net-398-wire` and the open Pi PRs (the desk owns PRs; #402 is open).
2. [A] One lab image that prints: how many cores start, a bandwidth loop (PI-M3 experiment a), the temperature and fan state (PI14), the RTC value (PI13). Read-only except the clock-free runs already done.
3. [H] UART input and a serial console shell on the Pi image; [O] a USB-serial cable (physical) removes the "labelled card file" from unlock (PI5, PI12).
4. [H] RP1 xHCI model and keyboard class reuse; [A] keyboard on hardware (PI6). This also unlocks the real-secret entry (PI5).
5. [H] Network receive on the GEM: find why no frame arrived in 400 ms (PI9). Then ARP reply, UDP echo, the iMac's report sender pattern (`x86/report_x86.c`) reused, and the signed transport to the Spark: first native-to-native signed datagram.
6. [H] Multi-core inference: split rows across cores, with a verification against the llama.cpp reference output (PI-M3 c); report speed against the measured ceiling.
7. [H] DNS server over UDP as the first native hub service (section 4).
8. [H][A] Wall clock (RTC), fan and thermal rule (PI13, PI14), so a native window can be long.
9. [H] Wi-Fi, Bluetooth, GPIO/I2C/SPI, HDMI models (PI15 to PI18), each with a documentation quote or a DOCS SILENT record before any chip code. Firmware load only with Drake's go.
10. [H] TCP then TLS 1.3 (shared with the Spark: its model-server gateway and git, ssh, TLS clients all need it). Then HTTP, then Headscale's control-protocol parts, one at a time.
11. [O] Decide the gateway machine and the second always-on machine (section 4). Until then the Pi's native windows stay short.

## 6. Operator steps (Drake only)

None of these is authorized by this document.

- Each native window needs the card's boot menu set for a one-time boot and Linux restored afterwards, as in `docs/native/PREFLIGHT-AND-RECOVERY.md`.
- A USB-serial cable from another machine to the Pi's UART header (PD2 in the roadmap ledger).
- A decision on which machine becomes the gateway and which becomes the second always-on Linux box.
- The same owner-key and release-key steps as the other machines (`PI5-8-OWNER-CEREMONY.md` records one ceremony).
- Allow any firmware load (Wi-Fi, Bluetooth) if those rows are built.

## 7. Limits of this document

- No test was run for this file. Gate names and results are quoted from `docs/native/PI5-*-RUNS.md` and the receipts table, which were not re-run.
- The ssh look was a single moment (2026-10-20 16:2x Z) on a loaded Pi. The `lsusb`/`lsblk`/`tailscale status` stalls have no confirmed cause. `headscale nodes list` failed on a file permission, so the peer list is unread.
- The memory bandwidth used for the ceiling is a vendor figure, not measured, and not from this repo. The percentage is therefore a bound, not a result.
- Pi-hole, nginx, Forgejo, Vault: only unit and container names and ports were read. No configuration, key or database was opened.
- Reuse search: `origin/main`, branches `pi5/*` and `allen-399-route`, `docs/native/PI5-*`, `scripts/pi5*`, `~/handoffs` (six Pi files), the mind map (`mm show` on the Tailscale idea), `org-check` not run. `docs/adr/0020` was named, not re-read in full.
- No time, effort or speed estimate appears here except those tied to a receipt id.
