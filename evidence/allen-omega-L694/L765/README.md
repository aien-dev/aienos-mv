# L765 clipped native text surface

The native surface renders retained text using the qualified Omega stroke font.
The actual image follows provider BOOT/ASK/RECV -> retained note -> RGB pixels.
It is a test reply, not live inference or a completed hosted application. The
inspected PNG is a lossless conversion of the actual native P6 output; final
full-gate pixels match the inspected specimen. Font specimen pixels and provider
wire bytes are unchanged after sharing surface storage with the old fixture.

Reproduce: `quiet-build native/omega/allen/gate.sh /absolute/fresh/output`.
Counts and actual compiled-mutant failures are in summary.json. Native tests
exercise blending, clipping, negative origins, fractional pen advance, maximum
span, refusal preservation and input/wire isolation. New mutation logic is Omega.

Initial stub compilation failed because parameter start collided with runtime's
start function (error10 atline4162). Retained red.om is that construction failure,
not behavioralRED. Renaming only that parameter to beg yielded79 assertions,
43 failures,exit1. Expanded stub85 assertions/46 failures and final native85/85
are retained. No assertion was weakened; no compiler implementation was changed.

The public-alpha direction is native/omega/allen/ALPHA.md: sovereign local compute,
optional user cloud or keyed Spark, free temporary hosted trial without boot,
partition or driver modification. The current fixture neither installs an app
nor touches devices. Target/hosted packaging and live inference remain unqualified.
Independent surface review remains pending.
