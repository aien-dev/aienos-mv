/* sky_real.h -- the real sky behind Allen: the catalogue's stars at their
 * true places, brightness and colour; the Milky Way from a photograph of
 * the whole sky; the galaxies the eye can see. The data are in
 * sky_data.c (generated, see sky_data.h); this is the drawing, integer only.
 *
 * The view: a gnomonic projection (what a pinhole camera sees) of a set
 * piece of sky, the view centre at (cx, cy) on the screen, a set width of
 * sky across the screen's width. By default it is centred where the first
 * black-hole merger ever heard, GW150914, most probably came from.
 *
 * Also here, not yet used by the startup: the sidereal-time maths for a
 * later "the sky above you now" (see docs/ALLEN_SKY_SCIENCE.md). */
#ifndef AIENOS_SKY_REAL_H
#define AIENOS_SKY_REAL_H
#include <stdint.h>

#include "night.h"
#include "sky_data.h"

/* Where the direction d (Q15) lands on a screen w px wide whose view
 * centre is at (cx, cy) px: (x16, y16) in 16ths of a pixel, the pixel i
 * spanning 16 i to 16 i + 16. 0 if it is more than 88 degrees from the
 * centre (behind the camera), else 1. */
int ck_sky_project(const ck_sky_view *v, const int16_t d[3], int32_t w, int32_t cx, int32_t cy, int32_t *x16,
                   int32_t *y16);

/* The Milky Way's light at a quarter of the resolution: q holds qw x qh
 * cells of three channels in 16ths of a level, cell (i, j) for the pixel
 * (4 i, 4 j) of a w-wide screen with its view centre at (cx, cy). */
void ck_sky_milkyway_q(uint16_t *q, int32_t qw, int32_t qh, int32_t w, int32_t cx, int32_t cy);

/* Draws every catalogue star in the view into cv (a w x h sky whose view
 * centre is at (cx, cy); k: 1/256 px per design px of a 1920-wide design),
 * adding its light to what is there. The brightest few (V 2.0 and
 * brighter) are also listed in tw, up to max, to twinkle; *ntw: how many. */
void ck_sky_draw_stars(ck_canvas *cv, uint32_t k, int32_t cx, int32_t cy, ck_night_star *tw, unsigned max,
                       unsigned *ntw);

/* The largest box galaxy i can need on a w-wide screen (for memory). */
ck_rect ck_sky_galaxy_bound(uint32_t w, unsigned i);
/* Galaxy i's box on a w x h screen, view centre (cx, cy) (clipped; empty if
 * it is not in the view or there is no galaxy i). */
ck_rect ck_sky_galaxy_box(uint32_t w, uint32_t h, int32_t cx, int32_t cy, unsigned i);
/* Fills g->light (its own light, above the sky round it, from the Milky Way
 * photograph) and g->phase (where along its long axis, 0..255) over
 * g->box. */
void ck_sky_galaxy_maps(ck_night_galaxy *g, unsigned i, uint32_t w, int32_t cx, int32_t cy);

/* ---- the sky above you now (not wired yet) ---- */

/* Greenwich mean sidereal time at unix time t (seconds, UTC taken as UT1:
 * within a second), in turns / 2^32. IAU 1982 (Aoki et al. 1982; USNO). */
uint32_t ck_sky_gmst(int64_t unix_s);
/* The direction straight up (the zenith) at unix time t from latitude lat
 * and east longitude lon (millionths of a degree), in J2000 axes (the
 * local sidereal time, then the 1976 IAU precession back to J2000), Q15.
 * As RA and Dec in millionths of a degree too, if ra or dec are given. */
void ck_sky_zenith(int64_t unix_s, int32_t lat_udeg, int32_t lon_udeg, int16_t d[3], int32_t *ra_udeg,
                   int32_t *dec_udeg);
/* A view centred on (ra, dec) (millionths of a degree), fov degrees wide
 * (thousandths), north up turned by roll (thousandths), built with integer
 * trigonometry. */
void ck_sky_view_at(ck_sky_view *v, int32_t ra_udeg, int32_t dec_udeg, int32_t fov_mdeg, int32_t roll_mdeg);

/* the location setting: unset unless the build sets both (millionths of a degree) */
#if defined(CK_SKY_LAT_UDEG) && defined(CK_SKY_LON_UDEG)
#define CK_SKY_HAVE_LOCATION 1
#else
#define CK_SKY_HAVE_LOCATION 0
#endif
#endif
