# L764 independent provider/presentation review

**Verdict: no scoped source defect found.** Review is against exact HEAD `92b405477cfdff40dda25c5afcac5b65cb04122a` with base `61ef587ee5d24bf862705f51bffb8039c9a2ed01`. The production checkout remained clean and unchanged.

## Source review

- Reviewed the narrow six-file delta: `PROVIDER.md`, `README.md`, `gate.sh`, `provider-core.om`, `tests/provider-core.om`, and `tests/provider-display-mutants.om`. `git diff --check` is clean.
- `provider-core.om:205-211` keeps CANCEL on `appcancel`, uses `dreset` for RESET; `:213-214` refuses invalid clock before stateful dispatch; `:231-243` selects `dask` for ASK and `drecv` for RECV, returning refusal before `prevent`/accepted status when either call refuses. The call order preserves the existing provider frame and output semantics.
- `gate.sh:10-14` places `presentation.om` before `provider-core.om` for seed definition-before-use. Gate additions at `:138-153` create three separate Omega mutants, each bypassing one attachment call.
- Read `PROVIDER.md` and `PRESENTATION.md`. The expected contract is retained D* projection only on admitted events; CANCEL retains state, RESET clears it, and code snapshots are independent of reusable assembly storage. No new wire operation/record or pixel/device claim is introduced.

## Independent verification

- Exported `native/omega/allen` from the exact Git object into `~/handoffs/L764-review/export`. Recomputed all 63 entries in the L763 source manifest from Git blobs and the export. All hashes match. Membership comparison confirms all 57 tracked `.om`/`.sh` source/build files under the component tree are in the manifest; six remaining manifest entries are other file types. Source receipt: `closure.log`.
- Rebuilt the focused provider-core Omega test from the exported source using the exact pinned compiler (SHA-256 `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9`). Build returned 0; produced nonempty AArch64 ELF64 `EXEC`; executed 136 assertions, 0 failures, exit 0.
- Authored and ran a separate seeded-Omega lifecycle probe. It covers BOOT/ASK/POLL/SENT/RECV display projection, CANCEL retention, duplicate BOOT refusal, clock refusal, malformed RECV refusal with last-good display retained, and RESET clearing retained metadata/text. Build returned 0; produced nonempty AArch64 ELF64 `EXEC`; probe output `34 0`, exit 0.

## Raw receipts and hashes

All paths below are under `~/handoffs/L764-review/` on Spark.

- Closure: `closure.log` SHA-256 `acd672fdc4ae3434fa8d7bdb313803c85b98522e23fb6493457fee925800f2c0`; per-input source digests: `source-hashes.sha256` SHA-256 `305b12af3f8e5d1087778b7fe423221c30f7be784eaa348536d9563d6935e230`.
- Focused provider: source `run/provider-focus.om` SHA-256 `db32a876b46a90be0fc4c5181dfc4af0420aff2b5a4dd323765cd4d9eaf76222`; ELF `run/provider-focus` SHA-256 `ef1a108242bf2d963f2f93d057eb0efb1776f3510824ba517f96bfeacb96689f`; build log `run/provider-focus.build.log` SHA-256 `7cf0d9627d797238980dd2af938c370dbd0e36326ddb3dbf9edd0ef8174ba1e3`; raw result `run/provider-focus.log` SHA-256 `3eb3f819d84ee4b044ca462072cbbe83714e75b7574d0ab3beedb4cc1de8c408`.
- Lifecycle probe: immutable input `lifecycle-v3.om` SHA-256 `aa6c0dda358b55999b81350b893dede9fbbe1b43ba0d794480b3720ae0ebfbba`; combined source `run/lifecycle-v3.om` SHA-256 `5bd44701d8bd97ee04e63ae58f3dac0718e67e0766437498662ed51bd9996cf4`; ELF `run/lifecycle-v3` SHA-256 `c1b25a69bab4ea86076dab5b37f48a4431c87e2a8752791fe50c96da0d720c64`; raw result `run/lifecycle-v3.log` SHA-256 `8ebec9aeab32e4e47f9749d0e0c5979863f447625b089e2514d73bdd64b2824c`; exit receipt `run/lifecycle-v3.exit` SHA-256 `9a271f2a916b0b6ee6cecb2426f0b3206ef074578be55d9bc94f6f3fe3ab86aa`.
- Combined final probe hash inventory: `lifecycle-v3.sha256` SHA-256 `0df81acc47911c4b91a63cb12fdc0ab770b1eb59f40f9edd135f1c6aafffcf3a`.

## Scope and issues

This verifies source closure, focused native provider behavior, and the bounded lifecycle/refusal probe only. It does not replay the full native suite/mutants, nor qualify pixels, live sockets/disk, x86/EFI, QEMU, device behavior, or physical iMac acceptance. No source finding is being reported. Two host-orchestration attempts failed before behavioral execution (a relative build path from the wrong cwd and an unsupported shell process-substitution/quoting form); both were corrected, and the final build/probe receipts above are from successful executions. An earlier independent probe draft is preserved separately as `lifecycle-v2.om`; it is not the final probe receipt.
