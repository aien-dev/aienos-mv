# AienOS program map: every machine, every rung, the network between them, and what comes after (L211)

Status: DRAFT 1, 2026-10-20. Written from the repo at `18e2765`, the ledger, the Spark map (`docs/SPARK_REPLACE_LINUX.md`, L191) and the hand-off notes. Three of the four machine maps this page depends on have not landed yet, so those rows are marked PROVISIONAL and the page says which map will replace them. Nothing here is a claim that a row works unless the "Where proven" column says so.

Names (Drake, 2026-10-20): **Aienux** is the kernel, **AienOS** is the system. Older files still say AIENOS for both.

Tags on every claim: OBSERVED (read from source, a record or a live box), INFERRED (with confidence), UNKNOWN (with the experiment that would settle it). Nothing unmeasured is called done. No time, effort or speed estimate appears on this page, because none has a receipt.

| Map | Machine | Ledger | State on 2026-10-20 |
| --- | --- | --- | --- |
| `docs/SPARK_REPLACE_LINUX.md` | DGX Spark (GB10, aarch64) | L191 | LANDED (READY `bb2399f`, branch `feat/spark-replace-linux-map`) |
| `docs/IMAC_AIENOS_MAP.md` | iMac 2017 (x86_64) | L210 | PENDING |
| `docs/PI5_AIENOS_MAP.md` | Raspberry Pi 5 (hub) | L210 | PENDING |
| `docs/MACBOOK_AIENOS_MAP.md` | MacBook Air (Apple Silicon) | L208 | PENDING |
| `docs/PC_AIENOS_MAP.md` | Ballentine_PC (Ryzen 5 7600X, RTX 5070, Windows 11) | L209 | PENDING |

## 1. The whole shape in plain English

AienOS is one operating system meant to run on five different computers. Each computer climbs the same twelve-step ladder from "the chip powers on" to "another machine can ask it for an answer". The lower rungs are drivers (disk, network, screen, keyboard). The upper rungs are the graphics chip and the AI model. A computer is "done" when every rung has been proven **on the real machine**, not only in the emulator (QEMU), and Linux (or macOS or Windows) can be switched off.

Where each machine stands today, from the records:

- **iMac 2017**: furthest on real hardware. Our kernel has booted on it, started its cores, turned on memory protection, read both disks, shown the 4K screen and sent reports over Ethernet. The graphics chip driver exists but has not run on the chip.
- **Raspberry Pi 5**: our kernel has booted on it, read and written its SD card, and run a small language model on its own processor at 1.20 tokens per second. Network send works, receive has not been seen.
- **DGX Spark**: our kernel booted once, for 75 seconds, in September, and only printed a report. Everything since is emulator work plus drivers in flight. The Spark is the machine that has to run the 27B model, so it carries the longest ladder (the GPU path).
- **MacBook Air and the PC**: no AienOS code has run on either. Their maps are being written.

Three things hold for the whole program:

1. **Some Linux programs will never run on AienOS.** Claude and Codex agents, git, compilers, Docker, Tailscale and the test judge are Linux programs with no native counterpart and no plan for one (Spark map, rows U1 to U6). "Linux off" on a machine means that machine runs only AienOS code. The work that needs those programs lives on a machine that keeps Linux. This is a physical boundary, not a gap to close.
2. **The network between machines has one real piece today**: a signed, encrypted transport (section 4). It is proven in the emulator with test keys only, and it knows exactly one peer. Both limits matter for "many machines".
3. **Omega is the destination language** (ADR 0024). Rust is scaffolding, C and assembly only where hardware forces them. The future section (section 6) says how the kernel itself moves to Omega.

## 2. The ladder and the tables

The twelve rungs, bottom to top: **1 boot, 2 CPU, 3 memory protection, 4 disk, 5 network, 6 input, 7 display, 8 trust (TPM or equivalent), 9 GPU firmware, 10 GPU compute, 11 model run, 12 service API.**

Columns: **What** is the rung, **Gate** is the named test that would prove it, **Proven where** is QEMU, HARDWARE (real machine) or NONE, **Ledger** is who owns the next step or `unowned`. Gate names come from `native/kernel/GATES.md` and the machine maps; `no gate yet` means the rung has no named test, which is itself a gap.

### 2.1 DGX Spark (GB10): source `docs/SPARK_REPLACE_LINUX.md` (L191, landed)

Pin: repo `18e2765`, live box `spark-b87b` read 2026-10-20. Row ids (D, G, U) point at that map's gap table.

| Rung | What | Gate | Proven where | Ledger |
| --- | --- | --- | --- | --- |
| 1 boot | C kernel started from firmware, device list matches the box, return to Linux unharmed (D1) | `AIENOS_CK_M1`, `M0_ROLLBACK` (NOT_RUN for the C loader); attended boot 1 | HARDWARE once (2026-09-24, handoff image, `M2_GATE: PASS`, 75 s); C kernel QEMU only | L197 (boot package, in flight); attended boot is Drake |
| 1 boot | Installer and rollback-safe A/B slots (path item 17) | `SLOT_LOADER` (N7), `DISK_LAYOUT` | QEMU for slots; `DISK_LAYOUT` NOT_RUN | unowned |
| 2 CPU | MMU, EL2 to EL1, GICv3, timer, threads, EL0, capabilities, IPC, 20 cores in two classes (D2) | `M1`, `M3`, `SMP` | QEMU; `SMP` NOT_RUN; physical NONE | L201 (baseline re-run) |
| 2 CPU | Power, thermal, fans, CPU frequency (D11) | no gate yet | NONE; UNKNOWN who holds fans if no OS does | unowned |
| 2 CPU | Clock, RTC, watchdog (D10) | TPM+RTC gate (L200) | QEMU timer and RNDR; RTC NONE | L200 |
| 3 memory protection | Two SMMUv3, DMA confinement, two-level stream table (D3) | `SMMU`, `NVME_SHUTDOWN` | QEMU PASS; real Spark IORT shape host case owed | L201 |
| 3 memory protection | GB-scale pinned regions, huge pages, 27B weight image (G5); the DMA pool is 8 MiB today | no gate yet | NONE | unowned |
| 4 disk | NVMe `144d:a810`, 512-byte blocks: read, write, flush; queue depth above 1, direct DMA (D4) | `QEMU_NVME`, `QEMU_NVME_RW`, `M4_NVME`, `STORE_512B_CRASH_RECOVERY_QEMU` | QEMU PASS, depth 1 only; never driven on this controller | L199 |
| 4 disk | Move data off ext4 and the Atlas TPM-sealed mounts (P3) | no gate yet | NONE | unowned |
| 5 network | Realtek `10ec:8127` 10 GbE, UDP report out (D5) | RTL8127 model gate + QEMU report gate | NONE (no driver exists; org-check found 0 hits) | L198 |
| 5 network | ConnectX: zero `15b3` devices seen on this unit (D5, section 3 of the map) | read link state of both root ports, read-only | UNKNOWN | L204 (Drake looks at the unit) |
| 5 network | Wi-Fi MT7925, Bluetooth, HDA audio (D13): Drake reversed the drop and asked for drivers | model gates | NONE | L206 (Wi-Fi), L207 (Bluetooth, audio) |
| 6 input | Six platform xHCI controllers, USB keyboard (D6) | `KEYBOARD` | QEMU PASS; physical NONE; UNKNOWN whether the MediaTek glue is powered when we first touch it | unowned |
| 6 input | Serial console via SPCR (D8) | line on a serial capture | QEMU; NONE; UNKNOWN whether a header exists | L204 |
| 7 display | UEFI framebuffer console (D7) | photo of the console on the HDMI | QEMU; NONE (first boot showed `gb10: unavailable`) | L197 (checklist), attended boot |
| 8 trust | TPM 2.0 CRB driver, PCR policy, NV anti-rollback counter (D9) | `TPM2_Startup`, `GetRandom`, NV read against swtpm | NONE | L200 |
| 8 trust | Owner key ceremony and Secure Boot on for release builds (P1) | TRUST-1 | BLOCKED_OPERATOR | Drake (section 7) |
| 9 GPU firmware | GSP boot bytes and RPC subset (G2, cut C) | host byte-exact tests | host only, in flight | L192 |
| 9 GPU firmware | Kernel finds the GB10 and owns BAR0 (G1) | `BOOT_0` read after ExitBootServices | NONE | unowned |
| 9 GPU firmware | GPU interrupts, or a written reason polling suffices (G4) | fault or completion seen | NONE | unowned |
| 10 GPU compute | Page tables, allocator, channel and doorbell model (G3, cut D) | simulated device end to end, then one vector add on the chip with teardown and reopen | host simulator, in flight; chip NONE | L193 |
| 10 GPU compute | Omega platform services, typed handles, freestanding link (G6, cut A) | omega gates green hosted and freestanding | host | L202 |
| 10 GPU compute | Matmul, attention, launchers, session, resident runtime in Omega (G6, cut E) | receipts against the Linux reference | NONE | unowned |
| 11 model run | CPU inference on the Spark (`aienos-infer`) | tok/s against the memory ceiling (ceiling not measured) | NONE on Spark; Pi 5 proves the path | unowned |
| 11 model run | 27B weights from NVMe, KV on the GPU, token stream (G7) | a token stream from the Spark | NONE | unowned |
| 12 service API | Request path from another machine (G8) | a request answered by the Spark | QEMU for the transport; no TCP or TLS exists | unowned (section 4) |

### 2.2 iMac 2017 (iMac18,3, Core i5-7500, x86_64): PROVISIONAL until `docs/IMAC_AIENOS_MAP.md` lands (L210)

Built from `evidence/imac_*`, `native/kernel/README.md` (iMac section), the ledger and `~/handoffs/2026-10-17-imac-allen-milestone.md`. Hardware results below are transcriptions from photos of the screen, recorded in the evidence folders.

| Rung | What | Gate | Proven where | Ledger |
| --- | --- | --- | --- | --- |
| 1 boot | UEFI stub, GOP, ExitBootServices, `kernel: alive`, power off through ACPI S5 | `AIENOS_IMAC_KERNEL` | HARDWARE (2026-10-16 and 10-17, SanDisk stick through the Option-key picker) | L040 (bundle 1, desk) |
| 1 boot | Boot from the iMac's own disk, no stick (#418) | `AIENOS_FAT_BOOT` (61 checks PASS on host); `qemu_ck_imac_bootdisk_test` NOT_RUN by the last note | HARDWARE partial: Apple's firmware started the internal boot section by itself, then the FAT check failed on rewritten timestamps (fix `1c91bb2` pending a physical run) | unowned after the handoff of 2026-10-18 |
| 2 CPU | GDT, IDT, APIC timer 100 Hz, all cores started | kernel gate, `SMP` | HARDWARE: 4 cpus, 3 started, timer 10000 us, no stray IRQs (`imac_kernel1_2026-10-17`) | none needed |
| 3 memory protection | W^X page tables, NX, CR0.WP (#409, #413) | QEMU mutation probes | HARDWARE 2026-10-17: `wx ok`, audit 15/15 | done |
| 3 memory protection | VT-d (#411): DMA confinement before networked sensitive use | no gate yet | NONE | unowned |
| 4 disk | AHCI read-only (Apple HDD, 953869 MiB) and NVMe read-only (`144d:a806`, 4 KiB blocks), GPT check | `AIENOS_IMAC_KERNEL` disk lines | HARDWARE read-only; writes refused by the build | unowned (write path, Store on the iMac) |
| 5 network | BCM57766 transmit-only report on UDP 47031, opt-in lab build (#408, #412) | `ck_imac_report_test` | HARDWARE: Ethernet seen; report sent | L142 |
| 5 network | Receive, live kernel swap over Ethernet, signed GPU register console | QEMU swap and console round-trip | branch `feat/imac-gpu-bench` pushed; QEMU test NOT_RUN (lock held at the time) | L142 |
| 6 input | USB keyboard through xHCI (#415) | `KEYBOARD`-style gate | branch `feat/imac-keyboard`; hardware result UNKNOWN to this page | desk |
| 7 display | 3840x2160 GOP framebuffer, ALLEN window: sky, water, orb, black-hole dusk | `SCREEN` | HARDWARE: `gop: ok 3840x2160`; ALLEN frames shown | L076, L186 |
| 8 trust | TPM or secure element | no gate yet | UNKNOWN: a 2017 iMac probably has no TPM (INFERRED, medium, not checked) | unowned until the map answers |
| 9 GPU firmware | Polaris 11 / DCE 11.2: VRAM map, microcode load, SMU, interrupts; register numbers fixed to AMD headers (164 checks exit 0) | `check_regs.sh`; host tests 21 PASS | host only; default OFF; NOT tested on the iMac | L143, L187 |
| 10 GPU compute | Our own GCN 4 encoder, PM4 packets, fill, blit, sky-lens kernels | encoder byte-exact against `llvm-mc` (283 of 283), 4 kernels PASS in the interpreter | host only; chip NONE | L144, L145 |
| 11 model run | Native small model on the iMac CPU | no gate yet | NONE; ALLEN today answers through the Spark's 27B over UDP | pending L210 map |
| 12 service API | ALLEN UDP courier and relay to the model server (port 18091) | QEMU end to end with the 27B answering | QEMU PASS (L039) | L039 (done) |

### 2.3 Raspberry Pi 5 (the hub): PROVISIONAL until `docs/PI5_AIENOS_MAP.md` lands (L210)

Built from `evidence/pi5_hardware_runs_2026-10-13`, `docs/native/PI5-*.md` and `native/kernel/README.md`.

| Rung | What | Gate | Proven where | Ledger |
| --- | --- | --- | --- | --- |
| 1 boot | `BOOTAA64.EFI` through the one-shot tryboot runner, then back to Linux | `CK_PI5_BUILD`; runs `pi5-k1`, `pi5-sd1a`, `pi5-sd1b` | HARDWARE (2026-10-13) | done |
| 2 CPU | Core image plus board glue, BCM2712 watchdog armed 15 s | build checks | HARDWARE | done |
| 3 memory protection | same kernel core; no Pi-specific record read | no gate yet | UNKNOWN to this page | pending L210 map |
| 4 disk | microSD host read and write, raw model partition read through UEFI Block I/O | `pi5-sd1a`, `pi5-sd1b`, `pi5-infer` | HARDWARE | done |
| 5 network | Cadence GEM: loopback and wire transmit | `pi5_gem` loopback PASS (two), wire TX PASS; receive of an incoming frame NOT seen in 400 ms | HARDWARE for send; receive NONE | `pi5/net-398-wire` branch, unowned |
| 6 input | none planned for a headless hub; UART or none | no gate yet | UNKNOWN | pending L210 map |
| 7 display | none planned | none | n/a | pending L210 map |
| 8 trust | No TPM. Unlock through the ADR 0017 recovery-secret path with a throwaway card request (qualification only); owner key ceremony done on the YubiKey (#380, online by owner decision) | `PI5-UNLOCK-QUALIFICATION`, PI5-8 Q2 | HARDWARE for the qualification tier; production tier NONE | unowned |
| 9 GPU firmware | VideoCore: no path exists | none | NONE | pending L210 map (is a Pi GPU path wanted at all?) |
| 10 GPU compute | same | none | NONE | pending L210 map |
| 11 model run | Llama-3.2-1B Q4_K_M, ARM dot-product instructions: 0.22 tok/s scalar, 1.20 tok/s after (7 timed tokens); prefill 11.1 s for 17 tokens | `pi5-infer` run record | HARDWARE | done; ceiling comparison unowned |
| 12 service API | ALLEN REFLEX decision core and the agent image | `PI5-6-AGENT-RUNS`, PI5-8 qualification | HARDWARE for the agent runs | done |

### 2.4 MacBook Air (Apple Silicon): PENDING `docs/MACBOOK_AIENOS_MAP.md` (L208)

No AienOS code has run on this machine, no inventory is in the repo, and the model of the MacBook is not recorded anywhere this page read. All twelve rungs are UNKNOWN until the map lands. What the map must answer, each as a physical or measured reason and not as "undocumented": how a custom kernel is started on Apple Silicon (Apple's boot chain permits it under reduced security, but which of the steps are documented is UNKNOWN to this page), the CPU and interrupt controller, the display path, the storage controller, whether any public GPU register documentation exists (if none, record DOCS SILENT and name the read-only open-source reference with its licence), and whether the machine is a compute node, a gateway or a client.

| Rung | Gate | Proven where | Ledger |
| --- | --- | --- | --- |
| 1 to 12 | per the L208 map | NONE | L208 (the map itself); every rung below it `unowned` until the map names them |

### 2.5 Ballentine_PC (Ryzen 5 7600X, RTX 5070 12 GB, Windows 11): PENDING `docs/PC_AIENOS_MAP.md` (L209)

No AienOS code has run on this machine. The brief for L209 says its GPU path is shared with the GB10 (both Blackwell family, INFERRED from the brief, not checked). If that holds, cut C and cut D work on the Spark (L192, L193) is reusable here, and the PC could be the second machine on which the GPU path is proven. The map must also produce a one-line PowerShell inventory command for Drake (section 7).

| Rung | Gate | Proven where | Ledger |
| --- | --- | --- | --- |
| 1 to 12 | per the L209 map | NONE | L209 (the map itself); every rung below it `unowned` until the map names them |

## 3. What the machines share (so work is built once)

| Shared piece | Used by | Where it lives | State |
| --- | --- | --- | --- |
| Kernel core (memory, scheduler, capabilities, IPC, Store, recovery) | all | `native/kernel/core`, `native/store`, `native/m5` | QEMU PASS; hardware on iMac and Pi (partial), Spark once |
| NVMe driver | Spark, iMac (read-only) | `native/disk/disk_nvme.c` | QEMU PASS; iMac HARDWARE read-only |
| Model inference on CPU | Pi (proven), others | `crates/aienos-infer`, `core/infer.c` | Pi HARDWARE |
| Signed transport and UDP stack | all | `native/net/aienos_sec.c`, `aienos_net.c` | QEMU and host, TEST keys |
| GPU path (firmware boot, MMU, channels) | Spark, PC | `docs/GB10_NATIVE_DEPENDENCY_MAP.md`, L192, L193, L202 | host simulation only |
| Omega compiler and kernels | Spark first | Omega repo | host; 14 fixture kernels byte-identical (omega#419) |

## 4. The network between machines

### 4.1 What exists (OBSERVED from source at `18e2765`)

- **UDP stack** (`native/net/aienos_net.c`): Ethernet, ARP, IPv4, ICMP echo, UDP. No TCP, no TLS, no DHCP, no IPv6.
- **Reliable channel** (`aienos_ctl.c`): ordered, exactly-once delivery with retransmit, TEST identity only.
- **Signed transport** (`aienos_sec.c`, milestone M6-B): three-message handshake in the SIGMA pattern, Ed25519 identities, fresh X25519 keys per session, HKDF-SHA256, AES-256-GCM-SIV records, replay window, key ratchet. Proven in QEMU and host tests. **Two limits that matter here:** the keys are public TEST labels that protect nothing, and the peer list is "a pinned roster of one", so one machine can accept exactly one other.
- **Drivers behind it**: virtio-net (QEMU only), Pi 5 GEM (send proven), iMac BCM57766 (send proven), Spark RTL8127 (none, L198).
- **Lab paths already used on hardware**: the iMac report broadcast on UDP 47031, the iMac netboot spool with live kernel swap (branch `feat/imac-gpu-bench`, L142), and the ALLEN UDP courier that relays a question to the model server on the Spark (L039).

### 4.2 The gateway machine

AienOS has no TCP or TLS, so clients (browsers, the agents, `curl`) cannot talk to an AienOS machine directly. One machine that keeps its operating system sits in front and speaks HTTPS to clients and the signed transport to the AienOS machines (Spark map section 6, row G8). Today the model server on the Spark does this itself over Linux (`aien-serve-http`, port 18091).

- **Who is it?** Unresolved: this is ledger item L203, Drake's decision. The Pi 5 hub is already the always-on Linux peer on the tailnet (Spark map section 6, INFERRED; hardware not inspected there). **Conflict to resolve:** the Pi is also a full AienOS target (section 2.3). If the Pi goes "Linux fully off" it cannot be the gateway. Recommendation: keep the Pi as gateway until a second always-on Linux machine exists, and treat "Pi Linux fully off" as the last step in section 5, not an early one.
- **What the gateway needs that does not exist**: an HTTP-to-signed-transport translator (a small host program: unowned), a signed-transport peer list with more than one entry (section 4.3), and real identity keys (TRUST-1, section 7).

### 4.3 How machines reach each other without Linux, and Tailscale's replacement

Tailscale does four jobs: it gives each machine a private address, authenticates each machine, finds a path through firewalls and home routers, and relays when no direct path exists. On AienOS machines the first two are already the signed transport. The last two are not built.

| Piece | Today | Gap | Gate that retires it | Ledger |
| --- | --- | --- | --- | --- |
| Machine identity and authentication | Ed25519 plus X25519, TEST keys | Production keys from the owner key chain; machine ids; TRUST-1 BLOCKED_OPERATOR | handshake between two machines with owner-issued keys, TEST keys refused | unowned |
| Many peers | roster of one | A roster of N signed by the owner: add, revoke, rotate; storage in the Store | a three-machine handshake matrix in QEMU, plus one revoked peer refused | unowned |
| Addressing | LAN IPv4 and ARP only | A stable name per machine inside the roster so an address change does not break it | peer reachable after its IP changes | unowned |
| Path across firewalls and routers | none | Same-LAN is enough for the Spark, iMac and Pi (INFERRED, low confidence: only the Spark address, 192.168.1.108, was observed; the iMac and Pi addresses were not read). A relay is only needed if a machine is on another network. UNKNOWN whether the MacBook or PC will ever be off-LAN: experiment is to ask Drake (section 7) | an AienOS machine reached from outside the LAN through a relay, or a written decision that off-LAN access stays with Tailscale on the gateway | unowned |
| Remote shell and control | operator keyboard and screen only; the iMac swap path has a signed register console | A signed control console on every machine (the iMac console from `feat/imac-gpu-bench` is the pattern) | an operator command round-trip, signed, on hardware | unowned |
| Time | none (no RTC on the Spark path yet, L200) | Network time over the signed transport, so replay windows and certificates have a date | wall clock set from a peer, drift measured | unowned |

Rule from Drake's own instructions: do not hand-roll crypto. The primitives above already exist in the tree and are reviewed (`native/crypto`, `native/sig`, `x25519.c`); the missing pieces are protocol and roster work around them. A WireGuard-compatible tunnel is not planned: the tailnet stays on the gateway for traffic that must leave the LAN.

## 5. Linux (or macOS, Windows) fully off, per machine

"Off" means: the machine boots AienOS from its own disk by default, and the old operating system is gone or is only a recovery option. Each step is an operator step that only Drake can do (section 7) and none is authorised by this page.

| Machine | Today's OS | Blocking rungs | Order to "off" | Operator steps |
| --- | --- | --- | --- | --- |
| Spark | Ubuntu on ext4, Secure Boot off, BootOrder `0001` ubuntu | rungs 1 to 8 on hardware, then 9 to 12 for the model; the gateway and moved services (section 4.2) | Spark map section 7, items 1 to 20: attended boots 1 to 3, native CPU run, GPU path, move non-model work off the box, installer, soak with Linux still on the disk, then boot-order change, then wipe last | plug in monitor, keyboard and the recovery stick; attended boots; photograph the console; key ceremony; allow GSP firmware load; look at the unit (L204); decide where build, judge, git and agents live (L203); lift the `BootOrder` invariant in writing for the one cutover change |
| iMac | macOS or whatever is on the Apple disk: UNKNOWN to this page (pending L210) | disk write and Store on the iMac, boot from its own disk (#418), keyboard (#415), VT-d (#411), model run | pending `docs/IMAC_AIENOS_MAP.md` | boot-picker attended boots (Option key) are already how every run happens; firmware work is on hold (blocked twice by the automated cyber safeguard, see `~/handoffs/2026-10-18-imac-firmware-backup.md`, do not retry) |
| Pi 5 | Raspberry Pi OS or similar on the hub | the gateway conflict (section 4.2), network receive, a production unlock tier (no TPM) | last, after another gateway exists | pulling the plug for the power-cut test (PI5-8 plan item R); owner-key signing on the Mac with PIN and touch (item Q6) |
| MacBook | macOS | all rungs UNKNOWN | pending L208 map | pending L208 map |
| PC | Windows 11 | all rungs UNKNOWN | pending L209 map | one-line device dump for Drake (L209 produces it) |

## 6. The future: what is measured, what is planned, and what retires each

Everything here is PLANNED unless the status column says measured. "Retired by" is the test that, once green, removes the item from this section.

| Item | Status | Retired by | Ledger |
| --- | --- | --- | --- |
| **Omega as the only language** (ADR 0024): the kernel itself written in Omega, C kept only for the pieces hardware forces | PLANNED; Omega floats (R2) done (L135); C kernel and Rust scaffolding exist today | Omegese rungs K1 to K6 below | L184 (hermes-chat, coordinator builds it personally) |
| K1: bytes, text and long arrays in Omegese | PLANNED | merged with tests, local `make` per rung | L184 |
| K2: peek and poke of machine memory | PLANNED | same | L184 |
| K3: system words (the calls the kernel needs) | PLANNED | same | L184 |
| K4: flat placement (fixed layout of code and data) | PLANNED | same | L184 |
| K5: units (L096) | PLANNED | same | L184, L096 |
| K6: Aienux stage 0 written in Omega boots the Spark | PLANNED | a receipt from the real Spark, plus the final head judged once on the Spark (Drake's one-PR rule for #601) | L184 |
| Omega platform services (cut A) so Omega runs without Linux | in flight | omega gates green on hosted and freestanding builds | L202 |
| AienOS installer: write the kernel and Store onto a blank disk, A/B slots, rollback | PLANNED; slot loader gate `SLOT_LOADER` PASS in QEMU only | an install from a recovery stick on a real machine, with a forced-failure rollback | unowned |
| AienOS updater: signed update of the kernel and system, on every machine, with rollback and no Linux | PLANNED; nothing exists beyond the slot loader | a signed update applied and rolled back on hardware | unowned |
| Multi-machine model serving: one model spread across the Spark, the PC and others, or requests routed by load | PLANNED; wormhole and heartbeat work (L190) is a different thing: a draft model on one machine | two machines answering one prompt through the signed transport, output identical to one machine | unowned |
| Speed against the machine ceiling | MEASURED on Linux: the speed audit (L123) and machine ceiling (L151) hold the numbers and their receipts; native speed NONE | native token rate reported as a fraction of the measured ceiling | L123, L151 and the SPEED lines |
| Production trust chain: owner keys, Secure Boot on, anti-rollback | PLANNED; BLOCKED_OPERATOR | TRUST-1 qualification receipt | Drake |
| Public proof (one-command 27B demo) | in flight, runs on Linux today | demo on a clean Spark | L182 |

## 7. What Drake does, so nothing is asked twice

Only Drake can do these. Each belongs to one ledger id so the coordinator can find it.

| Item | Why only Drake | Where it applies | Ledger |
| --- | --- | --- | --- |
| Decide where build, judge, git and agents live after Linux is off; approve moving the Phase 3 judge | it changes how the whole project is tested | all machines | L203 |
| Look at the Spark unit: ConnectX ports present? serial header present? | the box shows no ConnectX device and nothing in software can see the connector | Spark | L204 |
| Attended boots: plug in monitor, keyboard and the recovery stick, press the button, send a photo or let the report come over the cable | physical presence | Spark (three boots), iMac (every run), Pi (power-cut test) | L197 (Spark checklist) |
| Allow the first GPU firmware load and every later GPU use | irreversible on hardware | Spark, then the PC | L192 follow-on, unowned |
| Owner key ceremony (YubiKey, PIN and touch), machine ids for the hardware image | secret material | all machines | TRUST-1 |
| Run the one-line device dump on the PC and the equivalent on the MacBook | read-only, but only Drake is at the machines | PC, MacBook | L208, L209 |
| Tell the program whether the MacBook and PC will ever be outside the home network | decides whether a relay is built (section 4.3) | network | unowned (new) |
| Lift the `BootOrder` invariant in writing for the one cutover change; change the boot order at cutover | the rule currently forbids it | Spark first | unowned (new) |
| Reset or power-cycle tests for recovery | physical | Spark, Pi | L197 |

Already answered, do not ask again: Wi-Fi, Bluetooth and audio drivers for the Spark are wanted (L206, L207), after the earlier drop (L205 dropped). iMac firmware replacement is on hold and must not be retried.

## 8. Unowned rows for the coordinator

Run these lines as written. Each has an owner placeholder, a title and the gate that closes it. Rows already owned by a ledger id are not listed. Rows for the MacBook and the PC wait for their maps and are added by the next revision of this page.

```
ledger add unassigned "Spark: installer + rollback-safe A/B slots on a blank disk (SLOT_LOADER, DISK_LAYOUT NOT_RUN)" "install from a recovery stick on a real machine with a forced-failure rollback; DISK_LAYOUT PASS"
ledger add unassigned "Updater: signed AienOS update with rollback on every machine, no Linux" "signed update applied and rolled back on hardware"
ledger add unassigned "Spark: power, thermal, fans, CPU frequency under no OS (D11)" "long hold at load with temperatures read; or written reason firmware holds them"
ledger add unassigned "Spark: GB10 discovery and BAR0 ownership by the kernel (G1)" "BOOT_0 read after ExitBootServices on the real Spark"
ledger add unassigned "Spark: GPU interrupts or written reason polling suffices (G4)" "a fault or completion interrupt seen, or a measured reason"
ledger add unassigned "Spark: GB-scale pinned and mapped memory, huge pages, 27B weight image (G5)" "one multi-GiB allocation mapped for CPU and GPU, in QEMU first"
ledger add unassigned "Spark: USB xHCI physical bring-up, keyboard (D6)" "KEYBOARD lines from the real Spark; glue power and clock state read"
ledger add unassigned "Spark: Omega cut E, matmul, attention, launchers, session, resident runtime (G6)" "receipts against the Linux reference per kernel"
ledger add unassigned "Spark: native CPU inference with aienos-infer on the Spark, speed against the memory ceiling (G7 step 10)" "tok/s and fraction of the measured memory ceiling"
ledger add unassigned "Spark: 27B load from NVMe, KV on GPU, token stream (G7)" "a token stream from the real Spark under AienOS"
ledger add unassigned "Gateway: HTTP to signed-transport translator, and the request path from another machine (G8)" "a request from another machine answered by the Spark"
ledger add unassigned "Network: signed-transport roster of N peers (add, revoke, rotate), stored in the Store" "three-machine handshake matrix in QEMU plus one revoked peer refused"
ledger add unassigned "Network: production machine identity (owner-issued keys, TEST refused) for the signed transport" "handshake between two machines with owner-issued keys, TEST keys refused"
ledger add unassigned "Network: network time over the signed transport" "wall clock set from a peer with drift measured"
ledger add unassigned "Network: signed control console on every machine (pattern: iMac swap console)" "signed operator command round-trip on hardware"
ledger add unassigned "Network: off-LAN reach decision (relay vs gateway-held Tailscale)" "Drake answers; if relay, a path through a relay in QEMU"
ledger add unassigned "Spark: move Atlas data and Forgejo off the TPM-sealed mounts, retire or relocate every Linux service (P3, U1 to U7, U10)" "service list in the Spark map, each row moved or retired with a receipt"
ledger add unassigned "iMac: VT-d DMA confinement (#411)" "VT-d QEMU gate PASS then physical report"
ledger add unassigned "iMac: boot from own disk, finish #418 after the FAT timestamp fix, QEMU bootdisk test and a physical run" "second power-on without stick or network starts the kernel from disk"
ledger add unassigned "iMac: disk write path and Store v1 on the iMac" "Store write, flush and crash recovery on the real iMac disk"
ledger add unassigned "Pi 5: network receive (RX not seen in 400 ms, PI5-NET-3)" "an incoming frame received on the real Pi"
ledger add unassigned "Pi 5: production unlock tier (no TPM), replace the qualification card request" "unlock from an operator secret, no card file"
ledger add unassigned "Multi-machine model serving across the Spark and the PC (or others)" "two machines answer one prompt over the signed transport, output identical to one machine"
```

## 9. Limits of this draft and what the next revision does

- Rows tagged PROVISIONAL (iMac, Pi) were built from repo files and hand-off notes read on 2026-10-20, not from the L210 map. No test was run for this page. Where a row says `UNKNOWN to this page`, the map may already know.
- The MacBook and PC sections are placeholders. Revision 2 replaces them from L208 and L209 when their READY lines appear in `~/handoffs/2026-10-18-pr-desk.md`.
- The `ledger add` block is complete only for the Spark, iMac, Pi and the network. It will grow with the two pending maps.
- Gate names for the iMac and Pi come from the evidence folders and `GATES.md`; I did not re-run any gate.
- The Spark row for "NVMe queue depth", "TPM" and the others carry L191's own tags; this page does not upgrade them.
- Reuse search: origin/main, `git branch -r` (iMac, Pi, Spark branches read by name and last commit), sibling worktrees `~/workspace/aienos-*` (three map worktrees exist and are empty), `~/handoffs` (iMac and Pi hand-offs), the ledger through L212, `docs/SPARK_REPLACE_LINUX.md` on `origin/feat/spark-replace-linux-map`. No existing document covers all five machines or the multi-peer network; `docs/GB10_NATIVE_DEPENDENCY_MAP.md` covers only the Spark GPU path. ADR 0010 (fabric) is named in `ROADMAP.md` and not yet read for this page.
