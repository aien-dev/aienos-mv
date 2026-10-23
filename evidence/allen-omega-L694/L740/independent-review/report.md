# L744 independent review — L740 native Omega scene decoder

## Verdict

No correctness finding in the reviewed `scdecode` scope at `a65b70eb99a4d132c8c1f657f5cbb2a2fc1cd81d`. The implementation behaved as documented on the focused upstream test and independent boundary/recovery probes below. This does not approve application attachment, full Allen parity, target UI, hardware, service, or deployment.

## Immutable source and execution closure

- Spark checkout: `/home/drakestapleton/workspace/aienos-allen-omega-L694`
- Exact HEAD: `a65b70eb99a4d132c8c1f657f5cbb2a2fc1cd81d`; parent: `8d0d11babf7cc3b57ee000a265aeda9fe531ebf0`; checkout porcelain status count: `0`.
- Exported required source using `git archive <pin> ... | tar -x` under `/home/drakestapleton/handoffs/L744-review/source`. `git hash-object` matched pinned Git blobs for `scene-input.om` (`48c58ce8702d5108bfe4114ed53e363195f7e578`), `tests/scene-input.om` (`51ef9bf7259a9d41625f6cca20fe60fbc1240a5f`), strict `scene.om` (`376fd36150c85bd2e9774e8e7a1ef7204911457d`), `README.md` (`dfaa4d1cce2cdcc3c7e4c1d77f2aecc6cca3b111`) and `build-one.sh` (`c2c303dbb96bc3f9e7449983ce30cf39e64cbd55`).
- Pinned compiler `/home/drakestapleton/handoffs/allen-omega-L694/omc`, SHA256 `191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9`. Each binary was built by exported `build-one.sh`, which checked that hash and required nonempty static AArch64 EXEC output. `readelf` confirmed `Type: EXEC` and `Machine: AArch64` for both.

## Executed probes and receipts

All new executable probe logic is handwritten Omega; shell only assembled source, invoked the pinned build, ran binaries and collected logs.

1. **Focused pinned test, independently rebuilt:** concatenated exported dependencies, strict `scene.om`, `scene-input.om`, `tests/check.om` and pinned `tests/scene-input.om`; ran `build-one.sh` then `outputs/upstream-scene-test`. Exit `0`; 63 check lines, zero failure (`1`) lines. ELF SHA256 `4f4490e78b193ae6b1f4df12700db73dde18f7e7ffd5a9e3bfd76f5c5d873774`. Source bundle SHA256 `2f6b3345cf8e0633c0217d141929b2d961b044f78a258c46ead9a0d106885151`.
2. **Independent Omega probe:** `probes/scene-independent-v3.om`; checks mixed-case thought tags and hidden-line behavior, visible mixed-case text, exact caller input/length restoration, malformed known `number:` refusal without fallback, plain-text fallback, 2047-byte input normalizing to exactly 2048 bytes, and 2048-byte unterminated input whose appended newline overflows normalization (full refusal and cleared metadata). Rebuilt from exported source and ran `outputs/scene-independent-v3`; exit `0`, 2084/2084 assertions pass. ELF SHA256 `9c47cbb650c222bf0ac310664c00b316638dbf85dc6f32621f799142b8dfc94b`; complete Omega source bundle SHA256 `2c1c00919db88b0b40e294b23f4d4431d1d3638e2c9eb2e371c194a8e5c9886e`.
3. **Preserved probe iteration:** v1 expected two hidden lines for `<THINK>secret\nsay: hidden\n</tHiNk>`, but actual was three: the opening-tag line, enclosed line and closing-tag line are all hidden. This was a mistaken probe expectation, not a product failure. v1 compiled to a valid executable and failed only check 1 (`outputs/scene-independent.log`, exit 1). Corrected v2 changed only expected hidden-line count to 3 and passed. v3 adds exact 2047/2048 normalization-boundary assertions. All revisions remain in `probes/` and corresponding `outputs/`.

## Source review notes

- `scene-input.om` locations: `nmatch` line 20; bounded `nemit` line 31; thought/fence/canonical-line handling in `nprocess` line 126; malformed-known versus prose fallback in `nappend` line 187; metadata reset in `nclear` line 232; snapshot/normalize/parse/restore transaction in `scdecode` line 241. README contract/attachment caveat: lines 76–95.
- `scene-input.om`: `nmatch` performs ASCII case-folded byte-exact matching of `<think>` and `</think>`; `nprocess` hides tag-bearing lines and all lines while the state is open, including the unclosed-to-end case. The probe confirms mixed-case tags do not expose enclosed text.
- `nemit` bounds normalized output before writing; overflow sets `Nm[6]`, and `scdecode` clears scene metadata instead of parsing partial output. Probe observed 2047 raw bytes plus synthesized LF => exactly 2048 normalized bytes, while 2048 raw bytes without LF are refused because normalization would require byte 2049.
- `scdecode` snapshots bytes before normalization, publishes normalized bytes only during `scene` / fallback parsing, then restores all `n` input bytes and `Inlen`; malformed-known-command, fallback and overflow calls were followed by checks of unchanged input and refreshed metadata.
- `nprocess` tracks only the first 12 nonempty eligible canonical lines; hidden thought lines and fence lines return before incrementing that count. Pinned test covers canonical-line fallback/mood cap; this review did not rerun the full Allen suite.
- `nappend` keeps malformed recognized structured keywords out of prose fallback while allowing the first short unknown line if no recognized scene is accepted. Independent malformed-number and plain-text cases passed.
- README explicitly says this API is not yet attached to the application reply/display provider; that integration and full visual/target parity remain unqualified.

## Commands / raw receipts

Compiler/build pattern (run on Spark):

```sh
<exported-source>/native/omega/allen/build-one.sh <source-bundle.om> <fresh-output-elf>
<fresh-output-elf> > <output-log>
```

Artifacts, Omega probe revisions, binaries and logs are retained under `/home/drakestapleton/handoffs/L744-review/{source,probes,outputs}`. Parent's prior full 1337-check / mutant evidence was not rerun; this review re-executed only the focused scene-input test and new focused probes.
