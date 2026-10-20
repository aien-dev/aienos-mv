# Interplane join: every AienOS machine is one Interplane peer

Status: design plus hosted code and a QEMU gate (ledger L263, also covers L230 roster and L231 production
identity). Kernel = Aienux, system = AienOS. Every claim below is tagged OBSERVED (read or run, with the
source), INFERRED (follows from what was read) or UNKNOWN.

## 1. What Interplane is today, and what "join" means here

OBSERVED (aien-dev/interplane main b669d06, README.md, STATUS.md, spec/VECTORVEIL.md): Interplane is an
open protocol between AI models and agent runtimes. Models express intent, runtimes keep authority. It
defines a JSON envelope (Vectorveil) and message kinds (tool_request, decision, result, event, catalog, ...),
dialect parsers, the authority boundary (Crossveil) and conformance fixtures run by Rust and Python SDKs.
It has no network membership today: Vectorveil says transport is in-process or request/response, and
"signature (origin proof, never authority)" and "remote transport bindings" are reserved, not implemented.

So joining Interplane means two things, built here:

1. **Membership**: an owner-signed roster of machines. A machine is in Interplane when its key is an active
   entry of the owner's current roster.
2. **The reserved remote transport binding**: Interplane messages (and plain text, model requests, console
   commands) travel between members inside the AienOS signed transport, each one signed by the machine that
   wrote it. The signature proves origin, never authority, exactly as Interplane's CORE.md requires: the
   receiving runtime still decides everything through its own policy.

## 2. The pieces

| Piece | Where | What it does |
|---|---|---|
| Secure session | `native/net/aienos_sec.{h,c}` (M6-B, existing) | SIGMA handshake (Ed25519 identities, X25519 ephemeral keys), AES-256-GCM-SIV records, replay window, rekey. New: a responder roster hook (`sec_config.peer_ok`) so one responder accepts any active roster member instead of one pinned key. Without the hook the old pinned behaviour is unchanged. |
| Roster, beacon, rendezvous, signed message, node | `native/net/aienos_ipj.{h,c}` | Wire formats in the header. The node is a table of up to 8 secure sessions on one UDP port (47040), admitted by the roster only. No heap, clock or I/O. |
| Roster in the Store | `native/net/aienos_ipj_store.{h,c}` | One Store v1 object per accepted roster, kind 25 version 1, bytes = the signed roster. Load picks the highest generation that verifies. |
| Owner tool | `native/net/tools/ipj_tool.c` | Generates the QEMU fixture: owner key, machine keys, rosters, one provisioning bundle per machine. |
| Kernel binding | `native/boot/efi_join.c`, `native/kernel/dev/net_join.c` (`CK_INTERPLANE=1` image) | The UEFI stub reads `\EFI\AIENOS\JOIN.BIN`; the devices stage runs the join loop on the SMMU-confined virtio-net NIC instead of the slirp round trip. |
| Gate | `scripts/qemu_ck_interplane_test.sh` | Five AienOS instances on one QEMU LAN (section 8). |

## 3. Identity: owner-issued machine keys (L231)

- Each machine has one Ed25519 machine key. Its machine id is `SHA-256("AIENOS-IPJ-v1 machine id\0" | pk)[0..16)`.
- The owner key (the one from `docs/OWNER_KEY_CEREMONY.md`) issues identities by listing machine keys in the
  signed roster. A peer pins the owner public key and accepts nothing else: a roster signed by any other key
  is refused (`IPJ_ERR_OWNER`), and a valid signature over a malformed roster is still refused.
- Every roster entry carries a kind: PROD or TEST. A machine in **production mode** refuses any roster that
  holds a TEST entry and refuses a TEST owner key (`IPJ_ERR_TEST_KEY`); its membership check also requires
  PROD. Test mode exists only for hosted tests and fixtures that say so.
- The secure transport's old `sec_test_identity` keys (public labels) are TEST by construction and are not
  used by the join path. The QEMU fixture uses fresh random keys from `/dev/urandom` every run, issued by a
  fresh owner key that only lives inside `ipj_tool`.
- **Limit (stated):** the QEMU provisioning bundle carries the machine secret key in the clear. On real
  machines the machine key must be sealed under the M5 key hierarchy and released by TRUST-1; that path is
  not built here (UNKNOWN effort, not measured).

## 4. The roster: add, revoke, rotate (L230)

Format: `aienos_ipj.h` (64-byte header, 64 bytes per entry, owner signature). Rules:

- **Generation only rises.** Each edit (add, revoke, rotate) bumps it by one. A peer refuses a roster whose
  generation is not above the one it holds (`IPJ_ERR_ROLLBACK`), so an old roster cannot bring back a revoked
  machine.
- **Add**: a new key, a name (1 to 16 printable ASCII), kind, platform. A key once listed never returns, even
  after revocation; active names are unique.
- **Revoke**: the entry stays, with status revoked. Every node that accepts the new roster closes its session
  with the revoked machine at once and refuses its handshakes, beacons, rendezvous datagrams and messages.
- **Rotate**: the old key is revoked and a new key takes over the same name and platform, in one generation.
- **Propagation**: whenever a session comes up, and whenever a node accepts a newer roster, it pushes its
  roster to its live peers. Each peer checks the owner signature and generation itself, so a relay cannot
  forge or roll back a roster. (Hosted test: generation 6 handed to the spark reaches the pi and the mac.)
- **Size**: 15 entries keep a signed roster (1088 bytes) inside one secure record. More machines need roster
  fragments (not in v1).
- **Stored in the Store**: kind 25, version 1. The Store proves the bytes are intact (unkeyed structural
  integrity), the owner signature proves who issued them, the generation proves freshness against older
  copies in the same Store. These are kept separate, as `AGENTS.md` section 1 requires. Hardware
  anti-rollback of the roster (TPM counter / platform anchor) is not claimed: a whole-disk rollback could
  bring back an older roster until that anchor exists.

## 5. Finding Interplane: LAN discovery and the gateway

- **LAN beacon**: every second each node broadcasts a 128-byte beacon on UDP 47040: its key, roster
  generation, a counter that rises every beacon, its address and port, signed by its machine key. A receiver
  drops beacons from non-members, bad signatures and counters not above the last one seen, then asks for a
  session.
- **Who dials**: the lower machine key (byte order) initiates; the higher one waits. Two machines never run
  two sessions to each other.
- **Rendezvous through the gateway machine**: for peers that broadcast cannot reach (a VM behind host NAT,
  another subnet), a configured gateway (a roster member, platform `hub`) keeps a table of signed REGISTER
  datagrams and answers signed LOOKUPs. A REGISTER may say address 0.0.0.0, meaning "use the address you saw
  me come from", which is how a peer behind NAT learns nothing and still becomes reachable. Answers are
  signed by the gateway and matched to the lookup's counter. The gateway serves and answers for members only.
  Hosted tests only; no gateway runs on a real hub yet.

## 6. What a joined machine can do

| Capability | State |
|---|---|
| Signed messages, end to end | Built. Origin-signed (Ed25519), carried hop by hop inside secure sessions. A relay may lower the ttl byte and change nothing else; any other byte change is refused at the destination. Replay refused per origin sequence number. Owner-configured routes bind ("reach the laptop through the Pi"): they win over a direct session, and while the relay is not connected the message waits instead of going around it (found by the QEMU gate, test `test_route_binding`). |
| Interplane envelopes | Message kind 2 carries an Interplane envelope (JCS JSON) to the destination runtime. Proposed binding text is on the interplane branch `feat/aienos-join` (`docs/bindings/AIENOS-SIGNED-TRANSPORT.md`). The signature proves which machine sent it, never that the action is authorized. |
| Model requests | Message kind 3 is reserved for a model request to the destination's model service (for example the Spark's AIEN model). The service side is not built (UNKNOWN until a model service listens on the node). |
| Control console | Message kind 4 is reserved for operator console commands. Design: only an owner-held operator key may send them, each command signed and counted, replacing the iMac lab console's shared HMAC key (aienos-mv `feat/imac-gpu-bench`, UDP 47035, observed in `~/handoffs/2026-10-19-imac-gpu-bench.md`). Not built (ledger L237). |
| Roster distribution | Built (push on session up and on update). |

## 7. A laptop running our VM is a first-class peer (aienos-run on Windows, macOS, Linux)

aienos-run (ledger L261 Windows, L262 Linux, L265 macOS) boots AienOS in a VM with user-mode networking.
INFERRED from QEMU's user networking: the guest sits at 10.0.2.15 behind the host's NAT and its LAN
broadcasts never leave the host. The plan:

1. aienos-run forwards host UDP 47040 to guest UDP 47040 (QEMU `hostfwd=udp::47040-:47040`; the same rule in
   the WHPX and Hypervisor.framework back ends). This is the "signed transport's UDP port" in the L261 brief.
2. The guest registers with the configured gateway, address 0.0.0.0, port 47040. The gateway records the
   laptop's real LAN address (what it observed) plus 47040.
3. Other members look the laptop up through the gateway and dial `laptop_ip:47040`; the host forwards it to
   the guest. The session, the roster check and the signatures are the same as on native machines: the host
   OS only moves encrypted datagrams and cannot read or forge them.
4. The roster lists the laptop's own machine key with platform `run-windows`, `run-macos` or `run-linux`.

UNKNOWN (must be measured on aienos-run, not assumed): whether user-mode networking sends the guest's replies
from host port 47040 or from a fresh port. The node keys a session by the address it actually sees, so either
works as long as it stays the same for one session; if it changes mid-session the session times out and is
redialed.

## 8. Proof

- Hosted (`make -C native/net test`, `sanitize`, `mutants`; `tests/ipj_test.c`): roster round trip, every
  byte of a signed roster, beacon and message flipped and refused (only the ttl byte of a message may change),
  wrong owner, rollback, TEST refused in production, duplicate keys, full roster; rendezvous behind NAT,
  replay, revoked registrant, non-member subject; Store save, reboot, load the newest, refuse a rollback copy;
  provisioning bundle layout; a simulated LAN of spark, pi, mac and a revoked old machine with the spark-mac
  link cut: sessions only between members, old refused, a signed message spark -> pi -> mac delivered with
  the origin verified, then a rotation (mac -> new key) pushed from the spark through the pi, the old mac
  session closed, the new mac key joined and reached.
- QEMU (`scripts/qemu_ck_interplane_test.sh`, final line `AIENOS_CK_INTERPLANE: PASS|FAIL|NOT_RUN`): five
  Aienux instances on one virtual LAN: spark, pi, mac join one generation-5 roster; spark->pi and pi->mac
  sessions; the signed message spark -> pi -> mac; old (revoked, holding generation 4) refused by everyone;
  tst (a TEST entry) refused to join in production mode.
- Gate result (2026-10-20, Spark, QEMU TCG, run 4 of this change): `AIENOS_CK_INTERPLANE: PASS`. spark, pi and mac
  joined one generation-5 roster in production mode; sessions spark<->pi and pi<->mac; the signed message went
  spark -> pi -> mac (pi relayed, did not deliver; mac delivered with the origin signature verified, one hop);
  old (generation 4) held no session and a member recorded the refusal; tst was refused (TEST_KEY).
  Earlier runs failed for real causes that are fixed: a broadcast beacon refused by the unicast frame builder,
  and a route that silently detoured around its relay (now an owner route binds; hosted `test_route_binding`).
- Not proven: real NICs (QEMU only), real machine-key sealing, the gateway on a real hub, aienos-run's host
  forward, off-LAN reach, timing or speed (no measurement here).

## 9. What Interplane needs from the Linux hub until the Pi runs natively

The Pi is meant to be the hub; until Aienux runs there natively, the Linux hub has to provide:

1. **The gateway (rendezvous) service** on UDP 47040: the `ipj_gateway_handle` state machine in a small
   hosted C program (to be built from `aienos_ipj.c`; no outside library), holding the current signed roster
   and its own machine key (platform `hub`, a normal roster member).
2. **A fixed LAN address** for that gateway (a DHCP reservation), because every bundle names it.
3. **Off-LAN reach**: until a native tailnet node exists (L234) or a relay is decided (L238), the hub's
   Tailscale subnet route is what carries Interplane datagrams to members away from home. The datagrams stay
   end-to-end encrypted and signed; the hub only forwards.
4. **DNS and time**: the hub's DNS (Pi-hole role, L235) and NTP until network time over the signed transport
   exists (L236). Interplane does not depend on wall-clock time (counters and generations, not timestamps).
5. **No secrets**: the hub never holds the owner secret key. Roster edits are signed on the owner's machine
   and handed to any member; propagation does the rest.

## 10. Limits and next steps

- Roster fragments for more than 15 machines; the roster in the NVMe Store from the kernel (hosted only now).
- Machine key sealed under M5 and released by TRUST-1 instead of a cleartext bundle.
- The hub gateway program and the aienos-run port forward, measured on real hosts.
- The console (L237) and the model-request service on top of message kinds 3 and 4.
- Responder denial-of-service hardening (cookies) is still not done in M6-B, and identity hiding is not a goal.
