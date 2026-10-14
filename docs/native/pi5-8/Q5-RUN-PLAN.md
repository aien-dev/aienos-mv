# PI5-8 Q5 run plan: the release candidate end to end on the Raspberry Pi 5

STATUS: PLAN ONLY, NOT RUN. Written with PI5-8 cut R (build side). Nothing here has touched the Pi. The image is the release
candidate of `docs/native/PI5-8-QUALIFICATION.md`, "PI5-8 RC" (`make pi5-rc`, the owner root PUBLIC key as the only artifact
anchor). The run is called **clean-shutdown cold recovery**: a clean stop, then a real loss of power, then a cold boot that must
return the same bytes. It is not a power cut in the middle of a Store write (that stays QEMU-only, Q7).

## Before the first run (blocking)

1. **ALLEN in the RC image.** Today the lifecycle image (and so the RC) has no ALLEN decision engine in it; cut K links ALLEN
   into a Pi image. Q5 needs ALLEN inside the lifecycle path (plain-language request in, proposed command out, then the
   lifecycle checks it). When that lands: add any new build switch to `PI5_RC_INPUTS` and the `pi5-rc` rule
   (native/kernel/Makefile), add any new test-class switch to `include/ck_pi5_rc_guard.h`, and fill in the exact command
   lines marked **[after K]** below. Until then the ALLEN steps cannot run.
2. **Image.** Build the RC from the merged commit twice (`bash scripts/ck_pi5_rc_check.sh` with `AIENOS_STRICT=1` must end
   `PI5_RC_CHECK: PASS` with 0 skipped), and record `rc_efi_sha256` in the ledger. Every Q5 run uses that one EFI file.
3. **Inputs** (in `~/workspace/pi5-runs/q5-prep/`, never committed, the secret never printed):
   - `S.req`: the 512-byte `AIENUNQ1 QUALIF-THROWAWAY` request that opens the card Store (qualification tier, as in PI5-4 and
     PI5-7). `W.req`: `S.req` with its last secret byte changed.
   - `c-pos.cmd`: the positive command script **[after K]**: the plain-language request for ALLEN, then `approve`, then
     `recall`, `facts`, `evidence`.
   - `c-reject.cmd`: rejected operations only, no verb that can commit:
     `approve`, `deny`, `cancel` with nothing pending, plus the ALLEN cases **[after K]**: a request whose ALLEN prediction is
     "approve" (a prediction must never act as the operator), an approval replayed after `deny`, an approval for an object that
     is not the current pending one; then `recall`, `facts`, `evidence`.
   - `c-read.cmd`: read only: `status`, `recall`, `facts`, `evidence`. Before every use check it holds none of the verbs
     `prompt task remember prefer approve` (`grep -cE '^(prompt|task|remember|prefer|approve)( |$)' c-read.cmd` must print 0).
   - The sha256 of every input file goes into the ledger before the first run and must not change after.
4. **Spark:** `quietlock check` before starting; CPU only.

## The lease (held from the first staging to the last comparison)

The whole sequence runs as ONE command under one exclusive lock, so no other Pi job (cut K exam runs, network runs, the PKV
harness share) can stage, boot or format anything in between:

```
flock -x ~/workspace/pi5/.pi-lease bash ~/workspace/pi5-runs/q5-prep/q5-session.sh
```

`q5-session.sh` is written and reviewed before the run (not in this cut). It runs the steps below in order, stops at the
first failed check (no silent retry; a failed run is reported as it is), writes everything to `~/workspace/pi5-runs/q5-*`, and
waits for Drake at the power step while still holding the lock. Never kill it or a Pi run. Each Pi run inside it is the
existing harness `~/workspace/pi5/pi5-run-rc.sh <EFI> <tag>` with `KLOG=1`, `UNLOCK_REQ=` and `AGENT_CMD=` set as listed;
`ARTIFACT_BND` stays unset (the RC refuses card artifact bundles: they are TEST-anchor only). `MODEL_MAP=` only if the
positive request needs the model's reply **[after K]**. The harness places the inputs on the card, reboots the hub Pi once with
tryboot, collects the log, and zeroes, deletes and checks absent `UNLOCK.REQ` and `AGENT.CMD` after every run.

**Store view hash** (H): on the hub, partition not mounted,
`sudo dd if=/dev/mmcblk0p2 bs=512 count=8388544 status=none | sha256sum` (sectors 0 to 8388543; the 64-sector run marker at
the end changes every run and is left out, as in PI5-4).

## The runs, in order

Every successful open of the Store commits a boot record, so the raw hash changes after every run that opens it. "State
unchanged" therefore means: raw H unchanged after a run that must not open the Store, and the application state (every
`recall`, `facts` and preference line, object ids and generations) unchanged after a run that opens it but must not commit.

| Tag | Secret | Commands | Must show | Then check |
|---|---|---|---|---|
| q5-n0 baseline | S | c-read | `artifact_trust: PI5-8 RC owner anchor only, test_anchors=0 owner_root_ed25519=578ada3c...9c76`; `artifacts: candidates=0 admitted=0 rejected=0`; Store opened; continuity agent id and incarnation | record the application state A0 and H0 after the run |
| q5-n1 wrong secret | W | c-reject | `store: REFUSED proof=keyed ... (RecoverySecretRejected); disk left as found`; every verb refused, state locked | H after = H0 |
| q5-n2 missing secret | none | c-reject | Store not opened, lifecycle locked, every verb refused (exact refusal line not known yet: record it as printed) | H after = H0 |
| q5-n3 tamper | S | c-read | one byte of the KeySlotManifest flipped first (below); Store REFUSED, not reformatted | exactly one byte differs from the pre-flip copy; put the byte back; H = H0 again |
| q5-n4 rejected operations | S | c-reject | every operation REFUSED (`nothing pending`, binding refused, replay refused **[after K]**: exact lines); `last_commit_gen=0` | application state equals A0 |
| q5-p1 positive | S | c-pos | ALLEN's proposed command printed, the lifecycle's validation, `pending`, `approve committed gen=G id=I`, `recall gen=G ... id=I bytes=N text=...` | record the payload (below) before any shutdown |
| power step | | | Drake pulls the plug (below) | cold boot proven |
| q5-r1 recovery | S | c-read | Store opened, `continuity=resumed` with the same agent id and the next incarnation, `recall gen=G id=I bytes=N` | the recovered bytes equal the recorded ones |

**Tamper (q5-n3), as PI5-4 Run D.** Copy the partition on the hub, run `native/kernel/tools/ck_pi5_store_locate` on the
copy to find the KeySlotManifest, XOR one byte of the recovery slot's wrapped K_vol with 0xFF on the card by a whole-sector
read, modify, write, keep the original sector, and check on readback that exactly one byte differs. After the run, write the
saved sector back, check it byte for byte, and check H = H0. Limit: this tampers the key slot, not an encrypted object; a
flipped byte inside a stored object cannot be placed without the keys.

**Record before shutdown (q5-p1).** Into `~/workspace/pi5-runs/q5-p1/record.txt`: the full `recall gen=G` line from the raw
kernel log (not a cut copy), its byte length and sha256 (the payload as the kernel prints it), the object id I and generation
G, the continuity agent id and incarnation, the Store generation and boot count, the EFI sha256. Then confirm the harness
printed `agent_cmd_deleted_verified` and `unlock_req_deleted_verified` (the originating command is off the card, so nothing
can replay it), and run `c-read.cmd`'s verb check again. q5-r1 uses `c-read.cmd`, which cannot create or approve anything.

## The power step: exactly what Drake does

This is the same as the PI5-7 power cut (P1, 2026-10-14, 10 seconds unplugged). The hub Pi is also the house server, so
the phone VPN and the other hub services are off for about five minutes.

1. The session script shuts the hub down cleanly (`ssh hub sudo poweroff`) after q5-p1 is recorded, then waits until the hub
   has not answered a ping for 60 seconds in a row.
2. Only then does it print `Q5 POWER STEP: PULL THE PLUG NOW` (and the orchestrator tells Drake in plain words).
3. **Drake:** pull the power cable (USB-C) out of the hub Raspberry Pi 5. Leave it out for **at least 10 seconds, count to
   15 to be safe**. Then plug it back in. Nothing else to touch: no card, no stick, no keyboard.
4. **Drake** tells the orchestrator when the cable came out and when it went back in; the orchestrator writes both times into
   `~/workspace/pi5-runs/q5-p1/power.txt` and only then creates the file `~/workspace/pi5-runs/q5-p1/REPLUGGED`.
5. The script waits for that file, then for the hub to answer ping and ssh, records `uptime` (it must show a fresh boot of a
   few minutes, proving the power really went off) and starts q5-r1.

If the hub does not come back within 15 minutes after the plug goes back in, Drake unplugs it once more for 10 seconds and
plugs it back; the Pi boots its normal Linux either way.

## Pass rule for Q5 (fixed before the run)

All of these, else Q5 FAILS and is reported as it is:

- every negative run refused in the expected way and left H (or the application state, for q5-n4) unchanged;
- q5-p1 committed exactly one object through ALLEN proposal, lifecycle validation and an explicit operator `approve`;
- q5-r1 after the real power loss shows the same object id, generation, byte length, and the same full `recall` line sha256 as
  recorded before shutdown, and the same continuity agent with the next incarnation;
- every run printed the owner anchor line with the ceremony key, and no TEST anchor, TEST-ONLY or fw_cfg text appeared in any
  log.

## How the report states the result

Three proofs, reported separately, never merged into one "secure" claim:

- **Structural:** the Store opened, objects and generations are consistent, the tamper was detected and nothing was
  reformatted.
- **Keyed authentication:** the right secret opened the Store, the wrong and missing secret did not, the key slot tamper was
  refused.
- **Freshness:** the anchor resumed (`anchor=valid-resume`). Limit, to be stated every time: the anchor shares the disk, so a
  whole-disk rollback is not detectable (`svc/store_boot.c`); the owner key signing anything cannot give hardware freshness
  (that needs TPM NV or another external counter).

And the limits that go into Q7: the unlock is the qualification card tier (a file on the card holds the secret) until the USB
lane lands; one Pi, one card; the power was lost after a clean shutdown, not during a Store write; the byte comparison is of the
object as the kernel prints it (`recall` line), since the Store itself is encrypted; the owner-key path was proven by the host
test only (no owner-signed artifact exists, and the Pi run shows zero candidates).
