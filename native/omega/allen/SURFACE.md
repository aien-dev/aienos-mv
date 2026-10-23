# Bounded native text surface (L765)

A necessary rendering component for the iMac-first visual alpha, not a full
window system, animation, layout engine or display driver. Handwritten Omega;
qualified existing font geometry and retained presentation are reused.

`Canvas` is a fixed884736-byte RGB buffer. `Cv` fields are width,height,
clip-x,clip-y,clip-width,clip-height,initialized,last-advance256.

- `cvinit(w,h)`: accepts1..768 by1..384, clears the full buffer, fills active
  pixels with night RGB(6,8,18), and establishes a full-surface clip.
- `cvclip(x,y,w,h)`: unsigned clip must fit wholly within active dimensions.
  Zero-area clips at the edge are valid. Validate using subtraction before
  adding coordinates, so an oversized width cannot wrap into acceptance.
- `cvglyph(byte,x,y,color)`: requires initialized surface and font, signed
  baseline coordinates within[-4096,4096], packed24-bit RGB. Renders through
  the existing font and clips every pixel. Saves glyph advance in Cv[7].
- `cvspan(beg,size,x,y,color)`: same guards; reads an explicit bounded Dtext
  span. Length0 at2304 is legal. Glyph advance accumulates in256ths of a pixel,
  with signed floor at each baseline; no per-glyph rounding drift. All bytes
  use the existing font fallback. No implicit wrapping/newline interpretation.

Invalid public arguments preserve Canvas/Cv; font, mask and pose are scratch
for admitted drawing. Global arrays are module-owned, not an untrusted caller
mutation interface. Valid font settings and immutable glyph data are required;
this component does not promise rollback after arbitrary corruption of those
private globals. Maximum span is2304 bytes and positions remain bounded by the
qualified maximum glyph advance. No heap, file font or foreign runtime.

Blend each channel using integer `(background*(255-alpha)+foreground*alpha)/255`,
truncated down, from the CURRENT destination pixel. In/Inlen, retained D* text,
and offered Outb/Outlen/Err are not renderer scratch. Native tests cover analytic
alpha0/128/255 and repeated overlap, an independent crop comparison, negative
origins, exact edge/zero clips, oversized/refused arguments and span/glyph parity.
Compiled mutants remove the right clip or replace the destination color.

The actual `surface-render` executable submits BOOT/ASK/RECV through prdo,
projects a fixture note into retained presentation, and draws its title/body.
Its P6 image is real Omega-produced pixels; reply content is an explicit test
fixture, NOT a claimed live-model response. PNG conversion is external lossless
format conversion only. Hosted application/boot/device qualification remains open.
