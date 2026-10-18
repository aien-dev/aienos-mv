/* allen_ui.h -- Allen's window on the display: an aurora background, a glass
 * window with Allen's living orb (it breathes while waiting and glows while
 * answering), the conversation as bubbles, the line being typed in an input
 * field, and beside the window the map of Allen's notes. Drawn with integer
 * math into RAM and copied to the screen, only where something changed. Pure:
 * no MMIO of its own (the caller hands in the framebuffer), host tested in
 * ui/tests/allen_ui_test.c. Allen's text uses its own stroke typeface
 * (allen_font.h). */
#ifndef AIENOS_ALLEN_UI_H
#define AIENOS_ALLEN_UI_H
#include <stddef.h>
#include <stdint.h>

/* RAM the window needs for a w x h screen (two full-screen layers and the
 * glyph masks). */
size_t ck_allen_ui_mem_bytes(uint32_t w, uint32_t h);
/* Takes over the screen: fb = the framebuffer (pitch in pixels; format 0
 * RGBX, 1 BGRX), mem = ck_allen_ui_mem_bytes(w, h) bytes, hz = ticks per
 * second of the clock `now` counts in, footer = one short line under the
 * input field. Draws everything. 0, or -1 (bad size or format, short mem). */
int ck_allen_ui_start(volatile uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, void *mem,
                      size_t mem_len, uint64_t hz, uint64_t now, const char *footer);
int ck_allen_ui_on(void);
/* The line being typed changed (buf holds len characters). */
void ck_allen_ui_input(const char *buf, unsigned len, uint64_t now);
/* A line was entered and Allen answered: both become bubbles; the answer
 * appears letter by letter while the orb glows. line NULL: Allen speaks
 * first (the welcome after a restart), one bubble. */
void ck_allen_ui_said(const char *line, const char *reply, uint64_t now);
/* Animation: call often (every poll); it redraws only what is due. */
void ck_allen_ui_tick(uint64_t now);

/* The notes map, a glass panel beside the window (drawn when the screen has
 * room for it): n notes as points, 0 the newest at the centre, the older ones
 * spiralling out (at most CK_ALLEN_UI_MAP_NODES); ne links, point ea[i] to
 * point eb[i] (notes that share a key word; at most CK_ALLEN_UI_MAP_LINKS);
 * lit the point of the note just saved or handed back (-1: none) and label
 * its text; total and links count every live note and link (the map may show
 * only the newest). Kept, and drawn at once when the window is on. */
#define CK_ALLEN_UI_MAP_NODES 48u
#define CK_ALLEN_UI_MAP_LINKS 160u
void ck_allen_ui_map(unsigned n, const uint8_t *ea, const uint8_t *eb, unsigned ne, int lit, const char *label,
                     unsigned total, unsigned links);

#endif
