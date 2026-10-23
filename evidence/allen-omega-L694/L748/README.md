# L748 executable provider entry

The parent personally authored provider-core.om and provider.om. The final native
suite passed; summary.json counts actual assertion rows and names artifact hashes.
The 100-check provider core covers restoration, application calls, pending wire
preservation, explicit send results, answer/code delivery and refusal sequencing.
Two independent 292-check output-verifier executions exercised file and real pipe
streams. Native process-receipt checks validate six malformed/truncated/oversized
cases, including no later command after the malformed frame. These inputs are
synthetic qualification fixtures, not live model outputs.

Reproduce: `quiet-build native/omega/allen/gate.sh /absolute/fresh/private/output`.
The built `provider` is the actual static executable, not the older stdio adapter.
All new behavioral fixtures, refusal assertions and mutant construction execute
seeded Omega. The existing shell runner orchestrates builds/processes, collects
exit/byte metadata and uses external byte comparison. Historical gate shell and
legacy golden-oracle dependencies remain disclosed, not retired by this work.

Meaningful REDs: initial compilable core stub failed 74 of 97 checks; empty native
entry failed the output verifier; native refusal receipt validator rejected that
entry's incorrect exit0. Original logs/source are retained. Initial verifier
compilation exceeded the seed register budget and was split through vbyte without
changing expected values. Initial broad shell mutation also changed a function
signature and failed compilation (error16); it was not counted as a caught mutant.
The final exact-line mutation generator is Omega and requires exactly one match.
The first passing development runner still contained new shell test decisions;
those were moved to provider-process.om before this publication. New test exits
are fixed 1 on any assertion failure, not failure counts modulo256.

The final gate is release.log with exit0, not the earlier intermediate logs.
L748 independent final-source review is still required. No service, physical disk,
x86 EFI, QEMU Omega boot, netboot staging or physical iMac execution is claimed.
