# PI5-8 groundwork: release qualification evidence (Pi 5)

QUALIFICATION ONLY. This is the evidence package for qualifying the Pi, not a public release. Sections Q1, Q3 and Q6 use
TEST keys only. The owner key now exists (PI5-8 key ceremony, `docs/native/PI5-8-OWNER-CEREMONY.md`); only its PUBLIC half is
used here, by the release candidate image ("PI5-8 RC" below). Nothing in this document was signed with the owner key.

## Q1 Reproducible builds

`scripts/ck_pi5_repro_check.sh [commit]` makes two clean detached worktrees of one commit at two different paths, builds all
eleven Pi images in each (pi5, pi5-store, pi5-unlock, pi5-unlock-card, pi5-fact, pi5-infer, pi5-agent, pi5-lifecycle,
pi5-artifact, pi5-pcie, pi5-net) with the same toolchain, and compares sha256 of each `BOOTAA64.EFI` and `aienos-ck.elf`.

Finding and fix: the first run matched for the eight C-only images and differed for the three that link the Rust inference
library (pi5-infer, pi5-agent, pi5-lifecycle). The Rust panic messages carry the absolute source path
(`<checkout>/crates/aienos-infer/src/*.rs`). Fix at the source: the three Makefile rules that build that library pass
`RUSTFLAGS=--remap-path-prefix=$(REPO)=/aienos`, so the checkout path no longer enters the image. What the image does is
unchanged (only path strings in panic messages). After the fix every `BOOTAA64.EFI` matches byte for byte.

Remaining, understood difference: in the same three images the raw `aienos-ck.elf` still differs in symbol NAMES only (Rust
crate-hash suffixes: cargo hashes the absolute path of the path dependency `aienos-infer`, which lies outside the infer-kernel
crate's own workspace). Symbols are not in `BOOTAA64.EFI`. The script accepts that case only when the ELF is byte-identical
after `strip -s`, and prints it as `match-stripped`.

Toolchain recorded by the script on every run. Used: GNU Make 4.3, gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), GNU ld and
objcopy 2.42, rustc 1.98.1 (48a229cea 2026-09-01), cargo 1.98.1 (797e8a9bc 2026-08-05).

The kernel embeds the commit id (`aienos_commit:`), so two builds are comparable only from the same commit. For the
QEMU/Spark images (default `all`, `full`, `full` with the TEST anchor, and the QEMU artifact-card twin) byte identity against
origin/main was checked by building both with the same pinned commit string: all identical.

## Q3 Receipt tier on the Pi

The artifact loader used to write receipt `tier` = SEED-0B-QEMU (record offset 22) even in the Pi image. Now
`CKA_IMAGE_TIER` (native/kernel/artifact/ck_artifact.h) is `CKR_TIER_SEED0B_PI5` (3) when the image is built with `CK_PI5`
and `CKR_TIER_SEED0B_QEMU` (1) otherwise; decode accepts the Pi tier only in the Pi image and in hosted tools (the
verifier), so the QEMU/Spark kernels keep their code unchanged. The Rust receipt codec (`aienos-artifact`) knows tier 3 too.
The host tool prints the tier it decoded. Host test: `test_artifact` (default build expects QEMU, `-DCK_PI5` build expects
PI5, round trip, signature covers the tier, tiers 2 and 4 refused). Mutant: `make artifact-tier-mutant` (Pi build that still
writes the QEMU tier) must fail the test, and does.

Pi run `pi5-artifact-a2` (same card bundle as a1, sha256 0757c5c5...6d70, no MODEL.MAP on the card):
`artifact_receipt_tier: SEED-0B-PI5`, `artifacts: candidates=6 admitted=5 rejected=1`, six receipts. On the host,
`ck_artifact_tool receipt check` (bound to the supplied artifacts), TEST-ONLY `sign-test` and `verify` all PASS for the six
receipts with `tier=SEED-0B-PI5` and the digests equal to the ones printed in the log.

## Q6 Receipts manifest (no signing key yet)

`scripts/pi5_receipts_manifest.sh <run-folder>... | --all <runs-root>` reads each run folder's `kernel.log` and `run.log` and
prints a deterministic manifest: per run tag (sorted), image EFI sha256, aienos_commit, kernel.log sha256 and the verdict
lines picked by the checked-in table `scripts/pi5_receipts_verdicts.tsv` (PI5-3 storage, PI5-4 unlock A-E, PI5-5 F1-F3,
PI5-6 infer and G1-G3, PI5-7 L1-L3, power cut P1, artifact a1 and a2, pi5-pcie-1, pi5-net-1, pi5-net-3-r1). It refuses to
print anything if a folder is missing, a field is absent or a table row matches no line. Plain ASCII, LF only, no timestamps or paths.

Signing design (updated for the owner YubiKey, see `docs/native/PI5-8-OWNER-CEREMONY.md`): PIV Ed25519 signs only a small message,
so the owner signs a fixed four-line ASCII statement that binds the manifest by its length and sha256; the signature is a plain
Ed25519 signature (RFC 8032, 64 raw bytes) over those statement bytes. `native/kernel/tools/ck_manifest_sig.c verify MANIFEST SIG
PUBKEYHEX` rebuilds the statement from the manifest and checks it with the in-tree verifier; its only signing command,
`sign-test`, uses the public RFC 8032 TEST 1 key and says TEST ONLY. `scripts/ck_pi5_manifest_check.sh` proves the round trip (a
tampered manifest, a tampered signature, a different key and a signature over the raw manifest all fail; an openssl TEST key
signature verifies too).

## PI5-8 RC: release candidate image with the owner public key (cut R, build side)

Build: `make -C native/kernel pi5-rc CK_OWNER_PUBKEYS=<owner pubkeys file>` (the ceremony file
`~/aienos-owner-ceremony/20261014T114310Z/owner-pubkeys.txt` on the Spark, sha256 `9d2aabd9...ceb34d181`). Output:
`target/native-kernel-pi5-rc/full-pi5-unlock-card-lifecycle-rc/{aienos-ck.elf,BOOTAA64.EFI}` (own OUT). The image is the PI5-7
unlock-card lifecycle image (`CK_PI5_LIFECYCLE=1 CK_PI5_UNLOCK=1 CK_PI5_UNLOCK_CARD=1`) built with `CK_PI5_RC=1`. Its one
functional change: the artifact loader trusts the owner root public key
(`owner_root_ed25519 578ada3c...2d679b8c9e76`) and nothing else, and the image carries no TEST public key. At boot it prints
`artifact_trust: PI5-8 RC owner anchor only, test_anchors=0 owner_root_ed25519=<hex> signer_fp=<hex>`.

Proof: `bash scripts/ck_pi5_rc_check.sh` (CPU only, about 10 s, ends `PI5_RC_CHECK: PASS`). It needs the ceremony file
(`PI5_RC_OWNER_PUBKEYS=`, default the Spark path above). The repository keeps no owner file (native/kernel/README.md, "Real
owner files are never committed"), so in CI the steps that need it print SKIP; `AIENOS_STRICT=1` turns a SKIP into a FAIL.

### Why a new mode and not CK_HARDWARE_STAGING

`CK_HARDWARE_STAGING` is the switch that takes `CK_OWNER_PUBKEYS` today, but `svc/store_boot.c` refuses it together with the Pi
unlock (`CK_PI5_UNLOCK`), and that rule stays as it is: `store_boot.c` is not changed by this cut. So the RC mode never defines
`CK_HARDWARE_STAGING` and takes every protection that staging gives over explicitly, in five layers (native/kernel/Makefile,
"Pi 5 release candidate" block):

1. **Inputs.** Only `CK_PI5_RC CK_PI5_LIFECYCLE CK_PI5_UNLOCK CK_PI5_UNLOCK_CARD CK_INFER_LIB CK_OWNER_PUBKEYS OUT
   AIENOS_COMMIT CROSS HOSTCC` (`PI5_RC_INPUTS`) may come from the command line or an override, and no non-empty `CK_*`
   variable from the environment. Everything else is refused before anything builds, including `CK_EXTRA_CFLAGS`, `KCFLAGS`,
   `STAGE_DMA_FLAGS`, `ART_TRUST_FLAGS`, `CK_MACHINE_ID` and the `PI5_RC_*` variables themselves (all `override`, so the
   allowlist, the pin and the refusal list cannot be replaced from the command line). `make -e` is refused. `pi5-rc` must be
   the only goal and sets `CK_PI5_RC` and the lifecycle flags itself.
2. **Owner file.** `CK_OWNER_PUBKEYS` is required, must exist, and its sha256 must equal the pinned ceremony file
   (`PI5_RC_OWNER_SHA256`); `tools/ck_owner_gen.c --anchor` then refuses all-equal bytes and the RFC 8032 TEST 1/2/3 keys
   again and writes the anchor header into the RC's own OUT (never into the tree).
3. **Compile fence.** `native/kernel/include/ck_pi5_rc_guard.h` is force-included (`-include`) into every C and assembly unit
   of the image and stops the build with `#error` on any of 174 switches (list below), and also requires `CK_PI5_RC`,
   `CK_PI5`, `CK_PI5_UNLOCK`, `CK_PI5_UNLOCK_CARD` and `CK_PI5_LIFECYCLE` to be set.
4. **Flag scan.** When the makefile is read, the effective compile flags (`KCFLAGS`, `STAGE_KCFLAGS`, `ART_TRUST_FLAGS`) may
   hold no `-D` switch whose name contains TEST, MUTANT, MUTATION, UNSAFE, BYPASS, SELFTEST, STAGING, FWCFG or SEED0B.
5. **Image check after the link.** `scripts/ck_pi5_rc_image_check.sh` runs on the ELF and on BOOTAA64.EFI; on a failure the
   image is deleted. It requires: the owner key bytes exactly once and equal to the file; none of the RFC 8032 TEST 1/2/3
   public keys or secret seeds, nor the TEST-fixture owner key; no TEST Store identity text (the staging `owner_check` list:
   `AIENOS-LANE18-TEST-KVOL-NOT-SECRET`, `AIEN-TEST-BOOT01`, "TEST identity, TEST keys", "formatted TEST store",
   "TEST machine id 0xA1"), no TEST-ONLY / TEST ONLY banner, no SEED-0B qualification banner, no fw_cfg text
   (`opt/aienos/artifacts`, `opt/aienos/model`, `fw_cfg`, `QEMU0002`); no TEST key, TEST Store key or fw_cfg symbol in the
   ELF; the owner anchor announcement present.

The whole lifecycle block still applies on top (no other Pi tier, no `CK_SERVICE`, no `CK_SERVICE_TEST_KEYS`, no
`CK_TEST_FWCFG_ARTIFACTS`).

### Every staging protection and the test that proves the RC refuses it

Each row is a `$(error ... cannot be combined with CK_HARDWARE_STAGING)` rule or another staging check in
native/kernel/Makefile, plus a few test-class switches with no staging rule of their own.
"CL" = refused on the command line both through `make full CK_PI5_RC=1 ...` and through `make pi5-rc`; "guard" = stopped by
the compile fence; "image" = caught by the image check. All in `scripts/ck_pi5_rc_check.sh`.

| Staging protection (what it keeps out) | RC refusal | Tested by |
|---|---|---|
| TEST Store keys and TEST machine id (staging needs real owner files) | no `CK_MACHINE_ID`, no `CK_STORE_TEST_KEYS`; TEST Store identity text absent | CL, guard, image |
| `CK_SEED0B_TEST_ANCHOR` (TEST 1 artifact anchor) | refused; TEST 1/2 key arrays compiled out of `artifact/format.c` under `CK_PI5_RC` | CL, guard, flag scan, image (bytes and symbols) |
| `CK_TEST_FWCFG_ARTIFACTS` (QEMU fw_cfg artifact and model source) | refused | CL, guard, flag scan, image (text and symbols) |
| `CK_QEMU_UNSAFE_DMA` (unconfined DMA) | refused | CL, guard, flag scan |
| `CK_TEST_XHCI_MUTATION` (xHCI DMA fence mutation) | refused | CL, guard |
| `CK_TEST_DISK_XLATE_BYPASS` (disk translation bypass mutation) | refused | CL, guard |
| `CK_TEST_SMP_SKIP_CPU` (SMP mutation) | refused | CL, guard |
| `CK_B7B_PROBE_TEST_SEGMENT0` (QEMU positive path) | refused | CL, guard |
| `CK_TEST_STALE_HANDOFF` (stale handoff) | refused | CL, guard |
| `CK_TEST_ROLLBACK` (rollback candidate) and its variants | refused (also as `KCFLAGS=-DCK_TEST_ROLLBACK=1`) | CL, guard, flag scan |
| `CK_TEST_SLOT_KEY` (TEST-ONLY slot loader key) | refused (the guard also stops `CK_SLOT_UPDATE_KEYS`) | CL, guard |
| `CK_RUST_LIBS` (FP probe, QEMU gate only) | refused | CL |
| `CK_CONSOLE_SESSION` (QEMU test console session) | refused | CL, guard |
| `CK_TEST_CONSOLE_MUTATION` (console session mutation) | refused | CL, guard |
| `CK_SERVICE_TEST_KEYS` (TEST keys) | refused | CL, guard |
| `CK_TEST_STORE_CRASH`, `CK_TEST_STORE_CRASH_MUTANT` (Store crash hook) | refused | CL, guard |
| `CK_TEST_CONTINUITY`, `CK_TEST_CONTINUITY_MUTANT` (continuity mode selection) | refused | CL (guard for `CK_TEST_CONTINUITY`) |
| `CK_OSC_LAUNCH_TEST`, `CK_OSH_TEST`, `CK_OSH_OP_TEST` (test-only OSC/OSH adapters) | refused | CL, guard |
| `CK_HARDWARE_STAGING` itself (store_boot.c rule) | refused | CL, guard, flag scan |
| any other CK_* switch (tested: `CK_EXTRA_CFLAGS`, `KCFLAGS`, `STAGE_DMA_FLAGS`, `ART_TRUST_FLAGS`, `CK_SERVICE`, `CK_PI5_STORE`, `CK_PI5_ARTIFACT`, `CK_ARTIFACT_CARD`, `CK_PI5_PRODUCTION`, `CK_PI5_AGENT`, `CK_PI5_FACT`, `CK_PI5_NET`, `CK_FB_HOLD_S`) | not in `PI5_RC_INPUTS` | CL (and the guard for the test-class ones, e.g. `CK_ARTIFACT_CARD`, `CK_ART_STORE_WRITER`, `CK_FPU_PROBE`) |
| owner file: TEST keys refused by `ck_owner_gen` | pinned ceremony file, `--anchor` refusals | make refuses a missing owner file, the TEST-fixture file and the ceremony file with one hex digit changed; `ck_owner_gen --anchor` refuses TEST 1, TEST 2, TEST 3, all-zero, empty and short files and writes no header |

Other results of the check (last run, PASS, 0 skipped): all 174 guard switches stop the preprocessor one by one; every
test-class name found in `native/` sources is in the guard (coverage check, so a new mutant flag fails the check until it is
added); a switch smuggled past layer 1 through an extra makefile (`-DCK_FPU_PROBE`) is stopped at compile time; six smuggled
`-D` switches (`CK_TEST_ROLLBACK`, `CK_HARDWARE_STAGING`, `CK_QEMU_UNSAFE_DMA`, `CK_TEST_FWCFG_ARTIFACTS`, `CK_SEED0B_TEST_ANCHOR`,
`CC_MUTANT_SKIP_D1`) are refused by the flag scan; every compile line of the RC build carries the guard; the RC ELF and EFI pass
the image check while three controls fail it (the plain lifecycle image, the RC image with one owner key byte flipped, the RC
image checked against the TEST-fixture owner file); two RC builds in separate OUT directories give the same BOOTAA64.EFI bytes.

Guard list (`ck_pi5_rc_guard.h`, generated from the sources: every CK_/CC_/CM_/CR_/CS_/RC_ name containing MUTANT,
MUTATION, TEST, UNSAFE, BYPASS, SELFTEST, STAGING or PROBE, except `CK_INFER_PROBE`, which the lifecycle image itself uses,
plus six named by hand): CC_MUTANT_ACCEPT_RESERVED, CC_MUTANT_CHILD_INDEX_LE, CC_MUTANT_MANIFEST_LAST_WAL_ZERO,
CC_MUTANT_SKIP_D1, CC_MUTANT_SKIP_D2, CC_MUTANT_SKIP_TAKE_BOUND, CC_MUTANT_TRUNCATE_AT_CAP, CC_MUTANT_UNCHECKED_FORK_SUM,
CK_ACARD_MUTANT_TRAILING, CK_AGENT_CMD_MUTANT_ANY_VERB, CK_AGENT_MUTANT_MODEL_FIRST, CK_AGENT_MUTANT_NO_COMMIT,
CK_AGENT_MUTANT_NO_GOLDEN_CHECK, CK_AGENT_MUTANT_NO_PROMPT_CHECK, CK_AGENT_MUTANT_RECOVER_BLIND,
CK_AGENT_MUTANT_RECOVER_NO_CHECK, CK_AML_MUTANT, CK_ARTIFACT_CARD, CK_ART_STORE_WRITER, CK_B7B_PROBE,
CK_B7B_PROBE_TEST_SEGMENT0, CK_CONSOLE_SESSION, CK_CONT_SERVICE_HOST_TEST, CK_CTX_HOST_TEST, CK_CTX_MUTANT_SKIP_BINDING,
CK_DMAW_MUTANT_ (prefix), CK_DMAW_MUTANT_NO_ALIGN, CK_DMAW_MUTANT_NO_END, CK_DMAW_MUTANT_NO_FRAMECAP, CK_DMAW_MUTANT_NO_GOT,
CK_DMAW_MUTANT_NO_INDEX, CK_DMAW_MUTANT_NO_LATCH, CK_DMAW_MUTANT_NO_STALE, CK_DMAW_MUTANT_NO_ZERO, CK_DMAW_MUTANT_WRAP,
CK_FACT_MUTANT_NO_APPEND, CK_FACT_MUTANT_RECALL_BLIND, CK_FPU_PROBE, CK_GEM_MUTANT_ (prefix), CK_GEM_MUTANT_BME_EARLY,
CK_GEM_MUTANT_BME_LEFT_ON, CK_GEM_MUTANT_MDIO_OP, CK_GEM_MUTANT_NO_ADDR64, CK_GEM_MUTANT_NO_BARRIER,
CK_GEM_MUTANT_NO_BME_VERIFY, CK_GEM_MUTANT_NO_DIFF, CK_GEM_MUTANT_NO_DRFCS, CK_GEM_MUTANT_NO_FRAMECAP, CK_GEM_MUTANT_NO_GBE,
CK_GEM_MUTANT_NO_HWWIN, CK_GEM_MUTANT_NO_LLB_CLEAR, CK_GEM_MUTANT_NO_QUIESCE_WAIT, CK_GEM_MUTANT_NO_RETE_OFF,
CK_GEM_MUTANT_NO_RXLEN_CHECK, CK_GEM_MUTANT_NO_SENTINEL_USED, CK_GEM_MUTANT_NO_SEQ, CK_GEM_MUTANT_NO_TIEOFF,
CK_GEM_MUTANT_NOT_STICKY, CK_GEM_MUTANT_NO_TX_LAST, CK_GEM_MUTANT_NO_VALIDATE, CK_GEM_MUTANT_NO_WRAP, CK_GEM_MUTANT_PHYLB_BIT,
CK_GSP_BINDATA_EXTRACT_SELFTEST, CK_GSP_BOOT_ARGS_ENCODE_SELFTEST, CK_GSP_COT_ENCODE_SELFTEST, CK_GSP_FW_INSPECT_SELFTEST,
CK_GSP_RPC_ENCODE_SELFTEST, CK_HANDOFF_MUTANT_SKIP_RESERVED, CK_HARDWARE_STAGING, CK_HOST_TEST, CK_IORT_MUTANT_ (prefix),
CK_IORT_MUTANT_OTHER_BLIND, CK_IORT_MUTANT_OVERLAP_OK, CK_IORT_MUTANT_RMR_BLIND, CK_IORT_MUTANT_RMR_EXCLUSIVE,
CK_IORT_MUTANT_ROUTE_FIRST, CK_IORT_MUTANT_SEGMENT_BLIND, CK_IORT_MUTANT_TRUNCATE, CK_LAYOUT_PROBE_UNITS,
CK_NET_MUTANT_ (prefix), CK_NET_MUTANT_BAD_CLASS, CK_NET_MUTANT_NO_ASSERT, CK_NET_MUTANT_NO_BOUNDS,
CK_NET_MUTANT_NO_CLK_VERIFY, CK_NET_MUTANT_NO_DISABLE_FIRST, CK_NET_MUTANT_NO_EXACT_DIV, CK_NET_MUTANT_NO_HIGH_FIRST,
CK_NET_MUTANT_NO_HOLD, CK_NET_MUTANT_NO_IDLE_AFTER, CK_NET_MUTANT_NO_MDC_DIV, CK_NET_MUTANT_NO_MPE, CK_NET_MUTANT_NO_OTHERS,
CK_NET_MUTANT_NO_QUIESCE_CHECK, CK_NET_MUTANT_NO_READBACK, CK_NET_MUTANT_NO_RELEASE, CK_NET_MUTANT_NO_REMAP_EN,
CK_NET_MUTANT_NO_RESTORE, CK_NET_MUTANT_NO_STATUS_MASK, CK_NET_MUTANT_NO_STRADDLE, CK_NET_MUTANT_OB_NO_LIMIT,
CK_NET_MUTANT_OUI_ORDER, CK_NET_MUTANT_PARTIAL_ANY, CK_NET_MUTANT_TOUCH_ON_CLOCK, CK_NVME_DMA_BYPASS, CK_NVME_UNSAFE_BYPASS,
CK_OSC_LAUNCH_TEST, CK_OSH_OP_MUTANT_SKIP_PERM, CK_OSH_OP_TEST, CK_OSH_TEST, CK_PCIE_MUTANT_ (prefix),
CK_PCIE_MUTANT_NO_ALIGN, CK_PCIE_MUTANT_NO_FULLCOVER, CK_PCIE_MUTANT_NO_INDEX, CK_PCIE_MUTANT_NO_MERGE,
CK_PCIE_MUTANT_NO_OUTSIDE, CK_PCIE_MUTANT_NO_REMAP, CK_PI5_PRODUCTION, CK_PI5_STORE, CK_QEMU_UNSAFE_DMA, CK_RNG_UNPROBED,
CK_SB_E_TEST_KEYS, CK_SEED0B_TEST_ANCHOR, CK_SELFTEST_EL2_FAULT, CK_SERVICE_TEST_KEYS, CK_SLOT_TOOL_SELFTEST,
CK_SLOT_UPDATE_KEYS, CK_SMMU_MUTANT_ (prefix), CK_SMMU_MUTANT_BAD_L1_PTR, CK_SMMU_MUTANT_L2_NO_FILL,
CK_SMMU_MUTANT_SPAN_OVERFLOW, CK_SMMU_MUTANT_UNGRANTED_VALID, CK_STORE_TEST_KEYS, CK_TEST_CONSOLE_MUTATION,
CK_TEST_CONTINUITY, CK_TEST_DISK_XLATE_BYPASS, CK_TEST_FWCFG_ARTIFACTS, CK_TEST_ROLLBACK, CK_TEST_ROLLBACK_BAD_MAGIC,
CK_TEST_ROLLBACK_CPU_FAULT, CK_TEST_ROLLBACK_HANG, CK_TEST_SLOT_KEY, CK_TEST_SMP_SKIP_CPU, CK_TEST_STALE_HANDOFF,
CK_TEST_STORE_CRASH, CK_TEST_STORE_CRASH_MUTANT, CK_TEST_STORE_MUTANT_ACCEPT_BAD_ROOT_CRC,
CK_TEST_STORE_MUTANT_SKIP_ROOT_FLUSH, CK_TEST_XHCI_MUTATION, CK_UEFI_VARS_SELF_TEST, CM_MUTANT_FIXED_AGENT,
CM_MUTANT_NO_WRITABLE_CHECK, CM_MUTANT_PROVISION_WITH_ROOT, CM_MUTANT_RESUME_NO_COMMIT, CM_MUTANT_RESUME_PROVISIONS,
CM_MUTANT_SPLIT_TXN, CR_MUTANT_BARE_SHA256, CR_MUTANT_CHALLENGE_NO_ACTION, CR_MUTANT_CHALLENGE_NO_DIGEST,
CR_MUTANT_COMPARE_16, CR_MUTANT_IGNORE_PREVIOUS, CR_MUTANT_NO_DOMAIN, CR_MUTANT_NO_ORPHAN_CHECK, CR_MUTANT_SKIP_CONFLICT,
CS_MUTANT_ACCEPT_FOREIGN, CS_MUTANT_ACCEPT_FORK, CS_MUTANT_ACCEPT_TWO_ACTIVE, CS_MUTANT_PROVISION_SPLIT_TXN,
CS_MUTANT_RESTORE_MINTS, CS_MUTANT_SKIP_INTENT_ID, RC_MUTANT_DEGRADED_IS_UNPROVISIONED, RC_MUTANT_INSPECT_WRITES,
RC_MUTANT_PROVISION_QUALIFICATION, RC_MUTANT_REPAIR_ACTIVE, RC_MUTANT_REPAIR_NO_IDENTITY.

### Owner-key verification path (host test, no owner signature anywhere)

Embedding a public key proves nothing by itself, so `native/kernel/tests/test_pi5_rc_anchor.c` (`make -C native/kernel
test-pi5-rc-anchor`, run by the check) is built against the same generated anchor header and `core/artifact_anchor.h`
selection as the image, and calls the same `cka_verify` / `cka_boot_policy` / `cka_evaluate` as the kernel loader. Every
artifact in it is signed by a THROWAWAY key derived inside the test (public key `e02c14dd...1b890ce28e`, sha256 of a fixed label)
or by the public RFC 8032 TEST keys. Result with the ceremony file: PASS, 19 checks:

- the compiled anchor equals the `owner_root_ed25519` line of the ceremony file, is the only anchor, and is no RFC 8032 TEST key;
- artifacts signed by the throwaway key, by TEST 1 (the SEED-0B anchor) and by TEST 3 are REFUSED (`UntrustedSigner`), and the
  boot policy built from the anchor names only the owner fingerprint and refuses the throwaway and TEST 1 signers at admission;
- a forged artifact naming the owner key's fingerprint with a throwaway signature is REFUSED (`BadSignature`);
- positive control: with the throwaway key as the anchor the same artifact verifies and a one-byte payload flip is refused, so
  the refusals above come from the anchor and not from a broken artifact;
- red control: built with the throwaway key in place of the owner file, the test FAILS (3 checks), as it must.

Not exercised, by design: an artifact actually signed by the owner key (its private half never leaves the YubiKey and nothing
was signed in this cut). On the Pi the RC image has no artifact source with candidates (`CK_ARTIFACT_CARD` is TEST-anchor only
and refused, `CK_ART_STORE_WRITER` is refused), so a Q5 run shows the owner anchor announcement and zero candidates.

### What the RC does not change

- `svc/store_boot.c` is untouched; `CK_HARDWARE_STAGING` is never defined in the RC.
- The Store unlock stays the PI5-4 qualification tier (card request file, throwaway secret; receipt tier label
  `PI5-UNLOCK-QUALIFICATION`); continuity stays at its qualification tier.
- The OSC owner anchors (`core/osc_admit.c`) stay unprovisioned until TRUST-1, and the slot loader keys are not provisioned
  either: only the artifact loader takes the owner key.
- No image other than the RC changes: the anchor selection moved from `core/artifact_loader.c` into `core/artifact_anchor.h`
  with the same result for every existing image, and the TEST key arrays leave `artifact/format.c` only under `CK_PI5_RC`.
- When a later cut (for example the ALLEN kernel link) adds a new build switch to the lifecycle image, that switch must be added
  to `PI5_RC_INPUTS` and the `pi5-rc` rule, or the RC refuses it; a new test-class switch must be added to the guard, or the
  coverage check fails.

The Q5 run plan for this image on the Pi: `docs/native/pi5-8/Q5-RUN-PLAN.md` (not run yet).

## Limits

- All Pi evidence so far is one Pi, one card, TEST keys. The manifest is unsigned for real: only a TEST signature was made. The owner-key ceremony is done (public key in the RC image), but nothing has been signed with the owner key yet.
- The RC image is built and checked on the Spark only; it has not run on the Pi (Q5, `docs/native/pi5-8/Q5-RUN-PLAN.md`). CI cannot build the real RC because the repository keeps no owner file: there the steps that need it SKIP.
- Embedding the owner public key is not signed boot: nothing in this cut makes the Pi firmware check BOOTAA64.EFI against the owner key, the Store unlock is still the qualification card file, and owner signing cannot provide hardware freshness (a same-disk anchor cannot detect a whole-disk rollback; `svc/store_boot.c`).
- Run folders live on the Spark (`~/workspace/pi5-runs`), not in the repository; the manifest records their hashes.
- The Pi image tier change affects every Pi image that links the artifact loader (none of the other Pi images presents candidates).
