# L768 independent L765 bounded surface review

## Verdict
No source-level defect found in the bounded RGB/text surface within this review scope. Focused and independent native Omega checks passed. This is not hosted application, live inference, display-device, full layout/animation, or iMac acceptance.

## Frozen source and closure
- Source pin: f52f17a9e9520c1d16ba4d30aa8ee3bda8ee12a5; base: 92b405477cfdff40dda25c5afcac5b65cb04122a.
- Reviewed handwritten source/docs: native/omega/allen/surface.om, SURFACE.md, tests/surface.om, tests/surface-mutants.om, tests/surface-render.om, gate.sh and font-render.om reuse. Existing font/provider/presentation source reviews were reused as requested.
- Reconstructed the 20-file build closure from a fresh Git archive of the pin. All 20 working snapshot file blobs matched their exact pinned Git object IDs; see closure.status, consumed.paths, consumed.sha256. Repository working tree was clean at inspection.
- Pinned compiler SHA-256: 191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9.

## Execution receipts
- Rebuilt focused surface test from the frozen archive using native/omega/allen/build-one.sh; process exit 0 (85 assertions per the test source). ELF is nonempty static AArch64 ELF64 EXEC. Binary SHA-256 5a3a813ab4cfcef486b7920563d4b3fe615edafbf269ce6d8ed1796fb01fb230.
- Added four bounded Omega assertions to a private copy of tests/surface.om: valid zero-area edge clip; clipped pixel write preserves Canvas/Cv after admitted clip; maximum unsigned width is refused; refusal preserves Canvas/Cv. Rebuilt and executed the exact 89-assertion program, exit 0. ELF SHA-256 8d5e1c03af491bc89e629c10d4df3bb87a8fbc4ec942254e717d79d3bcf79ce9; added source is out/probe-v2.om, full concatenation out/probe-v2.full.om.
- The source's own tests cover signed negative origins/fractional advance, overlap and integer blending, canvas/clip boundaries, public refusal state preservation, and retained span parity. Existing exact-pin evidence covers 85/85 focused, 2831 full assertions and 24 compiled mutants; not rerun here. Live inference was not exercised.

## Findings / preserved failed attempts
- No actionable source finding. Guard ordering validates initialization/dimensions and uses subtraction for clip fit before state writes; pixel bounds are tested before unsigned offset calculation; blend reads the current destination; span bounds are checked before drawing and public refusal tests preserve Canvas/Cv.
- Exploratory attempts are retained without overwriting: out/independent.om + build log (compiler error 7, unchecked assignment form); out/independent-v2.om + build log (compiler error 4, unsupported probe form); out/probe-test.om / out/probe (the extra comparison initially asserted unchanged Cv after a valid clip, so assertion 87 failed). The corrected probe is separately named probe-v2 and passes. These are probe-authoring errors, not product failures.

## Immutable receipts
- Exact command output files and SHA-256s are listed in receipts.sha256; the final report hash is in report.sha256.
- Private artifacts are under ~/handoffs/L768-review/. No production source was modified; no deployment, push, service action or ledger closure performed.
