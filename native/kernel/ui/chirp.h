/* chirp.h -- the startup sound: the last moment of the chirp two black holes
 * make as they spiral into each other and merge (a gravitational wave), at its
 * real pitch, timed to the night sky of the startup (ui/night.h). The numbers
 * are GW150914's (Abbott et al., PRL 116, 061102): 35 Hz at the start, 250 Hz at
 * the merger, 0.165 s before it by the leading-order law for the published
 * masses (about 0.2 s and 8 cycles from 35 to 150 Hz in the signal itself).
 * Silent until CK_CHIRP_RISE_MS before the merger; then it comes in over
 * CK_CHIRP_IN_MS and sweeps up in pitch and loudness the way the real wave
 * does (pitch as t^(-3/8), loudness as pitch^(2/3), t the time left), with its
 * peak at the merger (CK_NIGHT_MERGE_MS); then the final hole rings at 250 Hz,
 * dying with a 4.0 ms time constant (its damping time). A quarter of a second
 * in all. Made from that formula, no recorded sound. Integer only (the kernel
 * builds with -mgeneral-regs-only), freestanding, no memory of its own beyond a
 * sine table. Host tested in ui/tests/chirp_test.c; measured against the real
 * signal in ui/tests/chirp_audit.c and docs/ALLEN_SKY_SCIENCE.md. */
#ifndef AIENOS_CHIRP_H
#define AIENOS_CHIRP_H
#include <stdint.h>
#include "night.h"

#define CK_CHIRP_RATE 48000u   /* frames a second; each frame is a left and a right sample, 16 bits */
#define CK_CHIRP_FROM_HZ 35u   /* the pitch the sweep starts at: GW150914 entered the detectors' band at 35 Hz */
#define CK_CHIRP_TO_HZ 250u    /* the pitch at the merger, held through the ring: the final hole's ringdown, 250 Hz */
#define CK_CHIRP_PEAK 22937    /* 70% of full scale */
#define CK_CHIRP_RISE_MS 165u  /* the sweep up to the merger: 35 Hz is 0.165 s before it (leading-order law, chirp mass 30.6) */
#define CK_CHIRP_IN_MS 40u     /* the sound comes in over this, just before the sweep */
#define CK_CHIRP_START_MS (CK_NIGHT_MERGE_MS - CK_CHIRP_RISE_MS)
#define CK_CHIRP_END_MS (CK_NIGHT_MERGE_MS + 60u) /* the ring has died away (15 time constants) */
#define CK_CHIRP_FRAMES (CK_CHIRP_RATE / 1000u * CK_CHIRP_END_MS)

/* Writes frames [from, from + n) as left, right pairs into out (2n samples),
 * frame 0 being the night's 0 ms. Silent past CK_CHIRP_FRAMES. Any frame
 * can be asked for in any order: each is worked out from the formula. */
void ck_chirp_fill(int16_t *out, uint32_t from, uint32_t n);

/* The pitch in Hz and the loudness (0..CK_CHIRP_PEAK) at a frame. */
uint32_t ck_chirp_hz(uint32_t frame);
uint32_t ck_chirp_level(uint32_t frame);

/* The chirp at 44.1 kHz instead: frames [from, from + n) of that rate, frame
 * 0 the night's 0 ms, each sample in the top 16 bits of a 32-bit slot (a
 * 24-bit converter's layout), left and right. */
#define CK_CHIRP_RATE_441 44100u
#define CK_CHIRP_FRAMES_441 (CK_CHIRP_RATE_441 / 100u * CK_CHIRP_END_MS / 10u)
void ck_chirp_fill_441(int32_t *out, uint32_t from, uint32_t n);
#endif
