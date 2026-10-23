# Sealed-store independent crypto/recovery qualification (#445)

NOT A MASTER PLAN. Base18e276590424593647cd7c39c6d16c2f9f8d6027. Reuse native/store tests, file-backed synthetic disk rig, and the existing M5 AES-GCM-SIV implementation. No volume, device, TPM, key rotation or disk-format change. Existing fix/aienux-crypto-speed branch/PR ownership is untouched. New C is only qualification of the existing native C ABI, not a new production component.

The production derivation is already plaintext/metadata-bound. This work adds independent known expected envelope-id/nonce-prefix bytes for actual ss_prepare output. Nine fixed cases vary store UUID, generation, object position, kind, version, key generation, identity class and plaintext length/content. Domain key is synthetic0x23*32. Expected bytes use independent stdlib SHA256/HMAC and the specified one-block HKDF-Expand construction, never envid_derive output. Each fixture records exact key scope/object/chunk mapping: generated 16-byte envelope identity feeds the object key, generated8-byte prefix plus existing chunk index feeds the per-object chunk nonce. Nonce byte equality across unrelated keys is not a safety criterion. Finite samples are regression controls, not a cryptographic uniqueness proof.

Existing store_sealed_test already tests both512/4096-byte blocks, store-only rollback, anchor-only one-behind prepared advancement, two-behind rejection, same-counter fork rejection, anchor missing/corruption, store/anchor commit crash boundaries, torn-slot write outcomes, identical-position deterministic rewrite, changed plaintext/kind, legacyV1 rejection and wrong-envelope output wiping. Those tests remain intact. The new known-answer fixture is an independent check, not a claim that current production had a new derivation defect.

## Separate properties

| Boundary | Evidence | Meaning |
|---|---|---|
| Structural integrity | Existing Store and powercut tests | Crash-consistent bytes without keys |
| Authentication | Existing forged records, envelopes and M5 primitive vectors | Authorized bytes and no unauthenticated output |
| Nonce-misuse safety | AES-GCM-SIV vectors plus V2 derivation fixture | Primitive-specific behavior and binding; not a uniqueness proof |
| Replay | Store older than retained anchor refused | Requires retained trustworthy anchor |
| Crash consistency | Store/anchor boundary tests | Valid committed/prepared states recover |
| Whole-image freshness | Whole store+anchor rollback mounts successfully | **Documented limitation**, NOT freshness PASS; external TPM/monotonic authority required (#32) |

`store_sealed_test` explicitly asserts the last limitation. It must not be removed or inverted.

## Reproduction

`make -C native/store OUT=out-crypto out-crypto/store_sealed_test`

`native/store/out-crypto/store_sealed_test native/store/out-crypto`

`python3 scripts/crypto_reuse_mutants.py <private-output>`

The external serial driver compiles mutants before running, requires normal exit1 and the intended CHECK failure and complete STORE_SEALED:FAIL summary. Missing compiler, compilation failure, crash/signal and timeout are INVALID. Mutants remove plaintext binding, kind binding, restore predictable V1 identity derivation, remove typed V1 refusal, bypass rollback and bypass anchor consistency. Each matches a named independent/legacy assertion. Original source is restored afterward; final control is rebuilt. No broad mutation runner's any-nonzero rule is borrowed.

## Acceptance

- [x] Independent expected bytes from fixed fixtures, metadata coverage, six compiled intended-assertion mutants and2519-check owning recovery suite.
- [x] Whole-image rollback limitation, key scope and evidence boundaries preserved.
- [ ] Full existing Store/M5/crypto host suites including crosscheck/sanitizers and exact committed-source replay.
- [ ] Independent review and sole-desk PR qualification. Physical storage is not claimed.
