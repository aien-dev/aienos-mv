# L740 native scene-input normalization

Host-only seeded Omega component, based on8d0d11b. Invocation on Spark:
`quiet-build native/omega/allen/gate.sh ~/handoffs/allen-omega-L694/L740/full`.
All new executable fixture/decoder code is handwritten Omega. Existing host
utilities only compile, inspect, compare and collect receipts.

Initial RED: compilable stub,54 checks,30 failures,exit30 (`red2`). Earlier
compile-only failure was an unsupported backslash-r literal; the Omega fixture
now constructs CR explicitly as byte13. That compiler error is not RED evidence.

Initial implementation passed54/54. Expanded boundary tests exposed nested-prefix
handling and mood/fallback decisions after the strict parser's twelve-line cap:
63 checks,4 failures,exit4 (`boundary-red`). Replaced nested prefix handling with
one iterative scanner and restricted fallback/mood eligibility to the same
canonical-line cap. Unchanged63-check suite then passed. Full native suite
passes; exact counts and eleven rejected behavioral mutants in counts.json.
Original checks, memory/stdio goldens and oversized-stdio refusal remain intact.

The published source inventory was verified byte-for-byte immediately before
commit. Full gate built all native components together. Independent review of
this new decoder remains pending. scdecode is not yet attached to the application
reply/display provider; this is neither full UI parity nor target/live proof.
