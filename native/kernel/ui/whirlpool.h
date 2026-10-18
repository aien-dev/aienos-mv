/* whirlpool.h -- the startup picture. When AIENOS takes the screen it keeps
 * what the firmware left in the middle of it (on most machines the maker's
 * logo, whatever machine this is), pulls it into an orbit round a black hole
 * at the centre of a turning whirlpool, and spirals it in until it plunges
 * and a ripple runs out; the water keeps turning until Allen's window takes
 * the screen. Nothing of any maker's artwork is in AIENOS: the picture is
 * read from the screen at that moment, and a blank screen gives the
 * whirlpool alone.
 *
 * Integer only (the kernel builds with -mgeneral-regs-only), freestanding, no
 * heap: the caller hands over ck_whirl_mem_bytes of memory. Pixels are the
 * screen's own (format 1: 0x00RRGGBB, format 0: 0x00BBGGRR). Drawn in square
 * blocks of `block` pixels (at most 1280 blocks across), straight to the
 * screen. Host tested in ui/tests/whirlpool_test.c. */
#ifndef AIENOS_WHIRLPOOL_H
#define AIENOS_WHIRLPOOL_H
#include <stddef.h>
#include <stdint.h>

#define CK_WHIRL_LOGO_FROM_MS 300u /* the picture stays still while the water appears */
#define CK_WHIRL_ORBIT_MS 1000u    /* by now it is out in its orbit */
#define CK_WHIRL_MERGE_MS 3000u    /* the plunge: the picture is gone */
#define CK_WHIRL_MS 3800u          /* the ripple has run out; only water from here on */

/* Memory ck_whirl_start needs for a w x h screen. */
size_t ck_whirl_mem_bytes(uint32_t w, uint32_t h);

/* Copies the middle of the screen (a square, half the screen's height) and
 * sets up the tables. *logo: pixels that differ from the screen's background
 * (0 for a blank screen); *block: the block size. 0, or -1 when the
 * geometry or the memory will not do (nothing read or drawn). */
int ck_whirl_start(volatile uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, void *mem,
                   size_t bytes, uint32_t *logo, uint32_t *block);

/* Draws the whole screen as it is t_ms after ck_whirl_start: at 0 the screen
 * as the firmware left it (in blocks), by CK_WHIRL_MS only water. Returns the
 * blocks where the picture still clearly shows: the first frame that returns
 * 0 after the picture started moving is the moment it is gone. */
uint32_t ck_whirl_frame(uint32_t t_ms);
#endif
