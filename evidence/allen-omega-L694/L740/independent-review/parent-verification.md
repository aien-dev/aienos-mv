# L744 parent verification

No correctness finding at exact a65b70eb99a4d132c8c1f657f5cbb2a2fc1cd81d.
All16 exported source files match pinned Git bytes. Independently reconstructed
final probe concatenation: base, vendor/sha256-core, runtime, code, courier,
scheduler, memory, intent, reply, view, scene, scene-input, tests/check, then
probes/scene-independent-v3.om. Reported bundle and ELF hashes match actual bytes.
Parent rebuilt final probe byte-identically and ran2084/2084 assertions,exit0.
The published63-check scene-input suite also remains green in the current full
gate. Scene product/test files are unchanged from the reviewed pin.

Original v1 expected two hidden lines but its fixture has three (opening line,
inside line, closing line). The already-published contract hides all three;
corrected v2 and added-boundaries v3 did not alter production. Original failure,
probe revisions and report are preserved. This clears only L740/L744 scene core,
not new L746 context, application/display attachment or target qualification.

Report SHA:e3ca62be1d5876da089c86852874f380c6c9876bf7d9e1a375ffc049804ef421.
Bundle SHA:2c1c00919db88b0b40e294b23f4d4431d1d3638e2c9eb2e371c194a8e5c9886e.
ELF SHA:9c47cbb650c222bf0ac310664c00b316638dbf85dc6f32621f799142b8dfc94b.
