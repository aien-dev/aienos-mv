/* allen_ui.h -- Allen's window on the display: an aurora background, a glass
 * window with Allen's living orb (it breathes while waiting and glows while
 * answering), the conversation as bubbles, and the line being typed in an
 * input field. Drawn with integer math into RAM and copied to the screen,
 * only where something changed. Pure: no MMIO of its own (the caller hands
 * in the framebuffer), host tested in ui/tests/allen_ui_test.c. Allen's text
 * uses its own stroke typeface (allen_font.h). */
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
 * appears letter by letter while the orb glows. */
void ck_allen_ui_said(const char *line, const char *reply, uint64_t now);
/* Animation: call often (every poll); it redraws only what is due. */
void ck_allen_ui_tick(uint64_t now);

#endif
