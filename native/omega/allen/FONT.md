# L758 native stroke-font implementation

Continuation of L694 checkpoint5, executed inline with RED/GREEN. Language:
handwritten seeded Omega. Reuse the exact existing Allen glyph design and sine
lookup data from native/kernel/ui/allen_font.c at4c7888f; do not transpile C.
The legacy source is an external pixel oracle only, never construction/runtime.

Create font-data.om (95 glyph command strings,91 sine samples), font.om (integer
sqrt/trig, fixed-pool segment generation and single-glyph antialiased masks),
tests/font.om, tests/font-output.om and tests/font-render.om. No heap or font file.
API afinit(cap,weight) accepts1..64px and1..2500 weight units; afglyph(byte) maps
nonprintable input to '?'. Af fields: scale256,halfwidth256,segment count,width,
height,advance256. Ab[0,1] is signed mask origin relative to baseline. Alpha is
fixed65536 bytes; Ag contains at most160 line segments. Invalid init preserves
last state; successful init clears validity. A glyph render replaces the single
mask; no caching/per-face heap allocation claim.

- Write compilable stub and native tests; observe meaningful behavior RED.
- Handwrite geometry/parser/rasterization with bounded cap/weight and arithmetic.
- Run native tests including all95 glyphs, space, fallback, symmetry, bounds,
  signed angles, full-width sqrt cases and fixed nonzero assertion failure exits.
- Compile the unmodified legacy font as a separately labeled host oracle; externally
  collect mask metadata/bytes, compare with an Omega-emitted glyph stream. Neither
  oracle C nor collection Python is linked into or used to produce Omega code.
- Produce an actual native-rendered specimen, inspect it visually, record hashes.
- Run the native combined gate; publish source, exact receipts and known limits.

The current provider/presentation review continues separately; do not duplicate
it or alter its frozen source. Full scene layout, sky, motion, framebuffer device
and physical iMac qualification remain subsequent work, not implied by a font.
