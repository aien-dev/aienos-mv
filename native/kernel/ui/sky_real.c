/* sky_real.c -- the real sky: projection, stars, Milky Way, galaxies, and
 * the sidereal time for a later "sky above you now"; see sky_real.h.
 *
 * Light is added the way light adds: each star's light is put on in linear
 * light (the sRGB curve undone, the light added, the curve put back), so a
 * faint star on the Milky Way brightens it by as much as it would in the
 * sky. A star is drawn as a small round core with two wider, much fainter
 * rings round it (the glare any lens or eye gives a bright point); 80% of
 * its light in the core, 15% in a ring 4 times as wide, 5% in one 12 times
 * as wide. Its light is 10^(-0.4 m) of a magnitude 0 star's, so the
 * brightness of every star against every other is the sky's own. Where
 * the light is more than the screen can show (the brightest stars' cores),
 * it is white: the screen's limit, as in a photograph. */
#include "sky_real.h"

/* ---- projection ---- */

int ck_sky_project(const ck_sky_view *v, const int16_t d[3], int32_t w, int32_t cx, int32_t cy, int32_t *x16,
                   int32_t *y16)
{
    int64_t a[3];
    for (int r = 0; r < 3; r++)
        a[r] = (int64_t)v->rot_q30[r][0] * d[0] + (int64_t)v->rot_q30[r][1] * d[1] + (int64_t)v->rot_q30[r][2] * d[2];
    if (a[2] < (1ll << 40)) /* Q45: closer than 1/32 to edge-on, about 88 degrees from the centre */
        return 0;
    int64_t z = a[2] >> 16;                       /* Q29 */
    int64_t u = a[0] * 16384 / z, t = a[1] * 16384 / z; /* tangent-plane x and y, Q30 */
    int64_t s = (int64_t)w * 8;                   /* 16ths of a pixel per half width */
    *x16 = cx * 16 + (int32_t)(u * s / v->tan_half_q30);
    *y16 = cy * 16 - (int32_t)(t * s / v->tan_half_q30);
    return 1;
}

/* ---- the Milky Way grid ---- */

/* where the screen point (x2, y2) (half pixels) falls in the grid, Q16 columns and rows */
static void grid_at(int32_t x2, int32_t y2, int32_t w, int32_t cx, int32_t cy, int64_t *col, int64_t *row)
{
    const ck_sky_map *m = &ck_sky_milkyway;
    int64_t pw = m->per_width;
    *col = (int64_t)(x2 - 2 * cx) * pw * 32768 / w + pw * 32768;
    *row = (int64_t)(y2 - 2 * cy) * pw * 32768 / w - (int64_t)m->row0_q16 * pw;
}

static int32_t bilinear_u8(const uint8_t *p, int32_t cols, int32_t rows, int64_t c, int64_t r) /* x 256 */
{
    c = c < 0 ? 0 : c > (int64_t)(cols - 1) << 16 ? (int64_t)(cols - 1) << 16 : c;
    r = r < 0 ? 0 : r > (int64_t)(rows - 1) << 16 ? (int64_t)(rows - 1) << 16 : r;
    int32_t i = (int32_t)(c >> 16), j = (int32_t)(r >> 16), i1 = ck_min(i + 1, cols - 1), j1 = ck_min(j + 1, rows - 1);
    int32_t fx = (int32_t)(c >> 8 & 255), fy = (int32_t)(r >> 8 & 255);
    int32_t a = p[j * cols + i] * (256 - fx) + p[j * cols + i1] * fx;
    int32_t b = p[j1 * cols + i] * (256 - fx) + p[j1 * cols + i1] * fx;
    return (a * (256 - fy) + b * fy) >> 8;
}

static int32_t bilinear_s8(const int8_t (*p)[2], int k, int32_t cols, int32_t rows, int64_t c, int64_t r) /* x 256 */
{
    c = c < 0 ? 0 : c > (int64_t)(cols - 1) << 16 ? (int64_t)(cols - 1) << 16 : c;
    r = r < 0 ? 0 : r > (int64_t)(rows - 1) << 16 ? (int64_t)(rows - 1) << 16 : r;
    int32_t i = (int32_t)(c >> 16), j = (int32_t)(r >> 16), i1 = ck_min(i + 1, cols - 1), j1 = ck_min(j + 1, rows - 1);
    int32_t fx = (int32_t)(c >> 8 & 255), fy = (int32_t)(r >> 8 & 255);
    int32_t a = p[j * cols + i][k] * (256 - fx) + p[j * cols + i1][k] * fx;
    int32_t b = p[j1 * cols + i][k] * (256 - fx) + p[j1 * cols + i1][k] * fx;
    return (a * (256 - fy) + b * fy) >> 8;
}

/* the photograph's light at (x2, y2) half pixels: three channels in 16ths of a level */
static void milkyway_at(int32_t x2, int32_t y2, int32_t w, int32_t cx, int32_t cy, int32_t rgb[3])
{
    const ck_sky_map *m = &ck_sky_milkyway;
    int64_t col, row;
    grid_at(x2, y2, w, cx, cy, &col, &row);
    int32_t hc = (m->cols + 1) / 2, hr = (m->rows + 1) / 2;
    int32_t y = bilinear_u8(ck_sky_luma, m->cols, m->rows, col, row) >> 4;              /* 16ths */
    int64_t cc = (col - 32768) / 2, cr = (row - 32768) / 2;                             /* colour: half the spacing */
    int32_t co = bilinear_s8(ck_sky_chroma, 0, hc, hr, cc, cr) >> 5;                   /* half levels x 256 -> 16ths */
    int32_t cg = bilinear_s8(ck_sky_chroma, 1, hc, hr, cc, cr) >> 5;
    rgb[0] = ck_clamp(y + co - cg, 0, 4080);
    rgb[1] = ck_clamp(y + cg, 0, 4080);
    rgb[2] = ck_clamp(y - co - cg, 0, 4080);
}

void ck_sky_milkyway_q(uint16_t *q, int32_t qw, int32_t qh, int32_t w, int32_t cx, int32_t cy)
{
    for (int32_t j = 0; j < qh; j++)
        for (int32_t i = 0; i < qw; i++) {
            int32_t rgb[3];
            milkyway_at(8 * i + 1, 8 * j + 1, w, cx, cy, rgb);
            uint16_t *c = q + ((size_t)j * (size_t)qw + (size_t)i) * 3;
            c[0] = (uint16_t)rgb[0], c[1] = (uint16_t)rgb[1], c[2] = (uint16_t)rgb[2];
        }
}

/* ---- stars ---- */

/* adds light round (x16, y16): radius r16, centre light peak (linear, Q16), colour col (linear, 4096 the
 * brightest channel); the light falls off as (1 - d^2 / r^2)^3 */
static void light_add(ck_canvas *cv, int32_t x16, int32_t y16, int32_t r16, uint64_t peak, const uint16_t col[3])
{
    if (r16 < 4 || peak < 4)
        return;
    int32_t x0 = ck_max(0, (x16 - r16) >> 4), x1 = ck_min(cv->w - 1, (x16 + r16) >> 4);
    int32_t y0 = ck_max(0, (y16 - r16) >> 4), y1 = ck_min(cv->h - 1, (y16 + r16) >> 4);
    int64_t r2 = (int64_t)r16 * r16;
    uint64_t inv = (1ull << 40) / (uint64_t)r2;
    uint64_t pc[3];
    for (int k = 0; k < 3; k++)
        pc[k] = peak * col[k] >> 13; /* Q15 linear at the centre, per channel */
    for (int32_t y = y0; y <= y1; y++) {
        int64_t dy = (int64_t)y * 16 + 8 - y16;
        uint32_t *row = cv->px + (size_t)y * (size_t)cv->w;
        for (int32_t x = x0; x <= x1; x++) {
            int64_t dx = (int64_t)x * 16 + 8 - x16, d2 = dx * dx + dy * dy;
            if (d2 >= r2)
                continue;
            uint64_t f = 65536u - ((uint64_t)d2 * inv >> 24);   /* Q16 */
            uint64_t f3 = ((f * f >> 16) * f) >> 16;              /* Q16 */
            uint32_t c = row[x], out = 0;
            int any = 0;
            for (int k = 0; k < 3; k++) {
                uint32_t lv = c >> (16 - 8 * k) & 255u;
                uint64_t add = pc[k] * f3 >> 16;
                if (add) {
                    uint64_t l = ck_sky_lin[lv] + add;
                    lv = (ck_sky_srgb16[l >= 32767 ? 4095 : l >> 3] + 8) >> 4;
                    lv = lv > 255 ? 255 : lv;
                    any = 1;
                }
                out = out << 8 | lv;
            }
            if (any)
                row[x] = (c & 0xff000000u) | out;
        }
    }
}

static uint32_t srgb_of(const uint16_t col[3])
{
    uint32_t c = 0;
    for (int k = 0; k < 3; k++) {
        uint32_t i = col[k] >= 4096 ? 4095 : col[k];
        c = c << 8 | ((ck_sky_srgb16[i] + 8) >> 4);
    }
    return c;
}

void ck_sky_draw_stars(ck_canvas *cv, uint32_t k, int32_t cx, int32_t cy, ck_night_star *tw, unsigned max,
                       unsigned *ntw)
{
    int32_t r0 = (int32_t)(k * 3 / 40); /* 16ths of a pixel: 1.2 design px */
    if (r0 < 12)
        r0 = 12;
    /* the core grows with the scale and its middle keeps its light, so a star's total light grows as the
     * square of the scale and it looks the same on any screen */
    *ntw = 0;
    for (unsigned i = 0; i < ck_sky_nstars; i++) {
        const ck_sky_star *s = &ck_sky_stars[i];
        int16_t d[3] = {s->x, s->y, s->z};
        int32_t x16, y16;
        if (!ck_sky_project(&ck_sky_the_view, d, cv->w, cx, cy, &x16, &y16))
            continue;
        int32_t reach = r0 * 12;
        if (x16 < -reach || y16 < -reach || x16 > cv->w * 16 + reach || y16 > cv->h * 16 + reach)
            continue;
        const uint16_t *col = ck_sky_palette[s->bv < 128 ? s->bv : 127];
        uint64_t core = ck_sky_flux[s->mag]; /* the light at the middle of the core */
        light_add(cv, x16, y16, r0, core, col);
        light_add(cv, x16, y16, r0 * 4, core * 3 / 256, col);   /* 15% of the light, 16 times the area */
        light_add(cv, x16, y16, r0 * 12, core / 2304, col);     /* 5% of the light, 144 times the area */
        if (s->mag <= 80 && *ntw < max) { /* V 2.0 and brighter */
            ck_night_star *t = &tw[(*ntw)++];
            t->x = x16, t->y = y16, t->r = r0 * 4;
            t->color = srgb_of(col);
            uint32_t h = ck_hash((int32_t)i, 5, 91);
            t->period = (uint16_t)(2600u + h % 4200u);
            t->phase = (uint16_t)(ck_hash((int32_t)i, 6, 91) % t->period);
        }
    }
}

/* ---- galaxies ---- */

ck_rect ck_sky_galaxy_bound(uint32_t w, unsigned i)
{
    if (i >= ck_sky_ngalaxies)
        return (ck_rect){0, 0, 0, 0};
    int32_t a = (int32_t)(((int64_t)ck_sky_galaxies[i].a_q16 * w * 115 / 100) >> 16) + 2;
    return (ck_rect){0, 0, 2 * a + 1, 2 * a + 1};
}

static int galaxy_centre(uint32_t w, int32_t cx, int32_t cy, unsigned i, int32_t *gx, int32_t *gy)
{
    return i < ck_sky_ngalaxies && ck_sky_project(&ck_sky_the_view, ck_sky_galaxies[i].dir, (int32_t)w, cx, cy, gx, gy);
}

ck_rect ck_sky_galaxy_box(uint32_t w, uint32_t h, int32_t cx, int32_t cy, unsigned i)
{
    int32_t gx, gy;
    if (!galaxy_centre(w, cx, cy, i, &gx, &gy))
        return (ck_rect){0, 0, 0, 0};
    const ck_sky_galaxy *g = &ck_sky_galaxies[i];
    int64_t a = (int64_t)g->a_q16 * w * 115 / 100, b = (int64_t)g->b_q16 * w * 115 / 100; /* Q16 px */
    int64_t c = ck_cos(g->angle), s = ck_sin(g->angle);
    int32_t hx = (int32_t)(ck_isqrt((uint64_t)((a * c >> 14) * (a * c >> 14) + (b * s >> 14) * (b * s >> 14))) >> 16) + 2;
    int32_t hy = (int32_t)(ck_isqrt((uint64_t)((a * s >> 14) * (a * s >> 14) + (b * c >> 14) * (b * c >> 14))) >> 16) + 2;
    ck_rect r = {(gx >> 4) - hx, (gy >> 4) - hy, 2 * hx + 1, 2 * hy + 1}, all = {0, 0, (int32_t)w, (int32_t)h};
    ck_rect bound = ck_sky_galaxy_bound(w, i);
    r.w = ck_min(r.w, bound.w), r.h = ck_min(r.h, bound.h);
    if (!ck_clip(&r, &all))
        return (ck_rect){0, 0, 0, 0};
    return r;
}

void ck_sky_galaxy_maps(ck_night_galaxy *g, unsigned i, uint32_t w, int32_t cx, int32_t cy)
{
    int32_t gx, gy;
    if (!g->box.w || !galaxy_centre(w, cx, cy, i, &gx, &gy))
        return;
    const ck_sky_galaxy *d = &ck_sky_galaxies[i];
    int64_t a = ((int64_t)d->a_q16 * w) >> 12, b = ((int64_t)d->b_q16 * w) >> 12; /* 16ths of a pixel */
    if (a < 16 || b < 16)
        return;
    int64_t c = ck_cos(d->angle), s = ck_sin(d->angle);
    /* the sky round it: its light on an ellipse 1.3 times as wide, averaged */
    int64_t bg = 0;
    for (uint32_t t = 0; t < 32; t++) {
        int64_t ca = ck_cos(t * 2048u), sa = ck_sin(t * 2048u);
        int64_t u = a * 13 / 10 * ca >> 14, v = b * 13 / 10 * sa >> 14;
        int32_t x = (int32_t)(gx + ((u * c - v * s) >> 14)), y = (int32_t)(gy + ((u * s + v * c) >> 14));
        int32_t rgb[3];
        milkyway_at(x >> 3, y >> 3, (int32_t)w, cx, cy, rgb);
        bg += (rgb[0] + 2 * rgb[1] + rgb[2]) / 4;
    }
    bg /= 32;
    g->tint = d->tint;
    g->period = (uint16_t)(3000u + 400u * i);
    for (int32_t y = g->box.y; y < g->box.y + g->box.h; y++)
        for (int32_t x = g->box.x; x < g->box.x + g->box.w; x++) {
            size_t at = (size_t)(y - g->box.y) * (size_t)g->box.w + (size_t)(x - g->box.x);
            int64_t dx = (int64_t)x * 16 + 8 - gx, dy = (int64_t)y * 16 + 8 - gy;
            int64_t along = (dx * c + dy * s) >> 14, across = (dy * c - dx * s) >> 14;
            int64_t qa = along * 256 / a, qb = across * 256 / b;
            int64_t rho = (int64_t)ck_isqrt((uint64_t)(qa * qa + qb * qb)); /* 256: on the catalogue's outline */
            g->light[at] = 0;
            g->phase[at] = (uint8_t)ck_clamp((int32_t)(128 + qa / 2), 0, 255);
            if (rho >= 294)
                continue;
            int32_t rgb[3];
            milkyway_at(2 * x + 1, 2 * y + 1, (int32_t)w, cx, cy, rgb);
            int64_t l = ((rgb[0] + 2 * rgb[1] + rgb[2]) / 4 - bg) * 4 / 16; /* levels above the sky round it, x 4 */
            if (l <= 0)
                continue;
            uint32_t edge = 256 - ck_smooth((int32_t)(rho - 230) * 256 / 64);
            l = l * edge >> 8;
            g->light[at] = (uint8_t)(l > 255 ? 255 : l);
        }
}

/* ---- the sky above you now: sidereal time, the zenith, precession ---- */

#define J2000_UNIX 946728000ll /* 2000 January 1, 12:00 UT */

uint32_t ck_sky_gmst(int64_t t)
{
    /* GMST = 18.697374558 h + 24.06570982441908 h x D, D days from J2000 (USNO; Aoki et al. 1982),
     * here in turns x 2^48: 0.7790572732 + 1.0027379093508 D */
    int64_t s = t - J2000_UNIX, days = s / 86400, rem = s % 86400;
    if (rem < 0)
        rem += 86400, days -= 1;
    uint64_t turns = 219285127830237ull + (uint64_t)days * 770652970752ull + (uint64_t)rem * 3266731825ull;
    return (uint32_t)(turns >> 16);
}

/* (cos, sin) of a small angle a (radians, Q30), Q30 */
static void small_cs(int64_t a, int64_t *c, int64_t *s)
{
    int64_t a2 = a * a >> 30;
    *c = (1ll << 30) - a2 / 2 + (a2 * a2 >> 30) / 24;
    *s = a - (a2 * a >> 30) / 6;
}

static void rot_z(int64_t v[3], int64_t a) /* frame turned by a about z */
{
    int64_t c, s;
    small_cs(a, &c, &s);
    int64_t x = (v[0] * c + v[1] * s) >> 30, y = (v[1] * c - v[0] * s) >> 30;
    v[0] = x, v[1] = y;
}

static void rot_y(int64_t v[3], int64_t a) /* frame turned by a about y */
{
    int64_t c, s;
    small_cs(a, &c, &s);
    int64_t x = (v[0] * c - v[2] * s) >> 30, z = (v[0] * s + v[2] * c) >> 30;
    v[0] = x, v[2] = z;
}

/* sin and cos of a in millionths of a degree, Q30, from the Q14 table refined by one Newton-like step */
static void cs_udeg(int64_t a_udeg, int64_t *c, int64_t *s)
{
    a_udeg %= 360000000ll;
    if (a_udeg < 0)
        a_udeg += 360000000ll;
    uint32_t turn = (uint32_t)(a_udeg * 65536 / 360000000ll);
    /* the remainder past the table's step, radians Q30: (a - turn) x 2 pi / 360e6 */
    int64_t rest = a_udeg - (int64_t)turn * 360000000ll / 65536;
    int64_t e = rest * 18740 / 1000; /* 2 pi / 360e6 x 2^30 = 18.740 */
    int64_t c0 = (int64_t)ck_cos(turn) * 65536, s0 = (int64_t)ck_sin(turn) * 65536;
    *c = c0 - (s0 * e >> 30);
    *s = s0 + (c0 * e >> 30);
}

void ck_sky_zenith(int64_t t, int32_t lat_udeg, int32_t lon_udeg, int16_t d[3], int32_t *ra_udeg, int32_t *dec_udeg)
{
    /* straight up: right ascension the local sidereal time, declination the latitude (of the date) */
    int64_t lst = (int64_t)((uint64_t)ck_sky_gmst(t) * 360000000ull >> 32) + lon_udeg;
    int64_t ca, sa, cd, sd;
    cs_udeg(lst, &ca, &sa);
    cs_udeg(lat_udeg, &cd, &sd);
    int64_t v[3] = {cd * ca >> 30, cd * sa >> 30, sd};
    /* back from the equator of the date to J2000 (Lieske et al. 1977, IAU 1976):
     * v_J2000 = R3(zeta) R2(-theta) R3(z) v_date; the angles in radians Q40 per century */
    int64_t T = (t - J2000_UNIX) * (1ll << 30) / (36525ll * 86400); /* centuries, Q30 */
    int64_t T2 = T * T >> 30;
    int64_t zeta = (12293486530ll * T + 1609196ll * T2) >> 40; /* 2306.2181" T + 0.30188" T^2, Q30 rad */
    int64_t zz = (12293486530ll * T + 5835282ll * T2) >> 40;   /* 2306.2181" T + 1.09468" T^2 */
    int64_t th = (10684145203ll * T - 2274293ll * T2) >> 40;   /* 2004.3109" T - 0.42665" T^2 */
    rot_z(v, zz);
    rot_y(v, -th);
    rot_z(v, zeta);
    for (int k = 0; k < 3; k++)
        d[k] = (int16_t)ck_clamp((int32_t)(v[k] >> 15), -32767, 32767);
    if (ra_udeg) {
        uint32_t ra = ck_atan2((int32_t)(v[1] >> 8), (int32_t)(v[0] >> 8));
        *ra_udeg = (int32_t)((int64_t)ra * 360000000ll / 65536);
    }
    if (dec_udeg) {
        int64_t h = (int64_t)ck_isqrt((uint64_t)((v[0] >> 8) * (v[0] >> 8) + (v[1] >> 8) * (v[1] >> 8)));
        uint32_t de = ck_atan2((int32_t)(v[2] >> 8), (int32_t)h);
        *dec_udeg = (int32_t)((int64_t)(int16_t)de * 360000000ll / 65536);
    }
}

void ck_sky_view_at(ck_sky_view *v, int32_t ra_udeg, int32_t dec_udeg, int32_t fov_mdeg, int32_t roll_mdeg)
{
    int64_t ca, sa, cd, sd, cr, sr, ch, sh;
    cs_udeg(ra_udeg, &ca, &sa);
    cs_udeg(dec_udeg, &cd, &sd);
    cs_udeg((int64_t)roll_mdeg * 1000, &cr, &sr);
    cs_udeg((int64_t)fov_mdeg * 500, &ch, &sh);
    int64_t west[3] = {sa, -ca, 0}, north[3] = {-(sd * ca >> 30), -(sd * sa >> 30), cd};
    int64_t fwd[3] = {cd * ca >> 30, cd * sa >> 30, sd};
    for (int k = 0; k < 3; k++) {
        v->rot_q30[0][k] = (int32_t)((cr * west[k] + sr * north[k]) >> 30);
        v->rot_q30[1][k] = (int32_t)((cr * north[k] - sr * west[k]) >> 30);
        v->rot_q30[2][k] = (int32_t)fwd[k];
    }
    v->tan_half_q30 = (int32_t)((sh << 30) / ch);
    v->ra_mdeg = ra_udeg / 1000, v->dec_mdeg = dec_udeg / 1000, v->fov_mdeg = fov_mdeg, v->roll_mdeg = roll_mdeg;
}
