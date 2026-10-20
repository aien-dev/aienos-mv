/* allen_ui.c -- Allen's home; see allen_ui.h.
 *
 * Layers: the night (ui/night.c) keeps the sky in its own full-screen
 * buffer; `back` is where a frame is put together: for each changed part of
 * the screen the sky as it looks now, then the lights in it, Allen, his
 * words, the bar, all added in order and clipped to that part, which is
 * then copied to the screen. Colours are 0x00RRGGBB in RAM; the copy swaps
 * red and blue for an RGBX framebuffer.
 *
 * Which parts change is kept on a grid of tiles (64 x 32 px): each frame
 * marks the tiles under anything that moved, and each row of marked tiles
 * becomes a strip. Strips never overlap, so they can be drawn by any core in
 * any order: a frame publishes its strips in one word (count and next index)
 * that cores claim one at a time.
 *
 * Calls from the keyboard side (input, said, map, wake) only leave a note
 * under a short lock; the next frame picks it up. The layout is designed on
 * a 1920 x 1080 grid and scaled to the screen (k, 1/256). */
#include "allen_ui.h"
#include "allen_font.h"
#include "flow.h"
#include "mind.h"
#include "night.h"
#include "paint.h"

#define TEXT_MAX 160
#define LINES_MAX 5
#define TW 64
#define TH 32
#define TILES_MAX 16384u
#define JOBS_MAX 1200u
#define FRAME_MS 16u

#define BORN_MS (CK_NIGHT_MERGE_MS + 300u) /* Allen starts to gather */
#define FORMED_MS (BORN_MS + CK_MIND_FORM_MS)
#define LETTER_MS 35u
#define BAR_REST_MS 9000u /* the bar fades this long after the last thing happened */
#define MAP_SHOW_MS 9000u
#define PANEL_STEP_MS 600u /* the panel's lines ease in this far apart */
#define PL_MAX 96
#define PC_MAX 32

/* the setup wizard's panel, as the keyboard side left it and as it is drawn */
struct panel {
    char line[CK_ALLEN_UI_PANEL_LINES][PL_MAX];
    unsigned nline;
    char choice[CK_ALLEN_UI_CHOICES][PC_MAX];
    unsigned nchoice;
    int lit;
    char hint[PL_MAX], aside[64], prompt[48];
    int on;
};

struct words {
    char text[TEXT_MAX];
    unsigned len, lines, st[LINES_MAX], ln[LINES_MAX];
    int32_t x[LINES_MAX]; /* where each line starts, px */
    uint32_t from;        /* the first letter's start, ms */
    uint32_t done;        /* the last letter settled */
    uint32_t fade;        /* fading from here (0: not yet) */
    ck_rect box;
};

/* what the keyboard side has left for the next frame */
static struct {
    volatile char lock;
    volatile int refresh;
    unsigned input_gen, said_gen, map_gen, wake_gen;
    unsigned said_input_gen; /* input_gen when the last words came: keys after them stay in the bar */
    int sleep;
    unsigned sleep_gen;
    char input[TEXT_MAX];
    unsigned input_len;
    char line[TEXT_MAX], reply[TEXT_MAX];
    int has_line;
    int think; /* the line went in but the answer is still coming: he thinks until it does */
    const char *footer;
    unsigned n, ne, total, links;
    int lit;
    uint8_t ea[CK_ALLEN_UI_MAP_LINKS], eb[CK_ALLEN_UI_MAP_LINKS];
    char label[TEXT_MAX];
    struct panel panel;
    unsigned panel_gen;
} note;

static struct {
    int started, on;
    volatile uint32_t *fb;
    int32_t w, h;
    uint32_t pitch, format, k;
    uint32_t *back;
    ck_canvas cv;
    ck_night night;
    ck_mind mind;
    ck_flow flow;     /* the water and the ink round him */
    uint32_t dim;     /* dusk: 0 the sky, DIM_FULL dark (1/256 steps of 1/256); eased, so sleep and waking are never a cut */
    int thinking;     /* an answer is on its way from further off; he thinks until it arrives */
    ck_af_face voice, body, small;
    uint64_t hz, t0;
    uint64_t (*clock)(void);
    uint32_t t, last; /* this frame's time and the last frame's, ms */
    int32_t cx, cy;
    /* the bar */
    const char *footer;
    ck_rect bar, bar_box;
    uint32_t bar_a, bar_was, wake_t, quiet_t; /* bar_a: 0..256; quiet_t: when things last happened */
    char input[TEXT_MAX];
    unsigned input_len;
    uint32_t key_t;
    unsigned seen_input, seen_said, seen_map, seen_wake;
    int asleep; /* dark screen, nothing drawn but the mark */
    unsigned seen_sleep;
    int input_moved;
    /* the conversation */
    struct words now, next; /* now: on screen; next: what Allen will say */
    uint32_t speak_t;       /* when next starts (0: nothing waiting) */
    char guest[TEXT_MAX];
    unsigned guest_len;
    char hint[40]; /* "Speak to Allen", or whatever he is called now */
    unsigned hint_len;
    int32_t guest_x;
    uint32_t guest_t; /* when the guest's line began to rise (0: none) */
    uint32_t shimmer, shimmer_was, level, word_t; /* level: the voice, 0..256 */
    ck_rect guest_box, words_was;
    /* the notes constellation */
    unsigned n, ne, total, links;
    int lit;
    uint8_t ea[CK_ALLEN_UI_MAP_LINKS], eb[CK_ALLEN_UI_MAP_LINKS];
    char label[TEXT_MAX];
    unsigned label_len;
    int32_t mx, my, mr;
    uint32_t map_t, map_a, map_was; /* map_t: when it was last shown (0: never) */
    ck_rect map_box;
    /* the setup wizard's panel */
    struct panel pan;
    unsigned seen_panel;
    uint32_t pl_t[CK_ALLEN_UI_PANEL_LINES]; /* when each line starts to ease in */
    uint32_t pan_a, pan_was, lit_t;
    int lit_was;
    ck_rect pan_box, row_box;
    /* the frame */
    uint8_t tiles[TILES_MAX];
    int32_t tx, ty;
    ck_rect jobs[JOBS_MAX];
    volatile uint64_t ticket; /* count << 16 | next; claimed with compare-and-swap */
    volatile uint32_t done;
    unsigned twinkle_turn;
    ck_rect flash_was, lens_was;
    int32_t ripple_was;
    uint32_t frames, slowest, total_ticks;
} ui;

#define S(v) ((int32_t)(((int64_t)(v) * (int64_t)ui.k + 128) / 256))
#define DIM_FULL (256u << 8)

static void relax(void)
{
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

static void lock(void)
{
    while (__atomic_test_and_set(&note.lock, __ATOMIC_ACQUIRE))
        relax();
}
static void unlock(void) { __atomic_clear(&note.lock, __ATOMIC_RELEASE); }

static uint32_t ms(uint64_t now) { return (uint32_t)((now - ui.t0) * 1000u / ui.hz); }

static unsigned copy_len(const char *s)
{
    unsigned n = 0;
    while (s && s[n])
        n++;
    return n;
}

static unsigned copy(char *d, const char *s, unsigned max)
{
    unsigned n = 0;
    while (s && s[n] && n < max - 1)
        d[n] = s[n], n++;
    d[n] = 0;
    return n;
}

/* ---- text ---- */

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

static int32_t text(ck_canvas *cv, const ck_rect *cl, ck_af_face *f, int32_t x, int32_t y, const char *s, unsigned n,
                    uint32_t color, uint32_t a)
{
    uint64_t pen = (uint64_t)x * 256;
    for (unsigned i = 0; i < n; i++)
        pen = ck_letter(cv, cl, f, pen, y, s[i], color, a, 0);
    return (int32_t)((pen + 255) / 256);
}

static int32_t line_h(void) { return S(40); }
static int32_t words_top(void) { return ui.cy + S(250); }

static void lay_out(struct words *w)
{
    w->lines = wrap(&ui.voice, w->text, w->len, S(900), w->st, w->ln);
    int32_t top = words_top(), x0 = ui.w, x1 = 0;
    for (unsigned i = 0; i < w->lines; i++) {
        int32_t lw = (int32_t)ck_af_measure(&ui.voice, w->text + w->st[i], w->ln[i]);
        w->x[i] = (ui.w - lw) / 2;
        x0 = ck_min(x0, w->x[i]), x1 = ck_max(x1, w->x[i] + lw);
    }
    w->box = w->lines ? (ck_rect){x0 - S(8), top - S(34), x1 - x0 + S(16), (int32_t)w->lines * line_h() + S(30)}
                      : (ck_rect){0, 0, 0, 0};
    unsigned last = w->lines ? w->st[w->lines - 1] + w->ln[w->lines - 1] : 0;
    w->done = w->from + last * LETTER_MS + 700;
}

/* how long words stay once said: time to read them */
static uint32_t read_ms(const struct words *w) { return 6000u + w->len * 70u; }

static uint32_t words_alpha(const struct words *w, uint32_t t)
{
    if (!w->len)
        return 0;
    if (ui.pan.on && !w->fade) /* a question is up: his words stay with it */
        return 256;
    uint32_t out = w->fade ? w->fade : w->done + read_ms(w);
    return 256 - ck_smooth(ck_prog(t, out, out + 450));
}

static void draw_words(const struct words *w, const ck_rect *r, uint32_t t)
{
    uint32_t all = words_alpha(w, t);
    if (!all || !ck_meets(w->box, r))
        return;
    int32_t top = words_top();
    for (unsigned li = 0; li < w->lines; li++) {
        uint64_t pen = (uint64_t)w->x[li] * 256;
        int32_t y = top + (int32_t)li * line_h();
        for (unsigned j = 0; j < w->ln[li]; j++) {
            unsigned i = w->st[li] + j;
            uint32_t at = w->from + i * LETTER_MS;
            if (t <= at) /* not yet: but keep the pen moving so the line stays centred */
                break;
            uint32_t age = t - at, a = ck_smooth(ck_prog(age, 0, 260));
            uint32_t c = ck_mix(CK_RGB(255, 212, 150), CK_RGB(234, 238, 252), ck_smooth(ck_prog(age, 0, 900)));
            int32_t rise = (int32_t)((int64_t)S(6) * (256 - ck_ease_out(ck_prog(age, 0, 360))) >> 8);
            pen = ck_letter(&ui.cv, r, &ui.voice, pen, y + rise, w->text[i], c, a * all >> 8, 0);
        }
    }
}

/* ---- the bar ---- */

static void draw_bar(const ck_rect *r, uint32_t t)
{
    uint32_t A = ui.bar_a;
    const char *hint = ui.pan.on && ui.pan.prompt[0] ? ui.pan.prompt : ui.hint;
    unsigned hint_len = hint == ui.hint ? ui.hint_len : (unsigned)copy_len(hint);
    if (!A || !ck_meets(ui.bar_box, r))
        return;
    ck_rect b = ui.bar, d = ck_grow(b, S(14));
    if (!ck_clip(&d, r))
        d.w = 0;
    int32_t rad = b.h / 2, typing = ui.input_len > 0;
    for (int32_t y = d.y; d.w && y < d.y + d.h; y++) {
        uint32_t *row = ui.back + (size_t)y * (size_t)ui.w;
        for (int32_t x = d.x; x < d.x + d.w; x++) {
            int32_t sd = ck_sd_rrect(x, y, &b, rad);
            uint32_t in = ck_cover(sd);
            if (in)
                row[x] = ck_mix(row[x], CK_RGB(6, 8, 18), (in * 150 >> 8) * A >> 8);
            int32_t edge = ck_abs(sd + 8);
            if (edge < 16)
                row[x] = ck_add(row[x], CK_RGB(255, 255, 255), (uint32_t)(16 - edge) * (typing ? 4u : 3u) * A >> 8);
            if (typing && sd > 0 && sd < S(14) * 16) { /* a soft blue glow round it while typing */
                uint32_t g = 256 - (uint32_t)(sd * 256 / (S(14) * 16));
                row[x] = ck_add(row[x], CK_RGB(110, 130, 255), ((g * g >> 8) * 34 >> 8) * A >> 8);
            }
        }
    }
    int32_t pad = S(30), base = b.y + b.h / 2 + S(7);
    ck_rect tc = {b.x + pad, b.y, b.w - 2 * pad, b.h};
    if (!ck_clip(&tc, r))
        tc.w = 0;
    int32_t x = b.x + pad;
    if (!ui.input_len) {
        if (tc.w)
            text(&ui.cv, &tc, &ui.body, x, base, hint, hint_len, CK_RGB(150, 160, 196), 150 * A >> 8);
    } else {
        int32_t avail = b.w - 2 * pad - S(12);
        unsigned from = 0;
        while (from < ui.input_len &&
               (int32_t)ck_af_measure(&ui.body, ui.input + from, ui.input_len - from) > avail)
            from++;
        if (tc.w)
            x = text(&ui.cv, &tc, &ui.body, x, base, ui.input + from, ui.input_len - from, CK_RGB(236, 240, 252),
                     A);
        else
            x += (int32_t)ck_af_measure(&ui.body, ui.input + from, ui.input_len - from);
    }
    /* the caret breathes rather than blinks */
    uint32_t ca = (uint32_t)(16384 + ck_sin((t - ui.key_t) * 65536u / 1100u + 16384u)) * 200 >> 15;
    if (t - ui.key_t < 500)
        ca = 200;
    ck_rect caret = {x + S(3), b.y + b.h / 2 - S(12), ck_max(S(2), 2), S(24)};
    if (ck_clip(&caret, r))
        for (int32_t y = caret.y; y < caret.y + caret.h; y++)
            for (int32_t xx = caret.x; xx < caret.x + caret.w; xx++) {
                uint32_t *p = ui.back + (size_t)y * (size_t)ui.w + xx;
                *p = ck_add(*p, CK_RGB(150, 170, 255), (40 + ca) * A >> 8);
            }
    if (ui.footer && !ui.input_len && !ui.pan.on) {
        unsigned n = 0;
        while (ui.footer[n])
            n++;
        int32_t fw = (int32_t)ck_af_measure(&ui.small, ui.footer, n);
        text(&ui.cv, r, &ui.small, (ui.w - fw) / 2, b.y + b.h + S(34), ui.footer, n, CK_RGB(130, 140, 176),
             110 * A >> 8);
    }
}

/* ---- the setup wizard's panel ---- */

static int32_t pan_line_y(unsigned i) { return words_top() + S(76) + (int32_t)i * S(31); }
static int choices_up(void) { return ui.pan.on && ui.pan.nchoice && !ui.pan.prompt[0]; }

/* one choice: a soft pill, lit (l 0..256) from a faint outline to a bright, blue-glowing one */
static void draw_pill(const ck_rect *r, const ck_rect *p, const char *s, unsigned n, uint32_t l, uint32_t A)
{
    ck_rect d = ck_grow(*p, S(16));
    if (!ck_clip(&d, r))
        return;
    int32_t rad = p->h / 2;
    for (int32_t y = d.y; y < d.y + d.h; y++) {
        uint32_t *row = ui.back + (size_t)y * (size_t)ui.w;
        for (int32_t x = d.x; x < d.x + d.w; x++) {
            int32_t sd = ck_sd_rrect(x, y, p, rad);
            uint32_t in = ck_cover(sd);
            if (in) {
                row[x] = ck_mix(row[x], CK_RGB(6, 8, 18), (in * 140 >> 8) * A >> 8);
                row[x] = ck_add(row[x], CK_RGB(70, 90, 210), (in * (40 * l >> 8) >> 8) * A >> 8);
            }
            int32_t edge = ck_abs(sd + 8);
            if (edge < 16)
                row[x] = ck_add(row[x], CK_RGB(255, 255, 255), (uint32_t)(16 - edge) * (2u + (4u * l >> 8)) * A >> 8);
            if (l && sd > 0 && sd < S(16) * 16) {
                uint32_t g = 256 - (uint32_t)(sd * 256 / (S(16) * 16));
                row[x] = ck_add(row[x], CK_RGB(110, 130, 255), ((g * g >> 8) * 40 >> 8) * l >> 8);
            }
        }
    }
    int32_t tw = (int32_t)ck_af_measure(&ui.body, s, n);
    uint32_t c = ck_mix(CK_RGB(160, 168, 204), CK_RGB(244, 246, 255), l);
    text(&ui.cv, r, &ui.body, p->x + (p->w - tw) / 2, p->y + p->h / 2 + S(7), s, n, c, (150 + (106 * l >> 8)) * A >> 8);
}

static void draw_panel(const ck_rect *r, uint32_t t)
{
    uint32_t A = ui.pan_a;
    if (!A)
        return;
    if (ck_meets(ui.pan_box, r))
        for (unsigned i = 0; i < ui.pan.nline; i++) {
            if (t <= ui.pl_t[i])
                continue;
            uint32_t age = t - ui.pl_t[i], a = ck_smooth(ck_prog(age, 0, 420));
            int32_t rise = (int32_t)((int64_t)S(6) * (256 - ck_ease_out(ck_prog(age, 0, 520))) >> 8);
            unsigned n = copy_len(ui.pan.line[i]);
            int32_t lw = (int32_t)ck_af_measure(&ui.body, ui.pan.line[i], n);
            text(&ui.cv, r, &ui.body, (ui.w - lw) / 2, pan_line_y(i) + rise, ui.pan.line[i], n, CK_RGB(206, 212, 238),
                 (a * 230 >> 8) * A >> 8);
        }
    if (!choices_up() || !ck_meets(ui.row_box, r))
        return;
    /* the row: centred where the bar sits */
    int32_t pad = S(28), gap = S(20), h = ui.bar.h - S(8), total = 0, wd[CK_ALLEN_UI_CHOICES];
    for (unsigned i = 0; i < ui.pan.nchoice; i++) {
        wd[i] = (int32_t)ck_af_measure(&ui.body, ui.pan.choice[i], copy_len(ui.pan.choice[i])) + 2 * pad;
        total += wd[i] + (i ? gap : 0);
    }
    int32_t x = (ui.w - total) / 2, y = ui.bar.y + (ui.bar.h - h) / 2;
    uint32_t ease = ck_smooth(ck_prog(t, ui.lit_t, ui.lit_t + 220));
    for (unsigned i = 0; i < ui.pan.nchoice; i++) {
        uint32_t l = (int)i == ui.pan.lit ? ease : (int)i == ui.lit_was ? 256 - ease : 0;
        ck_rect p = {x, y, wd[i], h};
        draw_pill(r, &p, ui.pan.choice[i], copy_len(ui.pan.choice[i]), l, A);
        x += wd[i] + gap;
    }
    if (ui.pan.hint[0]) {
        unsigned n = copy_len(ui.pan.hint);
        int32_t hw = (int32_t)ck_af_measure(&ui.small, ui.pan.hint, n);
        text(&ui.cv, r, &ui.small, (ui.w - hw) / 2, ui.bar.y + ui.bar.h + S(34), ui.pan.hint, n, CK_RGB(170, 178, 210),
             170 * A >> 8);
    }
    if (ui.pan.aside[0]) {
        unsigned n = copy_len(ui.pan.aside);
        int32_t aw = (int32_t)ck_af_measure(&ui.small, ui.pan.aside, n);
        text(&ui.cv, r, &ui.small, (ui.w - aw) / 2, ui.bar.y + ui.bar.h + S(64), ui.pan.aside, n, CK_RGB(120, 130, 166),
             110 * A >> 8);
    }
}

/* ---- the guest's line rising into Allen, and the thoughts it becomes ---- */

static void curve(unsigned i, uint32_t t, int32_t *x, int32_t *y, uint32_t *a)
{
    /* thought i leaves the line at its own place and flies on a curve to the heart */
    uint32_t start = ui.guest_t + 150 + i * 40, dur = 850 + ck_hash((int32_t)i, 1, 5) % 300;
    int32_t p = ck_prog(t, start, start + dur);
    *a = t < start || p >= 256 ? 0 : 230;
    int32_t gw = (int32_t)ck_af_measure(&ui.body, ui.guest, ui.guest_len);
    int32_t x0 = (ui.guest_x + gw * (int32_t)(2 * i + 1) / 18) * 16, y0 = (ui.bar.y + ui.bar.h / 2 - S(60)) * 16;
    int32_t x2 = ui.mind.hx, y2 = ui.mind.hy;
    int32_t side = (int32_t)(ck_hash((int32_t)i, 2, 5) % 2u) ? 1 : -1;
    int32_t x1 = (x0 + x2) / 2 + side * S(160 + (int32_t)(ck_hash((int32_t)i, 3, 5) % 140u)) * 16, y1 = (y0 + y2) / 2;
    uint32_t e = ck_smooth(p), f = 256 - e;
    *x = (int32_t)(((int64_t)x0 * f * f + 2 * (int64_t)x1 * f * e + (int64_t)x2 * e * e) >> 16);
    *y = (int32_t)(((int64_t)y0 * f * f + 2 * (int64_t)y1 * f * e + (int64_t)y2 * e * e) >> 16);
    if (p > 200)
        *a = *a * (uint32_t)(256 - p) / 56;
}

static ck_rect guest_area(void)
{
    if (!ui.guest_t)
        return (ck_rect){0, 0, 0, 0};
    int32_t y0 = ui.cy - S(40), y1 = ui.bar.y + ui.bar.h;
    return (ck_rect){ui.w / 2 - S(720), y0, S(1440), y1 - y0};
}

static void draw_guest(const ck_rect *r, uint32_t t)
{
    if (!ui.guest_t || t - ui.guest_t > 2400 || !ck_meets(ui.guest_box, r))
        return;
    uint32_t p = (uint32_t)ck_prog(t, ui.guest_t, ui.guest_t + 900);
    if (p < 256) {
        int32_t y = ui.bar.y + ui.bar.h / 2 + S(7) - (int32_t)((int64_t)S(90) * ck_ease_out((int32_t)p) >> 8);
        text(&ui.cv, r, &ui.body, ui.guest_x, y, ui.guest, ui.guest_len, CK_RGB(196, 208, 255), (256 - p) * 210 >> 8);
    }
    for (unsigned i = 0; i < 9; i++) {
        int32_t x, y;
        uint32_t a;
        curve(i, t, &x, &y, &a);
        if (!a)
            continue;
        ck_glow(&ui.cv, r, x, y, S(9) * 16, CK_RGB(150, 170, 255), a / 4);
        ck_glow(&ui.cv, r, x, y, S(3) * 16, CK_RGB(255, 236, 210), a);
    }
}

/* ---- the notes constellation ---- */

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

/* where star j sits, 16ths of a pixel: the newest at the centre, the rest on
 * a golden-angle spiral spread by the square root of the order */
static void map_point(unsigned j, int32_t *x, int32_t *y)
{
    unsigned spread = ui.n > 7 ? ui.n - 1 : 6;
    int64_t rad = (int64_t)ui.mr * 16 * (int64_t)ck_isqrt((uint64_t)j * 65536u / spread) / 256;
    uint32_t ang = j * 25033u;
    *x = ui.mx * 16 + 8 + (int32_t)(rad * ck_cos(ang) >> 14);
    *y = ui.my * 16 + 8 + (int32_t)(rad * ck_sin(ang) >> 14);
}

static void draw_map(const ck_rect *r, uint32_t t)
{
    uint32_t A = ui.map_a;
    if (!A || !ui.n || !ck_meets(ui.map_box, r))
        return;
    for (unsigned i = 0; i < ui.ne; i++) {
        int32_t x0, y0, x1, y1;
        map_point(ui.ea[i], &x0, &y0);
        map_point(ui.eb[i], &x1, &y1);
        ck_thread(&ui.cv, r, x0, y0, x1, y1, CK_RGB(130, 150, 255), 70 * A >> 8, 70 * A >> 8);
    }
    for (unsigned j = 0; j < ui.n; j++) {
        int32_t x, y;
        map_point(j, &x, &y);
        int lit = (int)j == ui.lit;
        uint32_t pulse = lit ? (uint32_t)(16384 + ck_sin((t - ui.map_t) * 65536u / 1400u)) * 60 >> 15 : 0;
        uint32_t c = lit ? CK_RGB(255, 214, 160) : CK_RGB(206, 220, 255);
        ck_glow(&ui.cv, r, x, y, S(lit ? 16 : 8) * 16, c, (lit ? 70 + pulse : 30) * A >> 8);
        ck_glow(&ui.cv, r, x, y, S(lit ? 4 : 3) * 16, c, (lit ? 255 : 190) * A >> 8);
    }
    /* the spark that carried the note out from Allen */
    if (ui.lit >= 0 && t - ui.map_t < 1100) {
        int32_t lx, ly;
        map_point((unsigned)ui.lit, &lx, &ly);
        int32_t p = ck_prog(t, ui.map_t, ui.map_t + 900);
        for (int k = 0; k < 4 && p < 256; k++) {
            uint32_t e = ck_smooth(ck_clamp(p - k * 18, 0, 256));
            int32_t x = ui.mind.hx + (int32_t)((int64_t)(lx - ui.mind.hx) * e >> 8);
            int32_t y = ui.mind.hy + (int32_t)((int64_t)(ly - ui.mind.hy) * e >> 8) -
                        (int32_t)((int64_t)S(60) * 16 * (e * (256 - e) >> 6) >> 8);
            ck_glow(&ui.cv, r, x, y, S(5) * 16, CK_RGB(255, 222, 176), 220u >> k);
        }
    }
    char line[48];
    unsigned n = put_num(line, ui.total);
    n += copy(line + n, ui.total == 1 ? " note, " : " notes, ", 16);
    n += put_num(line + n, ui.links);
    n += copy(line + n, ui.links == 1 ? " link" : " links", 16);
    int32_t hw = (int32_t)ck_af_measure(&ui.small, "Notes", 5);
    text(&ui.cv, r, &ui.small, ui.mx - hw / 2, ui.my - ui.mr - S(34), "Notes", 5, CK_RGB(170, 180, 214), 170 * A >> 8);
    int32_t lw = (int32_t)ck_af_measure(&ui.small, line, n);
    text(&ui.cv, r, &ui.small, ui.mx - lw / 2, ui.my + ui.mr + S(40), line, n, CK_RGB(150, 160, 196), 150 * A >> 8);
    if (ui.lit >= 0 && ui.label_len) {
        char cut[TEXT_MAX + 4];
        unsigned m = ui.label_len;
        int32_t room = S(300);
        while (m > 0 && (int32_t)ck_af_measure(&ui.small, ui.label, m) + (m < ui.label_len ? S(14) : 0) > room)
            m--;
        for (unsigned i = 0; i < m; i++)
            cut[i] = ui.label[i];
        if (m < ui.label_len)
            cut[m++] = '.', cut[m++] = '.', cut[m++] = '.';
        int32_t cw = (int32_t)ck_af_measure(&ui.small, cut, m);
        text(&ui.cv, r, &ui.small, ui.mx - cw / 2, ui.my + ui.mr + S(68), cut, m, CK_RGB(255, 222, 180), 200 * A >> 8);
    }
}

/* ---- the flash of the merger ---- */

static int flash(uint32_t t, int32_t *rad, uint32_t *a)
{
    if (t < CK_NIGHT_MERGE_MS || t >= CK_NIGHT_MERGE_MS + 1600)
        return 0;
    uint32_t tm = t - CK_NIGHT_MERGE_MS;
    *rad = (S(40) + (int32_t)((int64_t)S(220) * ck_ease_out(ck_prog(tm, 0, 1200)) >> 8)) * 16;
    uint32_t rise = ck_smooth(ck_prog(tm, 0, 120));
    *a = rise * (ck_exp_neg(tm > 120 ? (tm - 120) * 256 / 450 : 0) >> 8) * 230 >> 16;
    return *a > 0;
}

/* The flash fills everything at its height; then the remaining hole's shadow
 * takes its black back (within 300 ms), so the light rings it instead of
 * filling it. */
static void unflash(const ck_rect *r, uint32_t t)
{
    uint32_t tm = t - CK_NIGHT_MERGE_MS;
    if (ui.night.holes != 1 || tm < 100)
        return;
    uint32_t keep = 256 - ck_smooth(ck_prog(tm, 100, 400)); /* of the flash, inside the shadow */
    int32_t hx = ui.night.hole[0].x, hy = ui.night.hole[0].y, rs = ui.night.hole[0].rs;
    ck_rect box = {(hx - rs) >> 4, (hy - rs) >> 4, (2 * rs >> 4) + 2, (2 * rs >> 4) + 2};
    if (rs <= 16 || !ck_clip(&box, r))
        return;
    for (int32_t y = box.y; y < box.y + box.h; y++)
        for (int32_t x = box.x; x < box.x + box.w; x++) {
            int32_t dx = x * 16 + 8 - hx, dy = y * 16 + 8 - hy;
            uint32_t d = ck_isqrt((uint32_t)(dx * dx + dy * dy)); /* 16ths of a pixel */
            if (d >= (uint32_t)rs)
                continue;
            uint32_t in = (uint32_t)rs - d >= 16 ? 256 : ((uint32_t)rs - d) * 16; /* a pixel's soft edge */
            uint32_t k = 256 - (in * (256 - keep) >> 8);
            ui.back[(size_t)y * (size_t)ui.w + x] = ck_scale(ui.back[(size_t)y * (size_t)ui.w + x], k);
        }
}

static ck_rect flash_box(uint32_t t)
{
    int32_t rad;
    uint32_t a;
    if (!flash(t, &rad, &a))
        return (ck_rect){0, 0, 0, 0};
    int32_t r = (rad >> 4) + 2;
    return (ck_rect){ui.cx - r, ui.cy - r, 2 * r + 1, 2 * r + 1};
}

/* ---- putting a part of the screen together ---- */

static void present(const ck_rect *r)
{
    for (int32_t y = r->y; y < r->y + r->h; y++) {
        const uint32_t *src = ui.back + (size_t)y * (size_t)ui.w + r->x;
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

/* Asleep: black, and a faint point of light where Allen was, so a dark room
 * still shows the computer is on. */
static void dark(const ck_rect *r)
{
    for (int32_t y = r->y; y < r->y + r->h; y++)
        for (int32_t x = 0; x < r->w; x++)
            ui.back[(size_t)y * (size_t)ui.w + r->x + x] = 0;
    ck_glow(&ui.cv, r, ui.cx * 16 + 8, ui.cy * 16 + 8, S(6) * 16, CK_RGB(120, 130, 160), 40); /* radius in 1/16 px */
    present(r);
}

/* Dusk: the picture dims toward black while the faint mark of the dark room
 * comes up, so falling asleep and waking are one movement each. */
static void dusk(const ck_rect *r)
{
    uint32_t keep = 256 - (ui.dim >> 8);
    for (int32_t y = r->y; y < r->y + r->h; y++) {
        uint32_t *px = ui.back + (size_t)y * (size_t)ui.w + r->x;
        for (int32_t x = 0; x < r->w; x++)
            px[x] = ck_scale(px[x], keep);
    }
    ck_glow(&ui.cv, r, ui.cx * 16 + 8, ui.cy * 16 + 8, S(6) * 16, CK_RGB(120, 130, 160), 40 * ui.dim >> 16);
}

static void compose(const ck_rect *r)
{
    uint32_t t = ui.t;
    if (ui.asleep && ui.dim == DIM_FULL) {
        dark(r);
        return;
    }
    ck_night_base(&ui.night, &ui.cv, r);
    ck_night_lights(&ui.night, &ui.cv, r, ui.shimmer, ui.level * ui.shimmer >> 9);
    ck_flow_draw(&ui.flow, &ui.cv, r, 256);
    int32_t rad;
    uint32_t a;
    if (flash(t, &rad, &a)) {
        ck_glow(&ui.cv, r, ui.cx * 16 + 8, ui.cy * 16 + 8, rad, CK_RGB(200, 210, 255), a / 3);
        ck_glow(&ui.cv, r, ui.cx * 16 + 8, ui.cy * 16 + 8, rad / 3, CK_RGB(255, 240, 224), a);
        unflash(r, t);
    }
    draw_map(r, t);
    ck_mind_draw(&ui.mind, &ui.cv, r);
    draw_guest(r, t);
    draw_words(&ui.now, r, t);
    draw_panel(r, t);
    if (!choices_up())
        draw_bar(r, t);
    if (ui.dim)
        dusk(r);
    present(r);
}

/* ---- the tiles that changed ---- */

static void mark(ck_rect r)
{
    ck_rect all = {0, 0, ui.w, ui.h};
    if (r.w <= 0 || r.h <= 0 || !ck_clip(&r, &all))
        return;
    for (int32_t ty = r.y / TH; ty <= (r.y + r.h - 1) / TH; ty++)
        for (int32_t tx = r.x / TW; tx <= (r.x + r.w - 1) / TW; tx++)
            ui.tiles[ty * ui.tx + tx] = 1;
}

static void mark_all(void)
{
    for (int32_t i = 0; i < ui.tx * ui.ty; i++)
        ui.tiles[i] = 1;
}

/* the tiles the ripple's band crosses, between radius lo and hi */
static void mark_ring(int32_t lo, int32_t hi)
{
    if (hi <= 0)
        return;
    lo = ck_max(lo, 0);
    int64_t lo2 = (int64_t)lo * lo, hi2 = (int64_t)hi * hi;
    for (int32_t ty = 0; ty < ui.ty; ty++)
        for (int32_t tx = 0; tx < ui.tx; tx++) {
            int32_t x0 = tx * TW, y0 = ty * TH, x1 = x0 + TW, y1 = y0 + TH;
            int64_t nx = ck_clamp(ui.cx, x0, x1) - ui.cx, ny = ck_clamp(ui.cy, y0, y1) - ui.cy;
            int64_t fx = ck_max(ck_abs(x0 - ui.cx), ck_abs(x1 - ui.cx)),
                    fy = ck_max(ck_abs(y0 - ui.cy), ck_abs(y1 - ui.cy));
            if (nx * nx + ny * ny <= hi2 && fx * fx + fy * fy >= lo2)
                ui.tiles[ty * ui.tx + tx] = 1;
        }
}

static unsigned strips(void)
{
    unsigned n = 0;
    for (int32_t ty = 0; ty < ui.ty; ty++) {
        uint8_t *row = ui.tiles + ty * ui.tx;
        int32_t runs = 0, first = -1, last = -1;
        for (int32_t tx = 0; tx < ui.tx; tx++)
            if (row[tx]) {
                if (first < 0)
                    first = tx;
                if (tx == 0 || !row[tx - 1])
                    runs++;
                last = tx;
            }
        if (first < 0)
            continue;
        int32_t y = ty * TH, h = ck_min(TH, ui.h - y);
        if (runs > 6 || n + (unsigned)runs > JOBS_MAX) { /* one strip for the row */
            int32_t x = first * TW;
            ui.jobs[n++] = (ck_rect){x, y, ck_min((last + 1) * TW, ui.w) - x, h};
        } else
            for (int32_t tx = 0; tx < ui.tx; tx++) {
                if (!row[tx] || (tx > 0 && row[tx - 1]))
                    continue;
                int32_t end = tx;
                while (end + 1 < ui.tx && row[end + 1])
                    end++;
                ui.jobs[n++] = (ck_rect){tx * TW, y, ck_min((end + 1) * TW, ui.w) - tx * TW, h};
            }
        for (int32_t tx = 0; tx < ui.tx; tx++)
            row[tx] = 0;
        if (n >= JOBS_MAX)
            break;
    }
    return n;
}

int ck_allen_ui_help(void)
{
    for (;;) {
        uint64_t v = __atomic_load_n(&ui.ticket, __ATOMIC_ACQUIRE);
        uint32_t next = (uint32_t)(v & 0xffffu), count = (uint32_t)(v >> 16 & 0xffffu);
        if (next >= count)
            return 0;
        if (__atomic_compare_exchange_n(&ui.ticket, &v, v + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            compose(&ui.jobs[next]);
            __atomic_fetch_add(&ui.done, 1, __ATOMIC_RELEASE);
            return 1;
        }
    }
}

/* ---- the frame ---- */

/* what the keyboard side left, taken under the lock */
static void take_notes(uint32_t t)
{
    lock();
    if (note.refresh)
        note.refresh = 0, mark_all();
    if (note.wake_gen != ui.seen_wake) {
        ui.seen_wake = note.wake_gen;
        ui.footer = note.footer;
        if (!ui.on)
            ui.on = 1, ui.wake_t = t, ui.quiet_t = ck_max((int32_t)t, (int32_t)FORMED_MS);
    }
    if (note.sleep_gen != ui.seen_sleep) {
        ui.seen_sleep = note.sleep_gen;
        ui.asleep = note.sleep;
        mark_all(); /* the whole screen changes either way */
    }
    if (note.input_gen != ui.seen_input) {
        ui.seen_input = note.input_gen;
        if (note.input_len > ui.input_len)
            ck_mind_key(&ui.mind, t), ck_flow_key(&ui.flow, t);
        ui.input_len = note.input_len;
        for (unsigned i = 0; i < ui.input_len; i++)
            ui.input[i] = note.input[i];
        ui.key_t = t, ui.quiet_t = t, ui.input_moved = 1;
    }
    if (note.said_gen != ui.seen_said) {
        ui.seen_said = note.said_gen;
        if (note.has_line) {
            ui.guest_len = copy(ui.guest, note.line, TEXT_MAX);
            ui.guest_x = ui.bar.x + S(30);
            ui.guest_t = t;
        }
        /* what is on screen fades; Allen thinks, then speaks. With no reply
         * yet he keeps thinking (the water turns) until it comes. */
        if (ui.now.len && !ui.now.fade)
            ui.now.fade = t;
        ui.next = (struct words){0};
        if (note.think) {
            ui.speak_t = 0;
            ui.thinking = 1;
        } else {
            ui.next.len = copy(ui.next.text, note.reply, TEXT_MAX);
            uint32_t think = ui.thinking ? 400u : note.has_line ? 900u + ck_min((int32_t)ui.next.len * 6, 700) : 500u;
            ui.speak_t = ck_max((int32_t)(t + think), (int32_t)FORMED_MS + 300);
            ui.thinking = 0;
        }
        ui.input_len = 0;
        if (note.input_gen != note.said_input_gen) { /* typed after the words came, in the same frame */
            ui.input_len = note.input_len;
            for (unsigned i = 0; i < ui.input_len; i++)
                ui.input[i] = note.input[i];
        }
        ui.input_moved = 1;
        ui.quiet_t = t;
        ck_mind_mode(&ui.mind, CK_MIND_THINK, t);
    }
    if (note.panel_gen != ui.seen_panel) {
        ui.seen_panel = note.panel_gen;
        const struct panel *p = &note.panel;
        uint32_t at = t + 300; /* after the words have started */
        for (unsigned i = 0; i < p->nline; i++) {
            int kept = ui.pan.on && i < ui.pan.nline;
            for (unsigned k = 0; kept && k < PL_MAX; k++) {
                if (ui.pan.line[i][k] != p->line[i][k])
                    kept = 0;
                if (!p->line[i][k])
                    break;
            }
            if (kept)
                at = ui.pl_t[i];
            else
                ui.pl_t[i] = at = ck_max((int32_t)at, (int32_t)t + (i ? 0 : 300));
            at += PANEL_STEP_MS;
        }
        if (p->lit != ui.pan.lit || !ui.pan.on)
            ui.lit_was = ui.pan.on ? ui.pan.lit : -1, ui.lit_t = t;
        if (ui.pan.on && !p->on && ui.now.len && !ui.now.fade)
            ui.now.fade = t; /* the panel goes, and the words held with it go too */
        ui.pan = *p;
        ui.quiet_t = t;
        mark(ui.pan_box);
        mark(ui.row_box);
    }
    if (note.map_gen != ui.seen_map) {
        ui.seen_map = note.map_gen;
        ui.n = note.n, ui.ne = note.ne, ui.total = note.total, ui.links = note.links, ui.lit = note.lit;
        for (unsigned i = 0; i < ui.ne; i++)
            ui.ea[i] = note.ea[i], ui.eb[i] = note.eb[i];
        ui.label_len = copy(ui.label, note.label, TEXT_MAX);
        if (ui.lit >= 0 && ui.n)
            ui.map_t = t ? t : 1;
        mark(ui.map_box);
    }
    unlock();
}

static uint32_t toward(uint32_t cur, uint32_t want, uint32_t dt, uint32_t full_ms)
{
    uint32_t step = (uint32_t)((uint64_t)256 * dt / full_ms);
    step = step || !dt ? step : 1;
    if (cur < want)
        return cur + step < want ? cur + step : want;
    return cur > want + step ? cur - step : want;
}

/* moves everything to t and marks what changed */
static void advance(uint32_t t)
{
    uint32_t prev = ui.last, dt = t - prev;
    take_notes(t);
    /* dusk and dawn: the whole picture dims or returns over a moment, everything still moving */
    uint32_t dim_was = ui.dim;
    uint32_t dim_step = (uint32_t)((uint64_t)DIM_FULL * dt / (ui.asleep ? 1400u : 900u)); /* toward() moves in 256ths */
    ui.dim = ui.asleep ? (ui.dim + dim_step < DIM_FULL ? ui.dim + dim_step : DIM_FULL) : (ui.dim > dim_step ? ui.dim - dim_step : 0);
    if (ui.dim != dim_was || (ui.dim && ui.dim < DIM_FULL))
        mark_all();
    if (ui.asleep && ui.dim == DIM_FULL)
        return; /* nothing moves while Allen sleeps; the tiles marked on the way in are drawn once */
    ck_flow_mode(&ui.flow,
                 ui.asleep || !ui.on || t < FORMED_MS ? CK_FLOW_SLEEP
                 : ui.mind.mode == CK_MIND_THINK      ? CK_FLOW_THINK
                 : ui.mind.mode == CK_MIND_SPEAK      ? CK_FLOW_SPEAK
                 : ui.mind.mode == CK_MIND_LISTEN     ? CK_FLOW_LISTEN
                                                      : CK_FLOW_IDLE,
                 t);
    /* the sky */
    ck_night_at(&ui.night, t);
    if (t < CK_NIGHT_FADE_END_MS + FRAME_MS * 2)
        mark_all();
    mark(ui.lens_was);
    mark(ui.night.lens);
    ui.lens_was = ui.night.lens;
    if (ui.night.ripple_w || ui.ripple_was >= 0) {
        int32_t reach = ui.night.ripple_w * 22 / 10 + 2;
        if (ui.ripple_was >= 0)
            mark_ring(ui.ripple_was - reach - 8, ui.ripple_was + reach + 8);
        if (ui.night.ripple_w)
            mark_ring(ui.night.ripple_r - reach - 8, ui.night.ripple_r + reach + 8);
        ui.ripple_was = ui.night.ripple_w ? ui.night.ripple_r : -1;
    }
    ck_rect fb = flash_box(t);
    mark(ui.flash_was);
    mark(fb);
    ui.flash_was = fb;
    for (unsigned i = ui.twinkle_turn % 3; i < ui.night.ntw; i += 3)
        mark(ck_night_star_box(&ui.night, i));
    ui.twinkle_turn++;

    /* the conversation: speaking starts, words reach the mind and the galaxies */
    if (ui.speak_t && t >= ui.speak_t) {
        ui.now = ui.next;
        ui.now.from = ui.speak_t;
        lay_out(&ui.now);
        ui.speak_t = 0;
        ck_mind_mode(&ui.mind, CK_MIND_SPEAK, t);
    }
    struct words *w = &ui.now;
    if (w->len && t >= w->from && prev < w->done) {
        for (unsigned i = 0; i < w->len; i++) {
            uint32_t at = w->from + i * LETTER_MS;
            if (at > prev && at <= t && w->text[i] != ' ' && (i == 0 || w->text[i - 1] == ' '))
                ck_mind_word(&ui.mind, t), ck_flow_word(&ui.flow, t), ui.word_t = t;
        }
    }
    uint32_t last_letter = w->len ? w->from + w->len * LETTER_MS : 0;
    int speaking = w->len && t >= w->from && t < last_letter + 300;
    ui.level = speaking ? ck_exp_neg((t - ui.word_t) * 256 / 200) >> 8 : 0;
    ck_mind_voice(&ui.mind, ui.level);
    if (!speaking && !ui.speak_t && (ui.mind.mode == CK_MIND_SPEAK || ui.mind.mode == CK_MIND_THINK) &&
        w->len && t >= w->from) {
        ck_mind_mode(&ui.mind, CK_MIND_IDLE, t);
        ui.quiet_t = t;
    }
    if (ui.mind.mode != CK_MIND_THINK && ui.mind.mode != CK_MIND_SPEAK)
        ck_mind_mode(&ui.mind, ui.input_len && t - ui.key_t < 4000 ? CK_MIND_LISTEN : CK_MIND_IDLE, t);
    uint32_t rise = w->len ? ck_smooth(ck_prog(t, w->from, w->from + 600)) : 0;
    uint32_t fall = w->len ? 256 - ck_smooth(ck_prog(t, last_letter + 300, last_letter + 2800)) : 0;
    ui.shimmer = rise < fall ? rise : fall;
    if (ui.shimmer || ui.shimmer_was)
        for (unsigned i = 0; i < CK_NIGHT_GALAXIES; i++)
            mark(ui.night.gal[i].box);
    ui.shimmer_was = ui.shimmer;
    if (w->len && (t < w->done + 40 || words_alpha(w, t) != words_alpha(w, prev)))
        mark(w->box);
    if (ui.words_was.w && (ui.words_was.x != w->box.x || ui.words_was.w != w->box.w || ui.words_was.h != w->box.h))
        mark(ui.words_was);
    ui.words_was = w->box;
    if (w->len && !words_alpha(w, t) && t > w->from)
        *w = (struct words){0};

    /* the water, and Allen floating on it: he sways a little with the current under him */
    ck_flow_at(&ui.flow, t);
    ck_flow_mark(&ui.flow, mark);
    int32_t vx, vy;
    ck_flow_velocity(&ui.flow, ui.cx, ui.cy, &vx, &vy);
    ui.mind.cx = ui.cx + ck_clamp(vx * S(10) / 1000, -S(10), S(10));
    ui.mind.cy = ui.cy + ck_clamp(vy * S(10) / 1000, -S(10), S(10));
    /* Allen */
    ck_mind_at(&ui.mind, t);
    mark(ck_mind_dirty(&ui.mind));
    if (ui.guest_t) {
        ck_rect g = guest_area();
        mark(g);
        ui.guest_box = g;
        if (t - ui.guest_t > 2400)
            ui.guest_t = 0;
    }

    /* the setup wizard's panel: in and out, its lines easing in, the lit choice moving */
    ui.pan_a = toward(ui.pan_a, ui.pan.on ? 256 : 0, dt, ui.pan.on ? 600 : 900);
    if (ui.pan_a != ui.pan_was) {
        mark(ui.pan_box);
        mark(ui.row_box);
    }
    ui.pan_was = ui.pan_a;
    for (unsigned i = 0; ui.pan_a && i < ui.pan.nline; i++)
        if (t > ui.pl_t[i] && prev < ui.pl_t[i] + 560) {
            mark(ui.pan_box);
            break;
        }
    if (ui.pan_a && t - ui.lit_t < 260)
        mark(ui.row_box);

    /* the bar: up once Allen has formed and while in use; down when things rest */
    int want = ui.on && t >= FORMED_MS && !choices_up() &&
               (ui.pan.on ? ui.pan.prompt[0] != 0 : ui.input_len || t - ui.quiet_t < BAR_REST_MS || speaking || ui.speak_t);
    ui.bar_a = toward(ui.bar_a, want ? 256 : 0, dt, want ? (t - ui.key_t < 300 ? 250 : 900) : 1400);
    if (ui.bar_a || ui.bar_was || ui.input_moved)
        mark(ui.bar_box);
    ui.bar_was = ui.bar_a;
    ui.input_moved = 0;

    /* the notes: shown a while after a note is saved or handed back */
    int show = ui.map_t && t - ui.map_t < MAP_SHOW_MS && ui.n;
    ui.map_a = toward(ui.map_a, show ? 256 : 0, dt, show ? 700 : 1500);
    if (ui.map_a || ui.map_was)
        mark(ui.map_box);
    ui.map_was = ui.map_a;
    if (ui.lit >= 0 && ui.map_t && t - ui.map_t < 1200) /* the spark's path, Allen to the star */
        mark(ck_union(ck_mind_dirty(&ui.mind), ui.map_box));
}

int ck_allen_ui_tick(uint64_t now)
{
    if (!ui.started)
        return 0;
    uint32_t t = ms(now);
    if (ui.frames && t - ui.last < FRAME_MS && !__atomic_load_n(&note.refresh, __ATOMIC_ACQUIRE))
        return 0;
    uint64_t c0 = ui.clock ? ui.clock() : 0;
    ui.t = t;
    advance(t);
    unsigned n = strips();
    ui.done = 0;
    __atomic_store_n(&ui.ticket, (uint64_t)n << 16, __ATOMIC_RELEASE);
    while (ck_allen_ui_help())
        ;
    while (__atomic_load_n(&ui.done, __ATOMIC_ACQUIRE) < n)
        relax();
    ui.last = t;
    ui.frames++;
    if (ui.clock) {
        uint64_t c = ui.clock() - c0;
        uint32_t us = (uint32_t)(c * 1000000u / ui.hz);
        ui.total_ticks += us;
        if (us > ui.slowest)
            ui.slowest = us;
    }
    return 1;
}

void ck_allen_ui_stats(uint32_t *frames, uint32_t *slowest_us, uint32_t *average_us)
{
    *frames = ui.frames;
    *slowest_us = ui.slowest;
    *average_us = ui.frames ? ui.total_ticks / ui.frames : 0;
}

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

#define CAP_VOICE 19
#define CAP_BODY 15
#define CAP_SMALL 11

size_t ck_allen_ui_mem_bytes(uint32_t w, uint32_t h)
{
    uint32_t k = scale_for(w, h);
    return (size_t)w * h * 4 + ck_night_mem_bytes(w, h, k) + face_bytes(CAP_VOICE * k / 256) +
           face_bytes(CAP_BODY * k / 256) + face_bytes(CAP_SMALL * k / 256) + 64;
}

int ck_allen_ui_started(void) { return ui.started; }
int ck_allen_ui_on(void) { return ui.on; }

int ck_allen_ui_start(volatile uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch, uint32_t format, void *mem,
                      size_t mem_len, uint64_t hz, uint64_t now, uint64_t (*clock)(void))
{
    if (!fb || w < 640 || h < 400 || pitch < w || format > 1 || hz == 0 || !mem ||
        mem_len < ck_allen_ui_mem_bytes(w, h) || ((w + TW - 1) / TW) * ((h + TH - 1) / TH) > TILES_MAX)
        return -1;
    char hint[sizeof ui.hint];
    for (unsigned i = 0; i < sizeof hint; i++)
        hint[i] = ui.hint[i];
    ui = (__typeof__(ui)){0};
    ck_allen_ui_named(hint[9] ? hint + 9 : "Allen"); /* keeps a name given before the window opened */
    ui.fb = fb;
    ui.w = (int32_t)w, ui.h = (int32_t)h;
    ui.pitch = pitch, ui.format = format, ui.hz = hz, ui.t0 = now, ui.clock = clock;
    ui.k = scale_for(w, h);
    ui.tx = (int32_t)((w + TW - 1) / TW), ui.ty = (int32_t)((h + TH - 1) / TH);
    uint8_t *p = mem;
    ui.back = (uint32_t *)p;
    p += (size_t)w * h * 4;
    ui.cv = (ck_canvas){ui.back, ui.w, ui.h};
    ui.cx = ui.w / 2, ui.cy = ui.h * 40 / 100;
    size_t nb = ck_night_mem_bytes(w, h, ui.k);
    if (ck_night_init(&ui.night, w, h, ui.k, ui.cx, ui.cy, p, nb, ui.back))
        return -1;
    p += nb;
    uint32_t caps[3] = {CAP_VOICE * ui.k / 256, CAP_BODY * ui.k / 256, CAP_SMALL * ui.k / 256};
    ck_af_face *faces[3] = {&ui.voice, &ui.body, &ui.small};
    unsigned weights[3] = {1000, 1150, 1250};
    for (int i = 0; i < 3; i++) {
        size_t n = face_bytes(caps[i]);
        ck_af_face_init(faces[i], caps[i], weights[i], p, n);
        p += n;
        /* every letter now, so drawing never writes to the face (other cores read it) */
        for (int c = CK_AF_FIRST; c <= CK_AF_LAST; c++)
            (void)ck_af_glyph_get(faces[i], (char)c);
    }
    ck_mind_init(&ui.mind, ui.cx, ui.cy, ui.k, BORN_MS);
    /* layout (design px on 1920 x 1080, centred) */
    int32_t bw = S(720), bh = S(54);
    ui.bar = (ck_rect){(ui.w - bw) / 2, ui.h - S(150), bw, bh};
    ui.bar_box = (ck_rect){ui.bar.x - S(20), ui.bar.y - S(20), bw + S(40), bh + S(80)};
    ui.bar_box.x = ck_min(ui.bar_box.x, ui.w / 2 - S(360));
    ui.bar_box.w = ck_max(ui.bar_box.w, S(720));
    ui.mx = ui.w * 84 / 100, ui.my = ui.h * 42 / 100, ui.mr = S(130);
    ui.map_box = (ck_rect){ui.mx - S(190), ui.my - ui.mr - S(64), S(380), 2 * ui.mr + S(150)};
    ui.pan_box = (ck_rect){ui.w / 2 - S(640), pan_line_y(0) - S(34), S(1280), (int32_t)CK_ALLEN_UI_PANEL_LINES * S(31) + S(30)};
    ui.row_box = (ck_rect){ui.w / 2 - S(900), ui.bar.y - S(24), S(1800), ui.h - ui.bar.y + S(24)};
    ui.lit_was = -1;
    ui.lit = -1;
    ui.ripple_was = -1;
    /* the typed line's ink leaves from just above the bar, so the hint under it stays clear */
    ck_flow_init(&ui.flow, ui.w, ui.h, ui.cx, ui.cy, ui.mind.R, ui.bar.x + ui.bar.w / 2, ui.bar.y - ui.bar.h / 2, 0);
    ui.seen_input = note.input_gen, ui.seen_said = note.said_gen, ui.seen_wake = note.wake_gen;
    ui.seen_map = note.map_gen - 1; /* the map kept from before is taken by the first frame */
    ui.seen_panel = note.panel_gen - 1;
    ui.started = 1;
    ui.ticket = 0;
    return 0;
}

void ck_allen_ui_refresh(void)
{
    lock();
    note.refresh = 1;
    unlock();
}

void ck_allen_ui_sleep(int asleep, uint64_t now)
{
    (void)now;
    lock();
    note.sleep = asleep != 0;
    note.sleep_gen++;
    unlock();
}
int ck_allen_ui_asleep(void) { return ui.asleep && ui.dim == DIM_FULL; }

void ck_allen_ui_wake(const char *footer, uint64_t now)
{
    (void)now;
    lock();
    note.footer = footer;
    note.wake_gen++;
    unlock();
}

void ck_allen_ui_input(const char *buf, unsigned len, uint64_t now)
{
    (void)now;
    if (len > TEXT_MAX - 1)
        len = TEXT_MAX - 1;
    lock();
    for (unsigned i = 0; i < len; i++)
        note.input[i] = buf[i];
    note.input_len = len;
    note.input_gen++;
    unlock();
}

void ck_allen_ui_said(const char *line, const char *reply, uint64_t now)
{
    (void)now;
    lock();
    note.has_line = line != 0;
    note.think = reply == 0;
    copy(note.line, line, TEXT_MAX);
    copy(note.reply, reply ? reply : "", TEXT_MAX);
    note.said_input_gen = note.input_gen;
    note.said_gen++;
    unlock();
}

void ck_allen_ui_think(const char *line, uint64_t now) { ck_allen_ui_said(line, 0, now); }

void ck_allen_ui_map(unsigned n, const uint8_t *ea, const uint8_t *eb, unsigned ne, int lit, const char *label,
                     unsigned total, unsigned links)
{
    lock();
    note.n = n < CK_ALLEN_UI_MAP_NODES ? n : CK_ALLEN_UI_MAP_NODES;
    note.ne = 0;
    for (unsigned i = 0; i < ne && note.ne < CK_ALLEN_UI_MAP_LINKS; i++)
        if (ea[i] < note.n && eb[i] < note.n && ea[i] != eb[i]) {
            note.ea[note.ne] = ea[i];
            note.eb[note.ne++] = eb[i];
        }
    note.lit = lit >= 0 && (unsigned)lit < note.n ? lit : -1;
    copy(note.label, label, TEXT_MAX);
    note.total = total;
    note.links = links;
    note.map_gen++;
    unlock();
}

void ck_allen_ui_named(const char *name)
{
    static const char lead[] = "Speak to ";
    unsigned n = 0;
    for (; lead[n]; n++)
        ui.hint[n] = lead[n];
    for (unsigned i = 0; name && name[i] && n + 1 < sizeof ui.hint; i++)
        ui.hint[n++] = name[i];
    ui.hint[n] = 0;
    ui.hint_len = n;
    if (ui.started)
        mark(ui.bar);
}

void ck_allen_ui_panel(const ck_allen_ui_panel_view *p, uint64_t now)
{
    (void)now;
    lock();
    struct panel *d = &note.panel;
    d->on = p != 0;
    d->nline = d->nchoice = 0;
    d->lit = 0;
    d->hint[0] = d->aside[0] = d->prompt[0] = 0;
    if (p) {
        for (unsigned i = 0; i < p->nline && i < CK_ALLEN_UI_PANEL_LINES; i++)
            copy(d->line[d->nline++], p->line[i], PL_MAX);
        for (unsigned i = 0; i < p->nchoice && i < CK_ALLEN_UI_CHOICES; i++)
            copy(d->choice[d->nchoice++], p->choice[i], PC_MAX);
        d->lit = p->lit >= 0 && (unsigned)p->lit < d->nchoice ? p->lit : 0;
        copy(d->hint, p->hint, PL_MAX);
        copy(d->aside, p->aside, sizeof d->aside);
        copy(d->prompt, p->prompt, sizeof d->prompt);
    }
    note.panel_gen++;
    unlock();
}
