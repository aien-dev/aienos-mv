/* chirp.h -- the startup sound: the chirp two bodies make as they spiral into
 * each other and merge (a gravitational wave), raised into hearing and timed
 * to the startup picture (ui/whirlpool.h). Silent while the picture holds
 * still; a low steady tone while it moves out into its orbit; rising in
 * pitch and loudness as it spirals in, the way the real wave does (pitch as
 * t^(-3/8), loudness as pitch^(2/3), t the time left), with its peak at the
 * plunge (CK_WHIRL_MERGE_MS); then a short ringing fade. Made from that
 * formula, no recorded sound. Integer only (the kernel builds with
 * -mgeneral-regs-only), freestanding, no memory of its own beyond a sine
 * table. Host tested in ui/tests/chirp_test.c. */
#ifndef AIENOS_CHIRP_H
#define AIENOS_CHIRP_H
#include <stdint.h>
#include "whirlpool.h"

#define CK_CHIRP_RATE 48000u   /* frames a second; each frame is a left and a right sample, 16 bits */
#define CK_CHIRP_FROM_HZ 150u  /* the steady tone of the orbit */
#define CK_CHIRP_TO_HZ 1000u   /* the pitch at the plunge, held through the fade */
#define CK_CHIRP_PEAK 22937    /* 70% of full scale */
#define CK_CHIRP_FRAMES (CK_CHIRP_RATE / 1000u * CK_WHIRL_MS)

/* Writes frames [from, from + n) as left, right pairs into out (2n samples),
 * frame 0 being the whirlpool's 0 ms. Silent past CK_CHIRP_FRAMES. Any frame
 * can be asked for in any order: each is worked out from the formula. */
void ck_chirp_fill(int16_t *out, uint32_t from, uint32_t n);

/* The pitch in Hz and the loudness (0..CK_CHIRP_PEAK) at a frame. */
uint32_t ck_chirp_hz(uint32_t frame);
uint32_t ck_chirp_level(uint32_t frame);
#endif
