# Native SHA256 reuse

Exact upstream `seed/native/sha256.om` from omega-osc-manual-migration commit
8f8be9a676b94197e5cb363d2dcbca113e208f09, last file commit at read-only checkout
9b072eb492e106d23f7eed984c5645251601adee. Its core comes from native Omega TRN1
12251e3403d42931ada27dc1c54b3b610954be5d. `sha256.om` preserves that whole file.
`sha256-core.om` is its byte-exact prefix before the handwritten standalone stream
interface comment (no algorithm changes). Build-time concatenation is not a new
language import/compiler feature. No primitive was reimplemented. Qualification
here includes independently constructed SHA-sealed record bytes and hash mutants.
The upstream AGPL licence is preserved as OMEGA-LICENSE. Imported sources retain
SPDX identifiers. No keyed authentication/freshness claim follows from these tests.

Compiler is immutable omega-bridge-native-development commit
0fc7d8662cb799e7c206b17c18ea4c3ec2b8fea9, omc SHA256
191b776ef98821597c3e30655da6d680a9b66edbc9ceaca3c39c61ea6df098e9.
Fresh source-only bootstrap from git archive at that pin passed, stage1==stage2,
and its omc hash matched the brief's compiler. No C parent or compiler edits.
