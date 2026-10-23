# L746 restored memory to real application jobs

Based on a65b70e. Handwritten seeded Omega context.om and tests/context.om;
appask now validates/stages context before jobbuild reads variable-layout names,
then appends memory metadata before the actual qsubmit snapshots the wire job.
No disk, networking, live model or target provider was added.

Initial old-application RED (`red`) trapped103 after62 checks at deliberately
invalid fact count33: self-name offset3769 exceeded Mem capacity3692. Preserved
as the old unchecked application path, not a complete assertion-suite result.
After connecting compilable context stubs, RED (`red2`) completed84 assertions,
46 failures,exit46. Implementation passed84/84. Expanded tests ran against the
same stub:142 checks,76 failures,exit76 (`red3`); real implementation then passed
142/142 (`green3`). Initial implementation compile error3 was unsupported `mod`;
corrected to the seed grammar's `rem`, not counted as behavioral evidence.

Coverage includes actual memory codec load, note record validation/loading in
arbitrary order, duplicate/damaged/deleted handling, incomplete catalog refusal,
newest-six/zero-note selection, literal metadata bytes, context carried inside
actual appask/qsubmit JOB, exact4096-byte append and overflow refusal, maximum
counts, malformed fields, u32 decimal starts, preserved Unit/Enc, and untouched
conversation/wire IDs and history on refused admission. Metadata-only and
complete-JOB overflow are tested separately. No silent dropping or truncation.

Fresh complete combined gate:
`quiet-build native/omega/allen/gate.sh ~/handoffs/allen-omega-L694/L746/full`.
Exact totals and13 rejected compiled mutants in counts.json/full.log. Original
memory/stdio goldens and oversized-stdio refusal remain passing. Input inventory
was verified byte-for-byte before commit. Independent review pending for L746;
newest-source whole-repo strict/hardware qualification is not claimed.
