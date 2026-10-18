/* whirlpool.c -- the startup picture; see whirlpool.h.
 *
 * Every block keeps its distance from the screen's centre (r, in 16ths of a
 * pixel) and its angle a (4096 to a turn), for the water: three spiral arms
 * turning anticlockwise, wound tighter towards the black hole in the middle,
 * whose rim glows. The picture is pulled off the middle into an orbit and
 * spirals in the way two bodies do before they merge: with t the time left,
 * its distance goes as t^(1/4) and its angle as t^(5/8), so it circles
 * faster and closer until it plunges in, spinning twice per orbit and
 * shrinking as it goes. At the plunge a ripple of light runs out through the
 * water. A block of the picture is found by taking the block's place
 * relative to the picture's centre, turning it back by the spin and scaling
 * it back up. */
#include "whirlpool.h"

#define RMAX 4096u  /* distances are capped here; a 4K screen's corner is 2203 */
#define ARMS 3u
#define WIND 10240u   /* 2.5 turns of winding from the rim to the black hole */
#define ORBITS 20480u /* 5 turns between reaching the orbit and the plunge */
#define TURN_MS 4000u /* the water's pattern goes round once in 4 s */
#define FADE_MS 700u  /* the water appears over this long */
#define GONE_MS 80u   /* the picture fades over its last 80 ms */

#define RGB(r, g, b) ((uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))

static struct {
    volatile uint32_t *fb;
    uint32_t w, h, pitch, format, block, gw, gh;
    uint32_t side;   /* the copied square */
    uint32_t bg;     /* the firmware's background, 0x00RRGGBB */
    uint32_t *snap;  /* side x side: 0xAARRGGBB, A the picture's coverage */
    uint32_t *grid;  /* gw x gh: r << 12 | a, r in 16ths of a pixel */
    uint32_t *row;   /* gw */
    int logo;
} wp;

static int16_t sin_t[4096];                /* Q14 */
static uint16_t atan_t[1025];              /* atan(i / 1024), 4096 to a turn */
static uint16_t q_water[RMAX];             /* the arms' winding weight, 65535 at the centre */
static uint32_t base_c[RMAX], arm_c[RMAX]; /* water colours by distance */

static uint32_t ch(uint32_t c, int s) { return c >> s & 255u; }

static uint32_t lerp(uint32_t a, uint32_t b, uint32_t t) /* t: 0..256 */
{
    uint32_t o = 0;
    for (int s = 0; s <= 16; s += 8)
        o |= ((ch(a, s) * (256 - t) + ch(b, s) * t) >> 8) << s;
    return o;
}

static uint32_t scale(uint32_t c, uint32_t t) /* t: 0..256 */
{
    return lerp(0, c, t);
}

static uint32_t swap_rb(uint32_t c) { return (c & 0xff00u) | (c >> 16 & 0xffu) | (c & 0xffu) << 16; }

static void tables(void)
{
    /* sine by rotating (cos, sin) one step at a time in Q30: the error stays
     * far below one Q14 step over a quarter turn */
    const int64_t C = 1073740561, S = 1647099; /* cos, sin of a 4096th of a turn, Q30 */
    int64_t c = 1ll << 30, s = 0;
    for (unsigned k = 0; k <= 1024; k++) {
        int16_t v = k == 1024 ? 16384 : (int16_t)((s + (1 << 15)) >> 16);
        sin_t[k] = v;
        sin_t[2048 - k] = v;
        sin_t[(2048 + k) & 4095] = (int16_t)-v;
        sin_t[(4096 - k) & 4095] = (int16_t)-v;
        int64_t nc = (c * C - s * S) >> 30, ns = (s * C + c * S) >> 30;
        c = nc, s = ns;
    }
    /* atan over the first eighth of a turn, from that table */
    unsigned k = 0;
    for (unsigned i = 0; i <= 1024; i++) {
        while (k < 512 && (int64_t)sin_t[k] * 1024 < (int64_t)i * sin_t[k + 1024])
            k++;
        atan_t[i] = (uint16_t)k;
    }
}

static uint32_t atan2_turn(int32_t dy, int32_t dx) /* 4096 to a turn, y down */
{
    uint32_t ax = (uint32_t)(dx < 0 ? -dx : dx), ay = (uint32_t)(dy < 0 ? -dy : dy), t;
    if (!ax && !ay)
        return 0;
    if (ay <= ax)
        t = atan_t[(uint64_t)ay * 1024 / ax];
    else
        t = 1024 - atan_t[(uint64_t)ax * 1024 / ay];
    if (dx < 0)
        t = 2048 - t;
    if (dy < 0)
        t = 4096 - t;
    return t & 4095;
}

static uint32_t isqrt(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

size_t ck_whirl_mem_bytes(uint32_t w, uint32_t h)
{
    uint32_t b = (w + 1279) / 1280, side = (w < h ? w : h) / 2;
    if (!b)
        return 0;
    size_t gw = (w + b - 1) / b, gh = (h + b - 1) / b;
    return ((size_t)side * side + gw * gh + gw) * 4;
}

/* The firmware's picture: its background is the square's corner; coverage is
 * how far a pixel is from it, against the strongest pixel there, and the
 * colour is taken back out of the blend with the background so the edges sit
 * cleanly on the water. */
static uint32_t capture(void)
{
    uint32_t side = wp.side, x0 = (wp.w - side) / 2, y0 = (wp.h - side) / 2, strongest = 0, n = 0;
    for (uint32_t y = 0; y < side; y++) {
        volatile const uint32_t *src = wp.fb + (size_t)(y0 + y) * wp.pitch + x0;
        uint32_t *dst = wp.snap + (size_t)y * side;
        for (uint32_t x = 0; x < side; x++) {
            uint32_t c = src[x] & 0xffffffu;
            dst[x] = wp.format ? c : swap_rb(c);
        }
    }
    wp.bg = wp.snap[0];
    for (size_t i = 0; i < (size_t)side * side; i++) {
        uint32_t c = wp.snap[i], d = 0;
        for (int s = 0; s <= 16; s += 8) {
            uint32_t a = ch(c, s), b = ch(wp.bg, s), v = a > b ? a - b : b - a;
            d = v > d ? v : d;
        }
        wp.snap[i] = c | d << 24;
        strongest = d > strongest ? d : strongest;
    }
    if (strongest < 24) { /* a blank screen, or near enough */
        for (size_t i = 0; i < (size_t)side * side; i++)
            wp.snap[i] &= 0xffffffu;
        return 0;
    }
    for (size_t i = 0; i < (size_t)side * side; i++) {
        uint32_t c = wp.snap[i], d = c >> 24, cov = d * 255 / strongest, o = 0;
        if (d < 8) { /* noise in the background */
            wp.snap[i] = c & 0xffffffu;
            continue;
        }
        n++;
        for (int s = 0; s <= 16; s += 8) {
            int32_t b = (int32_t)ch(wp.bg, s), v = b + ((int32_t)ch(c, s) - b) * 255 / (int32_t)cov;
            o |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << s;
        }
        wp.snap[i] = o | cov << 24;
    }
    return n;
}

static uint32_t add_sat(uint32_t a, uint32_t b)
{
    uint32_t o = 0;
    for (int s = 0; s <= 16; s += 8) {
        uint32_t v = ch(a, s) + ch(b, s);
        o |= (v > 255 ? 255 : v) << s;
    }
    return o;
}

static void water_tables(void)
{
    uint32_t R = (wp.w < wp.h ? wp.w : wp.h) / 2, r0w = R / 3;
    if (!r0w)
        r0w = 1;
    const uint32_t teal = RGB(64, 214, 200), blue = RGB(70, 110, 255), violet = RGB(110, 70, 230);
    for (uint32_t r = 0; r < RMAX; r++) {
        uint32_t q = (uint32_t)(65536ull * r0w / (r + r0w));
        q_water[r] = (uint16_t)(q > 65535 ? 65535 : q);
        uint32_t u = (uint32_t)((uint64_t)r * 256 / R), env; /* 256 at the edge of the short side */
        if (u < 16)
            env = 0; /* the black hole */
        else if (u < 64)
            env = (u - 16) * (u - 16) * 256 / (48 * 48);
        else if (u < 256)
            env = 256 - (u - 64) * 115 / 192;
        else if (u < 512)
            env = 141 - (u - 256) * 77 / 256;
        else
            env = 64;
        uint32_t arm = u < 40 ? teal : u < 120 ? lerp(teal, blue, (u - 40) * 256 / 80)
                     : u < 256 ? lerp(blue, violet, (u - 120) * 256 / 136) : violet;
        arm_c[r] = scale(arm, env * 200 / 256);
        uint32_t base = u < 16 ? 0 : u < 96 ? lerp(RGB(1, 2, 6), RGB(10, 14, 38), u * 256 / 96)
                      : u < 320 ? lerp(RGB(10, 14, 38), RGB(5, 7, 20), (u - 96) * 256 / 224) : RGB(5, 7, 20);
        uint32_t off = u > 18 ? u - 18 : 18 - u; /* the black hole's glowing rim */
        if (off < 5)
            base = add_sat(base, scale(teal, (5 - off) * 140 / 5));
        base_c[r] = base;
    }
}

int ck_whirl_start(volatile uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, void *mem,
                   size_t bytes, uint32_t *logo, uint32_t *block)
{
    if (!fb || w < 64 || h < 64 || w > 8192 || h > 8192 || pitch < w || format > 1 || !mem ||
        bytes < ck_whirl_mem_bytes(w, h))
        return -1;
    wp.fb = fb;
    wp.w = w, wp.h = h, wp.pitch = pitch, wp.format = format;
    wp.block = (w + 1279) / 1280;
    wp.gw = (w + wp.block - 1) / wp.block;
    wp.gh = (h + wp.block - 1) / wp.block;
    wp.side = (w < h ? w : h) / 2;
    wp.snap = mem;
    wp.grid = wp.snap + (size_t)wp.side * wp.side;
    wp.row = wp.grid + (size_t)wp.gw * wp.gh;
    tables();
    uint32_t n = capture();
    wp.logo = n > 0;
    water_tables();
    int32_t cx = (int32_t)w / 2, cy = (int32_t)h / 2, half = (int32_t)wp.block / 2;
    for (uint32_t gy = 0; gy < wp.gh; gy++)
        for (uint32_t gx = 0; gx < wp.gw; gx++) {
            int32_t dx = (int32_t)(gx * wp.block) + half - cx, dy = (int32_t)(gy * wp.block) + half - cy;
            uint32_t r = isqrt((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy) * 256);
            wp.grid[(size_t)gy * wp.gw + gx] = (r < RMAX * 16 ? r : RMAX * 16 - 1) << 12 | atan2_turn(dy, dx);
        }
    *logo = n;
    *block = wp.block;
    return 0;
}

/* x in 0..65536 (Q16) to x^(1/2), x^(1/4), x^(5/8) */
static uint32_t root2(uint32_t x) { return isqrt((uint64_t)x << 16); }
static uint32_t root4(uint32_t x) { return root2(root2(x)); }
static uint32_t pow58(uint32_t x) { return (uint32_t)((uint64_t)root2(x) * root2(root4(x)) >> 16); }

uint32_t ck_whirl_frame(uint32_t t)
{
    uint32_t seen = 0;
    if (!wp.fb)
        return 0;
    uint32_t R = (wp.w < wp.h ? wp.w : wp.h) / 2;
    uint32_t wa = t >= FADE_MS ? 256 : t * 256 / FADE_MS;
    uint32_t rot = (uint32_t)((uint64_t)t * ARMS * 4096 / TURN_MS) & 4095;

    /* the orbit: d (16ths of a pixel from the centre), angle phi, spin, size */
    const uint32_t ramp = CK_WHIRL_ORBIT_MS - CK_WHIRL_LOGO_FROM_MS, fall = CK_WHIRL_MERGE_MS - CK_WHIRL_ORBIT_MS;
    const uint64_t d0 = (uint64_t)wp.side * 16 * 2 / 5;
    /* the angular speed on reaching the orbit, per ms in 1/65536ths of a step: ORBITS * 5/8 / fall */
    const uint64_t w0 = (uint64_t)ORBITS * 5 * 65536 / 8 / fall;
    uint64_t d = 0, phi = 0;
    uint32_t fade = 256;
    int show = wp.logo && t < CK_WHIRL_MERGE_MS;
    if (t <= CK_WHIRL_LOGO_FROM_MS) {
        d = 0, phi = 0;
    } else if (t < CK_WHIRL_ORBIT_MS) {
        uint64_t e = (uint64_t)(t - CK_WHIRL_LOGO_FROM_MS) * 65536 / ramp;
        d = d0 * (e * e * (3 * 65536 - 2 * e) >> 32) >> 16; /* eased out to the orbit */
        phi = w0 * (t - CK_WHIRL_LOGO_FROM_MS) >> 16;
    } else if (show) {
        uint32_t left = (uint32_t)((uint64_t)(CK_WHIRL_MERGE_MS - t) * 65536 / fall);
        d = d0 * root4(left) >> 16;
        phi = (w0 * ramp >> 16) + ORBITS - ((uint64_t)ORBITS * pow58(left) >> 16);
        if (CK_WHIRL_MERGE_MS - t < GONE_MS)
            fade = (CK_WHIRL_MERGE_MS - t) * 256 / GONE_MS;
    }
    uint32_t pa = (uint32_t)(4096 - (phi & 4095)) & 4095;   /* anticlockwise on the screen */
    uint32_t spin = (uint32_t)(2 * phi) & 4095;               /* turns the picture anticlockwise */
    uint32_t size = (uint32_t)(64 + (d0 ? 192 * (d >= d0 ? d0 : d) / d0 : 192)); /* Q8 */
    if (t < CK_WHIRL_ORBIT_MS)
        size = 256;
    uint32_t zoom = 65536 / size; /* Q8, picture pixels per screen pixel */
    int64_t ox = (int64_t)d * sin_t[(pa + 1024) & 4095] >> 14, oy = (int64_t)d * sin_t[pa] >> 14;
    int64_t cs = sin_t[(spin + 1024) & 4095], sn = sin_t[spin];
    int32_t half = (int32_t)wp.side / 2;
    int64_t reach = (int64_t)half * 24 * size >> 8; /* 16ths: 1.5 half-squares, as drawn */

    /* the ripple from the plunge: its radius in 16ths, and its strength */
    uint32_t rip = 0, rip_r = 0, rip_w = R * 16 / 10 + 16;
    if (wp.logo && t >= CK_WHIRL_MERGE_MS && t < CK_WHIRL_MS) {
        uint32_t since = t - CK_WHIRL_MERGE_MS, span = CK_WHIRL_MS - CK_WHIRL_MERGE_MS;
        rip = 256 - since * 256 / span;
        rip_r = (uint32_t)((uint64_t)since * R * 16 * 8 / 5 / span);
    }

    int32_t cx = (int32_t)wp.w / 2, cy = (int32_t)wp.h / 2, bh = (int32_t)wp.block / 2;
    for (uint32_t gy = 0; gy < wp.gh; gy++) {
        const uint32_t *g = wp.grid + (size_t)gy * wp.gw;
        int64_t vy = ((int64_t)(gy * wp.block) + bh - cy) * 16 - oy;
        for (uint32_t gx = 0; gx < wp.gw; gx++) {
            uint32_t rq = g[gx] >> 12, r = rq >> 4, a = g[gx] & 4095;
            uint32_t ph = (ARMS * a + (WIND * q_water[r] >> 16) + rot) & 4095;
            uint32_t v = (uint32_t)(sin_t[(ph + 1024) & 4095] + 16384) >> 7; /* 0..256 */
            v = v * v >> 8;
            uint32_t base = base_c[r], arm = arm_c[r], c = 0;
            for (int s = 0; s <= 16; s += 8) {
                uint32_t x = ch(base, s) + (ch(arm, s) * v >> 8);
                c |= (x > 255 ? 255 : x) << s;
            }
            if (rip) {
                uint32_t off = rq > rip_r ? rq - rip_r : rip_r - rq;
                if (off < rip_w) {
                    uint32_t k = (rip_w - off) * 256 / rip_w;
                    c = add_sat(c, scale(RGB(150, 255, 240), (k * k >> 8) * rip >> 8));
                }
            }
            if (wa < 256)
                c = lerp(wp.bg, c, wa);
            if (show) {
                int64_t vx = ((int64_t)(gx * wp.block) + bh - cx) * 16 - ox;
                if (vx < reach && vx > -reach && vy < reach && vy > -reach) {
                    int32_t sx = half + (int32_t)(((vx * cs - vy * sn) * zoom + (1ll << 25)) >> 26);
                    int32_t sy = half + (int32_t)(((vx * sn + vy * cs) * zoom + (1ll << 25)) >> 26);
                    if (sx >= 0 && sy >= 0 && sx < (int32_t)wp.side && sy < (int32_t)wp.side) {
                        uint32_t px = wp.snap[(size_t)sy * wp.side + sx], cov = (px >> 24) * fade >> 8;
                        if (cov)
                            c = lerp(c, px & 0xffffffu, cov + (cov >> 7));
                        seen += cov >= 64;
                    }
                }
            }
            wp.row[gx] = wp.format ? c : swap_rb(c);
        }
        uint32_t y0 = gy * wp.block, y1 = y0 + wp.block < wp.h ? y0 + wp.block : wp.h;
        for (uint32_t y = y0; y < y1; y++) {
            volatile uint32_t *dst = wp.fb + (size_t)y * wp.pitch;
            uint32_t x = 0;
            for (uint32_t gx = 0; gx < wp.gw; gx++) {
                uint32_t c = wp.row[gx], end = x + wp.block < wp.w ? x + wp.block : wp.w;
                for (; x < end; x++)
                    dst[x] = c;
            }
        }
    }
    return seen;
}
