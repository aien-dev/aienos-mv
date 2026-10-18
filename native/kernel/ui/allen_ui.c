/* allen_ui.c -- Allen's window; see allen_ui.h.
 *
 * Two RAM layers the size of the screen: `base` holds what never changes
 * (background, window, title, status, footer) and is drawn once; `back` is
 * base plus the live parts (orb, bubbles, input). A redraw copies base into
 * back over a rectangle, draws the live parts clipped to it, and copies that
 * rectangle to the screen. Colors are 0x00RRGGBB in RAM; the copy to the
 * screen swaps red and blue for an RGBX framebuffer. Layout is designed on a
 * 1920 x 1080 grid and scaled to the screen (k, 1/256). */
#include "allen_ui.h"
#include "allen_font.h"

#define MSG_MAX 16
#define TEXT_MAX 160
#define LINES_MAX 6

struct rect {
    int32_t x, y, w, h;
};

struct msg {
    uint8_t allen; /* 0: typed by the person, 1: Allen */
    uint16_t len;
    char text[TEXT_MAX];
};

static struct {
    int on;
    volatile uint32_t *fb;
    uint32_t w, h, pitch, format;
    uint32_t *base, *back;
    uint32_t k; /* 1/256 px per design px */
    ck_af_face title, body, small;
    struct rect win, orb, convo, input, map;
    int map_on; /* the screen has room for the notes map beside the window */
    int32_t orb_r, radius;
    const char *footer;
    struct msg msgs[MSG_MAX];
    unsigned nmsg;
    char input_text[TEXT_MAX];
    unsigned input_len;
    unsigned shown;  /* letters of the newest Allen bubble on screen */
    uint64_t hz, t0, next_letter, next_orb, next_caret, speak_until;
    int caret;
} ui;

/* ---- scale and colors ---- */

static int32_t S(int32_t v) { return (int32_t)(((int64_t)v * ui.k + 128) / 256); }
static uint32_t ms(uint64_t now) { return (uint32_t)((now - ui.t0) * 1000u / ui.hz); }

#define RGB(r, g, b) ((uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
static uint32_t ch(uint32_t c, int s) { return c >> s & 255u; }

static uint32_t mix(uint32_t d, uint32_t c, uint32_t a) /* a: 0..255 */
{
    if (a == 0)
        return d;
    if (a >= 255)
        return c;
    uint32_t o = 0;
    for (int s = 0; s <= 16; s += 8) {
        uint32_t v = (ch(c, s) * a + ch(d, s) * (255 - a) + 128) * 257 >> 16;
        o |= v << s;
    }
    return o;
}

static uint32_t lerp(uint32_t a, uint32_t b, uint32_t t) /* t: 0..256 */
{
    uint32_t o = 0;
    for (int s = 0; s <= 16; s += 8)
        o |= ((ch(a, s) * (256 - t) + ch(b, s) * t) >> 8) << s;
    return o;
}

static uint32_t add_sat(uint32_t d, uint32_t c, uint32_t a) /* d + c * a / 255 per channel */
{
    uint32_t o = 0;
    for (int s = 0; s <= 16; s += 8) {
        uint32_t v = ch(d, s) + ch(c, s) * a / 255;
        o |= (v > 255 ? 255 : v) << s;
    }
    return o;
}

static const uint32_t C_TEXT = RGB(236, 240, 250), C_MUTED = RGB(146, 156, 188), C_ACCENT = RGB(124, 140, 255),
                      C_USER0 = RGB(84, 118, 255), C_USER1 = RGB(146, 92, 255), C_WHITE = RGB(255, 255, 255),
                      C_TEAL = RGB(64, 214, 200);

/* ---- clipping ---- */

static int clip(struct rect *r, const struct rect *c)
{
    int32_t x0 = r->x > c->x ? r->x : c->x, y0 = r->y > c->y ? r->y : c->y;
    int32_t x1 = r->x + r->w < c->x + c->w ? r->x + r->w : c->x + c->w;
    int32_t y1 = r->y + r->h < c->y + c->h ? r->y + r->h : c->y + c->h;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    *r = (struct rect){x0, y0, x1 - x0, y1 - y0};
    return 1;
}

static struct rect grow(struct rect r, int32_t m) { return (struct rect){r.x - m, r.y - m, r.w + 2 * m, r.h + 2 * m}; }

/* ---- shapes ---- */

/* Signed distance from pixel (x, y)'s centre to a rounded rectangle, 1/16 px
 * (negative inside). */
static int32_t sd_rrect(int32_t x, int32_t y, const struct rect *r, int32_t rad)
{
    int64_t px = (int64_t)x * 16 + 8, py = (int64_t)y * 16 + 8;
    int64_t cx = (int64_t)r->x * 16 + (int64_t)r->w * 8, cy = (int64_t)r->y * 16 + (int64_t)r->h * 8;
    int64_t qx = (px > cx ? px - cx : cx - px) - ((int64_t)r->w * 8 - rad * 16);
    int64_t qy = (py > cy ? py - cy : cy - py) - ((int64_t)r->h * 8 - rad * 16);
    int64_t out = qx > 0 && qy > 0 ? (int64_t)ck_af_isqrt((uint64_t)(qx * qx + qy * qy)) : qx > qy ? qx : qy;
    return (int32_t)(out - rad * 16);
}

static uint32_t cover(int32_t d16) /* coverage of a pixel at signed distance d (1/16 px) from an edge */
{
    int32_t v = 8 - d16;
    return v <= 0 ? 0 : v >= 16 ? 255 : (uint32_t)(v * 255 / 16);
}

/* A rounded rectangle filled with color, or a left-to-right gradient c0..c1,
 * at opacity a; border > 0 draws only a ring of that width (px). */
static void rrect(uint32_t *buf, const struct rect *cl, struct rect r, int32_t rad, uint32_t c0, uint32_t c1,
                  uint32_t a, int32_t border)
{
    struct rect d = r;
    if (!clip(&d, cl))
        return;
    for (int32_t y = d.y; y < d.y + d.h; y++) {
        uint32_t *row = buf + (size_t)y * ui.w;
        for (int32_t x = d.x; x < d.x + d.w; x++) {
            int32_t sd = sd_rrect(x, y, &r, rad);
            uint32_t cov = cover(sd);
            if (border > 0) {
                uint32_t inner = cover(sd + border * 16); /* coverage of the shape shrunk by the border */
                cov = cov > inner ? cov - inner : 0;
            }
            if (!cov)
                continue;
            uint32_t c = c0 == c1 ? c0 : lerp(c0, c1, (uint32_t)((x - r.x) * 256 / (r.w ? r.w : 1)));
            row[x] = mix(row[x], c, cov * a / 255);
        }
    }
}

/* Text at pen (x, baseline y); returns the pen after it. */
static int32_t text(uint32_t *buf, const struct rect *cl, ck_af_face *f, int32_t x, int32_t y, const char *s, size_t n,
                    uint32_t color, uint32_t a)
{
    uint64_t pen = (uint64_t)x * 256;
    for (size_t i = 0; i < n && s[i]; i++) {
        const ck_af_glyph *g = ck_af_glyph_get(f, s[i]);
        if (g && g->alpha) {
            int32_t gx = (int32_t)((pen + 128) / 256) + g->ox, gy = y + g->oy;
            struct rect d = {gx, gy, g->w, g->h};
            if (clip(&d, cl))
                for (int32_t yy = d.y; yy < d.y + d.h; yy++) {
                    uint32_t *row = buf + (size_t)yy * ui.w;
                    const uint8_t *m = g->alpha + (size_t)(yy - gy) * g->w;
                    for (int32_t xx = d.x; xx < d.x + d.w; xx++) {
                        uint32_t cov = m[xx - gx];
                        if (cov)
                            row[xx] = mix(row[xx], color, cov * a / 255);
                    }
                }
        }
        pen += ck_af_advance(f, s[i]);
    }
    return (int32_t)((pen + 255) / 256);
}

/* A soft disc: alpha * (1 - d^2/R^2)^2, added (light) or mixed. */
static void glow(uint32_t *buf, const struct rect *cl, int32_t cx, int32_t cy, int32_t R, uint32_t color, uint32_t a,
                 int additive)
{
    struct rect d = {cx - R, cy - R, 2 * R + 1, 2 * R + 1};
    if (R <= 0 || !clip(&d, cl))
        return;
    int64_t R2 = (int64_t)R * R;
    for (int32_t y = d.y; y < d.y + d.h; y++) {
        uint32_t *row = buf + (size_t)y * ui.w;
        int64_t dy = y - cy;
        for (int32_t x = d.x; x < d.x + d.w; x++) {
            int64_t dx = x - cx, d2 = dx * dx + dy * dy;
            if (d2 >= R2)
                continue;
            int64_t f = 256 - d2 * 256 / R2; /* 0..256 */
            uint32_t al = (uint32_t)(a * f * f / 65536);
            row[x] = additive ? add_sat(row[x], color, al) : mix(row[x], color, al);
        }
    }
}

/* ---- the static layer ---- */

/* The aurora: a vertical gradient and three soft lights, worked out in
 * sixteenths of a color step and rounded with a 4 x 4 ordered dither, so the
 * long gradients show no bands. */
static void background(const struct rect *cl)
{
    static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    static const int32_t top[3] = {9, 12, 26}, bottom[3] = {17, 10, 34};
    struct {
        int64_t x, y, r2;
        int32_t c[3]; /* color times strength, /255 */
    } light[3] = {
        {(int64_t)ui.w * 18 / 100, (int64_t)ui.h * 20 / 100, 0, {0, 120 * 90, 140 * 90}},
        {(int64_t)ui.w * 86 / 100, (int64_t)ui.h * 88 / 100, 0, {110 * 80, 50 * 80, 220 * 80}},
        {(int64_t)ui.w / 2, (int64_t)ui.h * 30 / 100, 0, {60 * 40, 70 * 40, 160 * 40}},
    };
    int64_t radius[3] = {(int64_t)ui.h * 70 / 100, (int64_t)ui.h * 75 / 100, (int64_t)ui.h * 45 / 100};
    for (int i = 0; i < 3; i++)
        light[i].r2 = radius[i] * radius[i];
    for (int32_t y = cl->y; y < cl->y + cl->h; y++) {
        uint32_t *row = ui.base + (size_t)y * ui.w;
        int64_t t = (int64_t)y * 256 / ui.h;
        int32_t g16[3];
        for (int k = 0; k < 3; k++)
            g16[k] = (int32_t)((top[k] * (256 - t) + bottom[k] * t) / 16);
        for (int32_t x = cl->x; x < cl->x + cl->w; x++) {
            int64_t v[3] = {g16[0], g16[1], g16[2]};
            for (int i = 0; i < 3; i++) {
                int64_t dx = x - light[i].x, dy = y - light[i].y, d2 = dx * dx + dy * dy;
                if (d2 >= light[i].r2)
                    continue;
                int64_t f = 256 - d2 * 256 / light[i].r2, f2 = f * f; /* 0..65536 */
                for (int k = 0; k < 3; k++)
                    v[k] += light[i].c[k] * f2 / (65536 * 255 / 16);
            }
            uint32_t d = bayer[(y & 3) * 4 + (x & 3)], c = 0;
            for (int k = 0; k < 3; k++) {
                int64_t q = (v[k] + d) >> 4;
                c = c << 8 | (uint32_t)(q > 255 ? 255 : q);
            }
            row[x] = c;
        }
    }
}


static void window(const struct rect *cl)
{
    /* shadow: below and around the window, fading over S(56) */
    struct rect sh = ui.win;
    sh.y += S(18);
    int32_t spread = S(56);
    struct rect d = grow(sh, spread);
    if (clip(&d, cl))
        for (int32_t y = d.y; y < d.y + d.h; y++) {
            uint32_t *row = ui.base + (size_t)y * ui.w;
            for (int32_t x = d.x; x < d.x + d.w; x++) {
                int32_t sd = sd_rrect(x, y, &sh, ui.radius);
                if (sd >= spread * 16)
                    continue;
                int32_t f = sd <= 0 ? 256 : 256 - sd * 256 / (spread * 16);
                row[x] = mix(row[x], RGB(2, 3, 10), (uint32_t)(f * f * 150 / 65536));
            }
        }
    /* glass: a light veil, brighter at the top, and a fine border */
    d = ui.win;
    if (clip(&d, cl))
        for (int32_t y = d.y; y < d.y + d.h; y++) {
            uint32_t *row = ui.base + (size_t)y * ui.w;
            uint32_t a = 26 - (uint32_t)((y - ui.win.y) * 12 / ui.win.h);
            for (int32_t x = d.x; x < d.x + d.w; x++) {
                uint32_t cov = cover(sd_rrect(x, y, &ui.win, ui.radius));
                if (cov)
                    row[x] = mix(mix(row[x], RGB(22, 26, 48), cov * 150 / 255), C_WHITE, cov * a / 255);
            }
        }
    rrect(ui.base, cl, ui.win, ui.radius, C_WHITE, C_WHITE, 40, S(1) > 1 ? S(1) : 1);
}

static void header_text(const struct rect *cl)
{
    int32_t cx = ui.win.x + ui.win.w / 2;
    const char *name = "Allen";
    int32_t tw = (int32_t)ck_af_measure(&ui.title, name, 5);
    text(ui.base, cl, &ui.title, cx - tw / 2, ui.win.y + S(186), name, 5, C_TEXT, 255);
    /* status: a teal dot, "Running on AIENOS", a separator dot, "All local" */
    const char *s1 = "Running on AIENOS", *s2 = "All local";
    int32_t w1 = (int32_t)ck_af_measure(&ui.small, s1, 17), w2 = (int32_t)ck_af_measure(&ui.small, s2, 9);
    int32_t gap = S(14), dot = S(4), total = dot * 2 + gap + w1 + gap * 2 + w2;
    int32_t x = cx - total / 2, base = ui.win.y + S(222);
    int32_t mid = base - S(5);
    glow(ui.base, cl, x + dot, mid, dot * 3, C_TEAL, 90, 1);
    rrect(ui.base, cl, (struct rect){x, mid - dot, dot * 2, dot * 2}, dot, C_TEAL, C_TEAL, 255, 0);
    x = text(ui.base, cl, &ui.small, x + dot * 2 + gap, base, s1, 17, C_MUTED, 255) + gap;
    rrect(ui.base, cl, (struct rect){x - S(2), mid - S(2), S(4), S(4)}, S(2), C_MUTED, C_MUTED, 160, 0);
    text(ui.base, cl, &ui.small, x + gap, base, s2, 9, C_MUTED, 255);
    /* divider */
    rrect(ui.base, cl, (struct rect){ui.win.x + S(48), ui.win.y + S(250), ui.win.w - S(96), S(1) > 1 ? S(1) : 1}, 0,
          C_WHITE, C_WHITE, 22, 0);
    /* footer */
    if (ui.footer) {
        size_t n = 0;
        while (ui.footer[n] && n < 120)
            n++;
        int32_t fw = (int32_t)ck_af_measure(&ui.small, ui.footer, n);
        text(ui.base, cl, &ui.small, cx - fw / 2, ui.win.y + ui.win.h - S(26), ui.footer, n, C_MUTED, 200);
    }
}

/* ---- the notes map ---- */

/* What the map shows; kept apart from ui, so it survives ck_allen_ui_start. */
static struct {
    unsigned n, ne, total, links;
    int lit;
    uint8_t ea[CK_ALLEN_UI_MAP_LINKS], eb[CK_ALLEN_UI_MAP_LINKS];
    char label[TEXT_MAX];
    unsigned label_len;
} map;

static int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t iabs(int32_t a) { return a < 0 ? -a : a; }

/* A line th px wide from (x0, y0) to (x1, y1), anti-aliased, added as light. */
static void seg(uint32_t *buf, const struct rect *cl, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t th,
                uint32_t color, uint32_t a)
{
    struct rect d = {imin(x0, x1) - th - 1, imin(y0, y1) - th - 1, iabs(x1 - x0) + 2 * th + 3,
                     iabs(y1 - y0) + 2 * th + 3};
    if (!clip(&d, cl))
        return;
    int64_t ax = (int64_t)x0 * 16 + 8, ay = (int64_t)y0 * 16 + 8;
    int64_t vx = (int64_t)(x1 - x0) * 16, vy = (int64_t)(y1 - y0) * 16, vv = vx * vx + vy * vy;
    for (int32_t y = d.y; y < d.y + d.h; y++) {
        uint32_t *row = buf + (size_t)y * ui.w;
        for (int32_t x = d.x; x < d.x + d.w; x++) {
            int64_t wx = (int64_t)x * 16 + 8 - ax, wy = (int64_t)y * 16 + 8 - ay;
            int64_t t = vv ? wx * vx + wy * vy : 0;
            t = t < 0 ? 0 : t > vv ? vv : t;
            int64_t qx = wx - (vv ? vx * t / vv : 0), qy = wy - (vv ? vy * t / vv : 0);
            uint32_t cov = cover((int32_t)ck_af_isqrt((uint64_t)(qx * qx + qy * qy)) - th * 8);
            if (cov)
                row[x] = add_sat(row[x], color, cov * a / 255);
        }
    }
}

/* Decimal digits of v into s; returns how many. */
static unsigned put_num(char *s, unsigned v)
{
    char t[10];
    unsigned n = 0, k = 0;
    do
        t[n++] = (char)('0' + v % 10);
    while ((v /= 10) && n < sizeof t);
    while (n)
        s[k++] = t[--n];
    return k;
}

/* Where point j sits: the newest at the centre, then a golden-angle spiral,
 * spread by the square root of the order so the points cover the disc evenly
 * (a few notes stay nearer the centre). */
static void map_point(unsigned j, int32_t cx, int32_t cy, int32_t R, int32_t *x, int32_t *y)
{
    unsigned spread = map.n > 7 ? map.n - 1 : 6;
    int32_t rad = (int32_t)((int64_t)R * ck_af_isqrt((uint64_t)j * 65536u / spread) / 256);
    int32_t ang = (int32_t)(j * 275u / 2u % 360u);
    *x = cx + (int32_t)((int64_t)rad * ck_af_cos(ang) / 16384);
    *y = cy + (int32_t)((int64_t)rad * ck_af_sin(ang) / 16384);
}

static void map_panel(const struct rect *cl)
{
    if (!ui.map_on)
        return;
    struct rect p = ui.map, c = *cl;
    if (!clip(&c, &p))
        return;
    rrect(ui.base, &c, p, S(26), RGB(22, 26, 48), RGB(22, 26, 48), 120, 0);
    rrect(ui.base, &c, p, S(26), C_WHITE, C_WHITE, 34, S(1) > 1 ? S(1) : 1);
    int32_t left = p.x + S(28);
    text(ui.base, &c, &ui.body, left, p.y + S(50), "Notes", 5, C_TEXT, 255);
    char line[48];
    unsigned k = put_num(line, map.total);
    const char *w1 = map.total == 1 ? " note, " : " notes, ";
    for (; *w1; w1++)
        line[k++] = *w1;
    k += put_num(line + k, map.links);
    const char *w2 = map.links == 1 ? " link" : " links";
    for (; *w2; w2++)
        line[k++] = *w2;
    text(ui.base, &c, &ui.small, left, p.y + S(78), line, k, C_MUTED, 230);
    /* the field: between the heading and the label line */
    int32_t top = p.y + S(100), bottom = p.y + p.h - S(64);
    int32_t cx = p.x + p.w / 2, cy = (top + bottom) / 2;
    int32_t R = imin(p.w / 2 - S(34), (bottom - top) / 2 - S(14));
    if (map.n == 0) {
        static const char hint[] = "Saved notes appear here";
        int32_t hw = (int32_t)ck_af_measure(&ui.small, hint, sizeof hint - 1);
        text(ui.base, &c, &ui.small, cx - hw / 2, cy, hint, sizeof hint - 1, C_MUTED, 170);
        return;
    }
    int32_t px[CK_ALLEN_UI_MAP_NODES], py[CK_ALLEN_UI_MAP_NODES];
    unsigned deg[CK_ALLEN_UI_MAP_NODES] = {0};
    for (unsigned j = 0; j < map.n; j++)
        map_point(j, cx, cy, R, &px[j], &py[j]);
    int32_t th = S(1) > 1 ? S(1) : 1;
    for (unsigned i = 0; i < map.ne; i++) {
        unsigned a = map.ea[i], b = map.eb[i];
        int hot = (int)a == map.lit || (int)b == map.lit;
        seg(ui.base, &c, px[a], py[a], px[b], py[b], hot ? th + 1 : th, hot ? C_TEAL : C_ACCENT, hot ? 170 : 70);
        deg[a]++;
        deg[b]++;
    }
    for (unsigned j = 0; j < map.n; j++) {
        int32_t r = S(5) + (int32_t)(deg[j] < 6 ? deg[j] : 6) * S(1);
        int hot = (int)j == map.lit;
        if (hot)
            r += S(3);
        glow(ui.base, &c, px[j], py[j], r * (hot ? 6 : 3), hot ? C_TEAL : RGB(70, 90, 255), hot ? 170 : 80, 1);
        rrect(ui.base, &c, (struct rect){px[j] - r, py[j] - r, 2 * r, 2 * r}, r, hot ? RGB(220, 255, 250)
              : RGB(150, 190, 255), hot ? C_TEAL : RGB(110, 70, 230), 255, 0);
    }
    if (map.lit >= 0 && map.label_len) {
        /* the lit note's text on the bottom line, cut with "..." to fit */
        int32_t avail = p.w - S(56), dots = (int32_t)ck_af_measure(&ui.small, "...", 3);
        unsigned n = map.label_len;
        int cut = 0;
        while (n && (int32_t)ck_af_measure(&ui.small, map.label, n) + (cut ? dots : 0) > avail)
            n--, cut = 1;
        int32_t lw = (int32_t)ck_af_measure(&ui.small, map.label, n) + (cut ? dots : 0);
        int32_t x = text(ui.base, &c, &ui.small, cx - lw / 2, p.y + p.h - S(30), map.label, n, C_TEXT, 255);
        if (cut)
            text(ui.base, &c, &ui.small, x, p.y + p.h - S(30), "...", 3, C_TEXT, 255);
    }
}

static void draw_base(const struct rect *cl)
{
    background(cl);
    window(cl);
    header_text(cl);
    map_panel(cl);
}

/* ---- the live layer ---- */

/* Allen's orb: a halo that breathes (a 4.2 s cycle) and swells while Allen
 * answers, a comet that circles it, and a lit sphere. */
static void orb(const struct rect *cl, uint32_t t)
{
    int32_t cx = ui.orb.x + ui.orb.w / 2, cy = ui.orb.y + ui.orb.h / 2, r = ui.orb_r;
    int32_t breath = (ck_af_sin((int32_t)(t % 4200u * 360u / 4200u)) + 16384) / 128; /* 0..256 */
    int speaking = ui.speak_until && t < ui.speak_until;
    uint32_t halo = (uint32_t)(70 + breath * 60 / 256 + (speaking ? 90 : 0));
    glow(ui.back, cl, cx, cy, r * 3, RGB(70, 90, 255), halo, 1);
    glow(ui.back, cl, cx, cy, r * 2, RGB(60, 200, 255), halo / 2, 1);
    /* the comet: a ring at 1.5 r, bright ahead of the angle, fading behind it */
    int32_t rr = r * 3 / 2, th = S(2) > 1 ? S(2) : 1;
    int32_t ang = (int32_t)(t % (speaking ? 1400u : 3600u) * 360u / (speaking ? 1400u : 3600u));
    int64_t ux = ck_af_cos(ang), uy = ck_af_sin(ang);
    struct rect d = {cx - rr - th - 1, cy - rr - th - 1, 2 * (rr + th + 1) + 1, 2 * (rr + th + 1) + 1};
    if (clip(&d, cl))
        for (int32_t y = d.y; y < d.y + d.h; y++) {
            uint32_t *row = ui.back + (size_t)y * ui.w;
            for (int32_t x = d.x; x < d.x + d.w; x++) {
                int64_t dx = (int64_t)(x - cx) * 16 + 8, dy = (int64_t)(y - cy) * 16 + 8;
                int64_t dist = (int64_t)ck_af_isqrt((uint64_t)(dx * dx + dy * dy));
                int64_t off = dist - rr * 16;
                if (off < 0)
                    off = -off;
                uint32_t cov = cover((int32_t)(off - th * 8));
                if (!cov || dist == 0)
                    continue;
                int64_t dot = (dx * ux + dy * uy) / dist; /* cos of the angle to the comet, 1/16384 */
                if (dot <= 0)
                    continue;
                int64_t f = dot * dot / 16384 * dot / 16384 * dot / 16384; /* cos^4 */
                uint32_t a = (uint32_t)(cov * f * (speaking ? 255 : 200) / 255 / 16384);
                row[x] = add_sat(row[x], RGB(170, 220, 255), a);
            }
        }
    /* the sphere: lit from the upper left */
    d = (struct rect){cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3};
    int32_t hx = cx - r * 2 / 5, hy = cy - r * 2 / 5;
    uint32_t lift = (uint32_t)(breath / 8 + (speaking ? 40 : 0));
    if (clip(&d, cl))
        for (int32_t y = d.y; y < d.y + d.h; y++) {
            uint32_t *row = ui.back + (size_t)y * ui.w;
            for (int32_t x = d.x; x < d.x + d.w; x++) {
                int64_t dx = (int64_t)(x - cx) * 16 + 8, dy = (int64_t)(y - cy) * 16 + 8;
                int32_t dist = (int32_t)ck_af_isqrt((uint64_t)(dx * dx + dy * dy));
                uint32_t cov = cover(dist - r * 16);
                if (!cov)
                    continue;
                int64_t ex = x - hx, ey = y - hy;
                uint32_t t2 = (uint32_t)(ck_af_isqrt((uint64_t)(ex * ex + ey * ey)) * 256 / (uint64_t)(r * 17 / 10));
                if (t2 > 256)
                    t2 = 256;
                uint32_t c = t2 < 128 ? lerp(RGB(232, 246, 255), RGB(110, 150, 255), t2 * 2)
                                      : lerp(RGB(110, 150, 255), RGB(72, 40, 190), (t2 - 128) * 2);
                c = add_sat(c, RGB(60, 80, 120), lift);
                row[x] = mix(row[x], c, cov);
            }
        }
}

/* Word-wraps s (n letters) to width px with face f: line starts and lengths. */
static unsigned wrap(ck_af_face *f, const char *s, unsigned n, int32_t width, unsigned *st, unsigned *ln)
{
    unsigned lines = 0, i = 0;
    while (i < n && lines < LINES_MAX) {
        while (i < n && s[i] == ' ')
            i++;
        unsigned start = i, last_space = 0, j = i;
        int32_t w = 0;
        while (j < n) {
            int32_t aw = (int32_t)((ck_af_advance(f, s[j]) + 255) / 256);
            if (w + aw > width && j > start)
                break;
            if (s[j] == ' ')
                last_space = j;
            w += aw;
            j++;
        }
        if (j < n && last_space > start)
            j = last_space;
        st[lines] = start;
        ln[lines] = j - start;
        lines++;
        i = j;
    }
    return lines;
}

/* Lays the bubbles out from the bottom of the conversation area up; draws the
 * ones that meet cl (when buf is set) and returns the newest one's rect. */
static struct rect bubbles(uint32_t *buf, const struct rect *cl)
{
    int32_t pad_x = S(20), pad_y = S(13), line_h = S(28), gap = S(14), avatar = S(10);
    int32_t maxw = ui.convo.w * 64 / 100;
    int32_t bottom = ui.convo.y + ui.convo.h;
    struct rect newest = {0, 0, 0, 0};
    struct rect area = ui.convo;
    struct rect c = *cl;
    if (!clip(&c, &area))
        return newest;
    for (unsigned k = ui.nmsg; k-- > 0;) {
        const struct msg *m = &ui.msgs[k];
        unsigned st[LINES_MAX], ln[LINES_MAX];
        unsigned lines = wrap(&ui.body, m->text, m->len, maxw - 2 * pad_x, st, ln);
        int32_t tw = 0;
        for (unsigned i = 0; i < lines; i++) {
            int32_t w = (int32_t)ck_af_measure(&ui.body, m->text + st[i], ln[i]);
            if (w > tw)
                tw = w;
        }
        int32_t bw = tw + 2 * pad_x, bh = (int32_t)lines * line_h + 2 * pad_y - S(4);
        int32_t bx = m->allen ? ui.convo.x + avatar * 4 + S(12) : ui.convo.x + ui.convo.w - bw;
        struct rect b = {bx, bottom - bh, bw, bh};
        if (k == ui.nmsg - 1)
            newest = b;
        if (b.y + b.h < ui.convo.y)
            break;
        if (buf) {
            if (m->allen) {
                rrect(buf, &c, b, S(20), C_WHITE, C_WHITE, 24, 0);
                rrect(buf, &c, b, S(20), C_WHITE, C_WHITE, 30, 1);
                int32_t ax = ui.convo.x + avatar * 2, ay = b.y + b.h - avatar - S(2);
                glow(buf, &c, ax, ay, avatar * 2, RGB(70, 110, 255), 120, 1);
                rrect(buf, &c, (struct rect){ax - avatar, ay - avatar, avatar * 2, avatar * 2}, avatar,
                      RGB(150, 190, 255), RGB(110, 70, 230), 255, 0);
            } else {
                rrect(buf, &c, b, S(20), C_USER0, C_USER1, 255, 0);
            }
            unsigned limit = m->allen && k == ui.nmsg - 1 ? ui.shown : m->len;
            for (unsigned i = 0; i < lines; i++) {
                unsigned n = ln[i];
                if (st[i] >= limit)
                    break;
                if (st[i] + n > limit)
                    n = limit - st[i];
                text(buf, &c, &ui.body, b.x + pad_x, b.y + pad_y + S(20) + (int32_t)i * line_h, m->text + st[i], n,
                     m->allen ? C_TEXT : C_WHITE, 255);
            }
        }
        bottom = b.y - gap;
    }
    if (buf && ui.nmsg == 0) {
        const char *hint = "Say hello to Allen";
        int32_t hw = (int32_t)ck_af_measure(&ui.body, hint, 18);
        text(buf, &c, &ui.body, ui.convo.x + (ui.convo.w - hw) / 2, ui.convo.y + ui.convo.h / 2, hint, 18, C_MUTED,
             170);
    }
    return newest;
}

static void input_field(const struct rect *cl)
{
    struct rect r = ui.input;
    int32_t rad = r.h / 2;
    rrect(ui.back, cl, r, rad, RGB(4, 6, 16), RGB(4, 6, 16), 90, 0);
    rrect(ui.back, cl, r, rad, C_ACCENT, C_ACCENT, 120, S(1) > 1 ? S(1) + 1 : 2);
    int32_t pad = S(28), base = r.y + r.h / 2 + S(8);
    const char *hint = "Enter";
    int32_t hw = (int32_t)ck_af_measure(&ui.small, hint, 5);
    int32_t hx = r.x + r.w - pad - hw;
    rrect(ui.back, cl, (struct rect){hx - S(10), r.y + r.h / 2 - S(14), hw + S(20), S(28)}, S(8), C_WHITE, C_WHITE,
          18, 0);
    text(ui.back, cl, &ui.small, hx, base - S(1), hint, 5, C_MUTED, 220);
    struct rect tc = {r.x + pad, r.y, hx - S(24) - (r.x + pad), r.h};
    if (!clip(&tc, cl))
        tc.w = 0;
    int32_t x = r.x + pad;
    if (ui.input_len == 0) {
        if (tc.w)
            text(ui.back, &tc, &ui.body, x, base, "Type a message", 14, C_MUTED, 150);
    } else {
        int32_t avail = hx - S(30) - x;
        unsigned from = 0;
        while (from < ui.input_len &&
               (int32_t)ck_af_measure(&ui.body, ui.input_text + from, ui.input_len - from) > avail)
            from++;
        if (tc.w)
            x = text(ui.back, &tc, &ui.body, x, base, ui.input_text + from, ui.input_len - from, C_TEXT, 255);
        else
            x += (int32_t)ck_af_measure(&ui.body, ui.input_text + from, ui.input_len - from);
    }
    if (ui.caret)
        rrect(ui.back, cl, (struct rect){x + S(3), r.y + r.h / 2 - S(15), S(2) > 2 ? S(2) : 2, S(30)}, S(1),
              C_ACCENT, C_ACCENT, 255, 0);
}

/* ---- redraw ---- */

static void present(const struct rect *r)
{
    for (int32_t y = r->y; y < r->y + r->h; y++) {
        const uint32_t *src = ui.back + (size_t)y * ui.w + r->x;
        volatile uint32_t *dst = ui.fb + (size_t)y * ui.pitch + r->x;
        if (ui.format == 1)
            for (int32_t x = 0; x < r->w; x++)
                dst[x] = src[x];
        else
            for (int32_t x = 0; x < r->w; x++) {
                uint32_t c = src[x];
                dst[x] = (c & 0xff00u) | (c >> 16 & 0xffu) | (c & 0xffu) << 16;
            }
    }
}

static void redraw(struct rect r, uint32_t t)
{
    struct rect screen = {0, 0, (int32_t)ui.w, (int32_t)ui.h};
    if (!clip(&r, &screen))
        return;
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        const uint32_t *src = ui.base + (size_t)y * ui.w + r.x;
        uint32_t *dst = ui.back + (size_t)y * ui.w + r.x;
        for (int32_t x = 0; x < r.w; x++)
            dst[x] = src[x];
    }
    orb(&r, t);
    (void)bubbles(ui.back, &r);
    input_field(&r);
    present(&r);
}

static struct rect orb_area(void) { return grow(ui.orb, ui.orb_r * 2 + S(4)); }

/* ---- the interface ---- */

static size_t face_bytes(uint32_t cap)
{
    size_t s = cap * 256u / 20u; /* as ck_af_face_init */
    size_t w = (26 * s + 2 * 1024) / 256 + 6, h = (33 * s + 2 * 1024) / 256 + 6;
    return CK_AF_COUNT * w * h;
}

static uint32_t scale_for(uint32_t w, uint32_t h)
{
    uint32_t kw = w * 256u / 1920u, kh = h * 256u / 1080u;
    return kw < kh ? kw : kh;
}

#define CAP_TITLE 30
#define CAP_BODY 15
#define CAP_SMALL 11

size_t ck_allen_ui_mem_bytes(uint32_t w, uint32_t h)
{
    uint32_t k = scale_for(w, h);
    return (size_t)w * h * 8 + face_bytes(CAP_TITLE * k / 256) + face_bytes(CAP_BODY * k / 256) +
           face_bytes(CAP_SMALL * k / 256) + 64;
}

int ck_allen_ui_on(void) { return ui.on; }

int ck_allen_ui_start(volatile uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, void *mem,
                      size_t mem_len, uint64_t hz, uint64_t now, const char *footer)
{
    if (!fb || w < 640 || h < 400 || pitch < w || format > 1 || hz == 0 || !mem ||
        mem_len < ck_allen_ui_mem_bytes(w, h))
        return -1;
    ui = (__typeof__(ui)){0};
    ui.fb = fb;
    ui.w = w;
    ui.h = h;
    ui.pitch = pitch;
    ui.format = format;
    ui.hz = hz;
    ui.t0 = now;
    ui.footer = footer;
    ui.k = scale_for(w, h);
    uint8_t *p = mem;
    ui.base = (uint32_t *)p;
    ui.back = (uint32_t *)(p + (size_t)w * h * 4);
    p += (size_t)w * h * 8;
    uint32_t caps[3] = {CAP_TITLE * ui.k / 256, CAP_BODY * ui.k / 256, CAP_SMALL * ui.k / 256};
    ck_af_face *faces[3] = {&ui.title, &ui.body, &ui.small};
    unsigned weights[3] = {950, 1250, 1300};
    for (int i = 0; i < 3; i++) {
        size_t n = face_bytes(caps[i]);
        ck_af_face_init(faces[i], caps[i], weights[i], p, n);
        p += n;
    }
    /* layout (design px on 1920 x 1080, centred) */
    int32_t ww = S(1080), wh = S(800);
    ui.win = (struct rect){((int32_t)w - ww) / 2, ((int32_t)h - wh) / 2, ww, wh};
    ui.radius = S(34);
    ui.orb_r = S(34);
    ui.orb = (struct rect){ui.win.x + ww / 2 - ui.orb_r, ui.win.y + S(92) - ui.orb_r, 2 * ui.orb_r, 2 * ui.orb_r};
    ui.input = (struct rect){ui.win.x + S(48), ui.win.y + wh - S(132), ww - S(96), S(66)};
    ui.convo = (struct rect){ui.win.x + S(48), ui.win.y + S(272), ww - S(96), ui.input.y - S(26) - (ui.win.y + S(272))};
    /* the notes map: right of the window, when it fits on the screen */
    ui.map = (struct rect){ui.win.x + ww + S(40), ((int32_t)h - S(560)) / 2, S(340), S(560)};
    ui.map_on = ui.map.x + ui.map.w <= (int32_t)w - S(8);
    ui.caret = 1;
    struct rect all = {0, 0, (int32_t)w, (int32_t)h};
    draw_base(&all);
    ui.on = 1;
    redraw(all, 0);
    ui.next_orb = now;
    ui.next_caret = now + hz * 53 / 100;
    return 0;
}

void ck_allen_ui_input(const char *buf, unsigned len, uint64_t now)
{
    if (!ui.on)
        return;
    if (len > TEXT_MAX)
        len = TEXT_MAX;
    for (unsigned i = 0; i < len; i++)
        ui.input_text[i] = buf[i];
    ui.input_len = len;
    ui.caret = 1;
    ui.next_caret = now + ui.hz * 53 / 100;
    redraw(ui.input, ms(now));
}

static void push(int allen, const char *s)
{
    if (ui.nmsg == MSG_MAX) {
        for (unsigned i = 1; i < MSG_MAX; i++)
            ui.msgs[i - 1] = ui.msgs[i];
        ui.nmsg--;
    }
    struct msg *m = &ui.msgs[ui.nmsg++];
    unsigned n = 0;
    while (s[n] && n < TEXT_MAX)
        n++;
    for (unsigned i = 0; i < n; i++)
        m->text[i] = s[i];
    m->len = (uint16_t)n;
    m->allen = (uint8_t)allen;
}

void ck_allen_ui_said(const char *line, const char *reply, uint64_t now)
{
    if (!ui.on)
        return;
    if (line)
        push(0, line);
    push(1, reply);
    ui.shown = 0;
    ui.input_len = 0;
    ui.next_letter = now + ui.hz * 35 / 100; /* a short pause, as if thinking */
    ui.speak_until = ms(now) + 1200u + (uint32_t)ui.msgs[ui.nmsg - 1].len * 40u;
    redraw(ui.convo, ms(now));
    redraw(ui.input, ms(now));
}

void ck_allen_ui_tick(uint64_t now)
{
    if (!ui.on)
        return;
    uint32_t t = ms(now);
    if (ui.nmsg && ui.msgs[ui.nmsg - 1].allen && ui.shown < ui.msgs[ui.nmsg - 1].len && now >= ui.next_letter) {
        ui.shown++;
        ui.next_letter = now + ui.hz * 32 / 1000;
        struct rect b = bubbles(0, &ui.convo);
        redraw(b, t);
    }
    if (now >= ui.next_caret) {
        ui.caret ^= 1;
        ui.next_caret = now + ui.hz * 53 / 100;
        redraw(ui.input, t);
    }
    if (now >= ui.next_orb) {
        ui.next_orb = now + ui.hz / 30;
        redraw(orb_area(), t);
    }
}

void ck_allen_ui_map(unsigned n, const uint8_t *ea, const uint8_t *eb, unsigned ne, int lit, const char *label,
                     unsigned total, unsigned links)
{
    map.n = n < CK_ALLEN_UI_MAP_NODES ? n : CK_ALLEN_UI_MAP_NODES;
    map.ne = 0;
    for (unsigned i = 0; i < ne && map.ne < CK_ALLEN_UI_MAP_LINKS; i++)
        if (ea[i] < map.n && eb[i] < map.n && ea[i] != eb[i]) {
            map.ea[map.ne] = ea[i];
            map.eb[map.ne++] = eb[i];
        }
    map.lit = lit >= 0 && (unsigned)lit < map.n ? lit : -1;
    map.label_len = 0;
    while (label && label[map.label_len] && map.label_len < TEXT_MAX)
        map.label_len++;
    for (unsigned i = 0; i < map.label_len; i++)
        map.label[i] = label[i];
    map.total = total;
    map.links = links;
    if (!ui.on || !ui.map_on)
        return;
    draw_base(&ui.map);
    redraw(ui.map, 0);
}
