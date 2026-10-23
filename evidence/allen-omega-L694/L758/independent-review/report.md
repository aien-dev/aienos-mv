# L758 handwritten Omega font review

**Verdict:** no font-component defect found in the reviewed pin. This is a static-source and native-host test review, not UI/device acceptance.

## Scope and immutable source closure

Reviewed `native/omega/allen/font.om`, `font-data.om`, `FONT.md`, the focused font tests/output/render fixture and the small gate/build integration at `61ef587ee5d24bf862705f51bffb8039c9a2ed01` (base `4c7888f65dca209a1e2b859ba695ae308657c03e`). At closure verification, HEAD was the pin and the worktree was clean. All 26 declared compiler/test/integration sources were byte-equal to `git show <pin>:<path>`; their SHA-256 values and the check are in `receipts/closure.log` (`CLOSURE_OK files=26`). The separate legacy reference source hash is `92d7157386e65351325d80d5036565e406145230dfb696c9804ec80888d26904`; frozen oracle hash is `0dc4abaa8c8cc1c5a79772bdd00b2d74458beac6ccfe85637069d04c77a7ff69` (436341 bytes, 294 records). This supports external-reference provenance; the C oracle is not a native construction/runtime backend.

Pinned compiler: `~/handoffs/allen-omega-L694/omc`, SHA-256 `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9`. Focused ELF is verified ELF64, AArch64, static EXEC, with no INTERP/NEEDED. Its hash and tool details appear in `receipts/closure.log` / `receipts/focused.sha256`.

## Verification

Executed the focused test build through `quiet-build` and the exact-pin `native/omega/allen/build-one.sh`, then ran its ELF: **421 emitted assertions, all `0`, exit 0**. Recounted emitted lines rather than relying on the parent summary.

Added `probes/font-boundary.om`, handwritten seeded Omega, and compiled/run it with the same pinned build path: **308 emitted assertions, all `0`, exit 0**. It exercises the four inclusive/exclusive cap/weight boundary failures, valid extreme cap/weight settings, all 95 printable glyphs at maximum size/weight with mask-area and segment-count limits, zero-area space behavior, invalid `afinit` preserving all Af/Ab fields and a mask sentinel, and unsupported-byte fallback metric behavior. Raw probe source, concatenated executable input, ELF, output, and SHA-256s are retained under `probes/` and `receipts/`.

Source review: invalid `afinit` inputs return before modifying state (`font.om:46-51`); valid setup uses fixed globals and initializes the 91-entry table (`:53-70`). The segment writer rejects counts at 160 (`:121-135`), mask bounds are checked against the 65536-byte alpha buffer (`:215-235`), and unsupported input maps to `?` (`:72-90`). Existing focused tests also cover full-width integer sqrt, signed trig, printable glyph rendering, maximum settings, fallback and invalid-init state preservation (`tests/font.om`). Parent evidence records exact native/oracle stream equivalence and specimen inspection; those are cited as prior evidence, not re-run as a UI acceptance claim.

**Findings:** none, within this component scope. I did not independently compare every fallback mask byte to `?`; the parent’s frozen-oracle evidence and focused suite cover the broader mask equivalence claims.

## Commands and receipts

From `/home/drakestapleton/workspace/aienos-allen-omega-L694` on Spark:

```sh
quiet-build native/omega/allen/build-one.sh   ~/handoffs/L762-review/receipts/focused-font.om   ~/handoffs/L762-review/receipts/focused-font.elf
~/handoffs/L762-review/receipts/focused-font.elf   > ~/handoffs/L762-review/receipts/focused-font.log
quiet-build native/omega/allen/build-one.sh   ~/handoffs/L762-review/receipts/font-boundary.om   ~/handoffs/L762-review/receipts/font-boundary3.elf
~/handoffs/L762-review/receipts/font-boundary3.elf   > ~/handoffs/L762-review/receipts/boundary.log
```

The exact build concatenations are preserved as `receipts/focused-font.om` and `receipts/font-boundary.om`; command output/hashes are in `receipts/focused-build.log`, `focused-font.log`, `focused.sha256`, `boundary-build.log`, `boundary.log`, and `boundary.sha256`. Source/object membership receipts: `receipts/closure.log`.

Probe authoring had two compiler-fixture corrections, not product failures: an initial concatenation placed the check helper after the probe (`boundary-build2.log`, compiler error 3), then oversized extra global scratch arrays exceeded compiler storage (`boundary-final2-build.log`, error 13; corrected in final probe). Raw failed compile receipts are retained. The production worktree was untouched by this review; a later status check observed an unrelated concurrent modification to `native/omega/allen/tests/provider-core.om`, which was not inspected or changed.

## Limits

No full suite, 19-mutant rerun, provider/presentation re-review, deployment, service/agent restart, live transport, complete scene/layout, framebuffer/device, or physical iMac acceptance was performed.
