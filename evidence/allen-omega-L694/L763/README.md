# L763 executable provider/presentation attachment

ASK/RECV/RESET now call dask/drecv/dreset. Presentation definitions precede the
provider in the seed concatenation. Input/output framing remains version1 and
byte-identical to L758's real normal and fragmented entry runs. New native prdo
assertions expose retained scene/code state, malformed and semantic refusal
isolation, pending-wire preservation and reset. Unchanged provider:24 failures
among136 assertions,exit1. Three-call attachment:136/136. All combined native
gates and22 compiled mutants pass; exact row counts are in summary.json. The
three new Omega-constructed mutants individually bypass ASK,RECV or RESET.

Reproduce with `quiet-build native/omega/allen/gate.sh /absolute/fresh/output`.
Host orchestration compiles, wires and compares; new behavioral assertions and
mutation construction are Omega. The renderer still must consume D* buffers:
this change supplies neither pixels nor actual device/network/storage backends.
Independent review of this attachment is pending. The separate font review is
published under L758/independent-review and does not certify this new dispatch.
