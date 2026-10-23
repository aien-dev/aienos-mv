# L758 handwritten native Allen font

font.om implements integer geometry, segment generation and antialiased masks in
seeded Omega. font-data.om retains the existing95 printable ASCII designs and91
sine samples; the collector checked exact declarative transcription. No compiler,
translator or foreign runtime constructs or executes the Omega font.

The native combined gate passed. summary.json counts actual test rows and compiled
behavioral mutant failures.294 native glyph records, including3 size/weight pairs
and unsupported-byte fallback cases, match436341 frozen external-oracle bytes.
The oracle was the unmodified legacy allen_font.c compiled into a separate host
shared library only for reference collection. Its source hash, command and output
hash are in oracle.json; collection script is retained as external reference text.
The native gate consumes those frozen bytes, not C/Python font execution.

Reproduce: `quiet-build native/omega/allen/gate.sh /absolute/fresh/private/output`.
This builds font-output and font-render with the pinned seed. The final native
specimen was visually inspected: all printable characters recognizable and no
obvious clipping. PNG is lossless format conversion of the actual Omega PPM;
there is no generated imagery, font substitution or invented star field.

Preserved development failures: weight parameter collided with existing weight
function; register-budget expressions were split into helpers without loosening
assertions. The initial oracle stream stopped after its64-byte space record with
exit105: negative long->ulong conversion is checked, not a bitcast. Output encoding
now explicitly forms two-complement fields via complement of magnitude-minus-one.
The old stream/source are retained as native-font.bin and output.om; corrected
native output matches the frozen oracle exactly. RED4/RED5 are meaningful stub
failures, separate from compiler setup errors. New tests use fixed nonzero failure
status. This does not implement scene layout, animation, framebuffer drivers,
live model/device interfaces or physical iMac execution. Font review is pending.
