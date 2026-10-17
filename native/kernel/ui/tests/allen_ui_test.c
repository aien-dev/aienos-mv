/* allen_ui_test.c -- host test for Allen's window (ui/allen_ui.c), its
 * typeface (ui/allen_font.c) and its first answers (svc/allen_reply.c).
 * Draws the window into RAM at the iMac's 3840 x 2160 (and at 1280 x 800 RGBX),
 * types "Hello Allen", lets Allen answer, checks the pixels, and writes PNG
 * pictures to the directory given as argv[1] (if any) for a person to look at.
 * Final line: ALLEN_UI_TEST: PASS|FAIL (N checks). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../svc/allen_reply.h"
#include "../allen_font.h"
#include "../allen_ui.h"

static int checks, fails;
#define CHECK(cond, what)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            fails++;                                                                                         \
            printf("FAIL  %s\n", what);                                                                      \
        }                                                                                                    \
    } while (0)

/* ---- a minimal PNG writer (stored deflate blocks), host only ---- */
static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ b[i]) & 255] ^ (c >> 8);
    return c;
}
static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}
static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t h[8];
    be32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n)
        fwrite(data, 1, n, f);
    uint32_t c = crc(0xffffffffu, (const uint8_t *)type, 4);
    c = crc(c, data, n) ^ 0xffffffffu;
    be32(h, c);
    fwrite(h, 1, 4, f);
}
/* pixels: 0x00RRGGBB (format 1) rows of `pitch`; region x,y,w,h; every `step`th pixel */
static void png(const char *path, const uint32_t *px, uint32_t pitch, int fmt, uint32_t x0, uint32_t y0, uint32_t w,
                uint32_t h, uint32_t step)
{
    uint32_t ow = w / step, oh = h / step;
    size_t raw_n = (size_t)oh * (1 + (size_t)ow * 3);
    uint8_t *raw = malloc(raw_n);
    size_t k = 0;
    for (uint32_t y = 0; y < oh; y++) {
        raw[k++] = 0;
        for (uint32_t x = 0; x < ow; x++) {
            uint32_t sr = 0, sg = 0, sb = 0;
            for (uint32_t j = 0; j < step; j++)
                for (uint32_t i = 0; i < step; i++) {
                    uint32_t c = px[(size_t)(y0 + y * step + j) * pitch + x0 + x * step + i];
                    sr += c >> 16 & 255, sg += c >> 8 & 255, sb += c & 255;
                }
            uint8_t r = (uint8_t)(sr / (step * step)), g = (uint8_t)(sg / (step * step)), b = (uint8_t)(sb / (step * step));
            if (fmt == 0) {
                uint8_t t = r;
                r = b;
                b = t;
            }
            raw[k++] = r, raw[k++] = g, raw[k++] = b;
        }
    }
    size_t blocks = raw_n / 65535 + 1;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(z_n);
    size_t o = 0;
    z[o++] = 0x78, z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n;) {
        size_t n = raw_n - i > 65535 ? 65535 : raw_n - i;
        z[o++] = i + n == raw_n;
        z[o++] = (uint8_t)n, z[o++] = (uint8_t)(n >> 8), z[o++] = (uint8_t)~n, z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + i, n);
        for (size_t j = 0; j < n; j++) {
            a = (a + raw[i + j]) % 65521;
            b = (b + a) % 65521;
        }
        o += n;
        i += n;
    }
    be32(z + o, b << 16 | a);
    o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("note: cannot write %s\n", path);
        free(raw), free(z);
        return;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, ow), be32(ihdr + 4, oh);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, o);
    chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw), free(z);
    printf("note: wrote %s (%ux%u)\n", path, ow, oh);
}

static unsigned count(const uint32_t *px, uint32_t w, uint32_t h, int fmt, int kind)
{
    unsigned n = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t c = px[i];
        int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
        if (fmt == 0) {
            int t = r;
            r = b;
            b = t;
        }
        if (kind == 0 && b > 200 && r < 170 && g < 150 && r > 60) /* the person's bubble */
            n++;
        if (kind == 1 && r > 215 && g > 215 && b > 215) /* bright text and the orb's highlight */
            n++;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    char path[512];
    crc_init();

    /* ---- answers ---- */
    CHECK(!strcmp(ck_allen_reply("Hello Allen"), "Hello Humanity."), "reply: Hello Allen");
    CHECK(!strcmp(ck_allen_reply("  hello   ALLEN!! "), "Hello Humanity."), "reply: case, spaces, punctuation folded");
    CHECK(!strcmp(ck_allen_reply("hello allen."), "Hello Humanity."), "reply: trailing period");
    CHECK(!strcmp(ck_allen_reply("hello allenx"), "I don't know that yet."), "reply: near miss is not a match");
    CHECK(!strcmp(ck_allen_reply(""), "I don't know that yet."), "reply: empty line");
    CHECK(!strcmp(ck_allen_reply(0), "I don't know that yet."), "reply: no line");
    CHECK(!strcmp(ck_allen_reply("who are you"), "I am Allen. I live on this iMac."), "reply: who are you");

    /* ---- typeface ---- */
    CHECK(ck_af_isqrt(0) == 0 && ck_af_isqrt(15) == 3 && ck_af_isqrt(16) == 4 &&
              ck_af_isqrt(1000000007ull * 1000000007ull) == 1000000007ull,
          "font: integer square root");
    CHECK(ck_af_sin(0) == 0 && ck_af_sin(90) == 16384 && ck_af_sin(-90) == -16384 && ck_af_cos(180) == -16384 &&
              ck_af_sin(450) == 16384,
          "font: sine table");
    static uint8_t pool[4 << 20];
    ck_af_face face;
    ck_af_face_init(&face, 30, 1250, pool, sizeof pool);
    int all = 1, sym = 1;
    for (int c = 33; c <= 126; c++) {
        const ck_af_glyph *g = ck_af_glyph_get(&face, (char)c);
        unsigned ink = 0;
        if (!g || !g->alpha) {
            all = 0;
            continue;
        }
        for (size_t i = 0; i < (size_t)g->w * g->h; i++)
            ink += g->alpha[i] > 128;
        if (ink < 4)
            all = 0;
    }
    const ck_af_glyph *o = ck_af_glyph_get(&face, 'o');
    for (int y = 0; o && y < o->h; y++)
        for (int x = 0; x < o->w; x++) {
            int d = (int)o->alpha[y * o->w + x] - (int)o->alpha[y * o->w + (o->w - 1 - x)];
            if (d > 40 || d < -40)
                sym = 0;
        }
    CHECK(all, "font: every printable character rasterizes with ink");
    CHECK(sym, "font: 'o' is left-right symmetric");
    CHECK(ck_af_glyph_get(&face, ' ') != 0 && ck_af_advance(&face, ' ') > 0, "font: space advances");
    CHECK(ck_af_measure(&face, "Hello Humanity.", 15) > ck_af_measure(&face, "Hello", 5), "font: measure grows");
    uint8_t tiny[64];
    ck_af_face small;
    ck_af_face_init(&small, 30, 1250, tiny, sizeof tiny);
    CHECK(ck_af_glyph_get(&small, 'W') == 0, "font: a full pool refuses instead of overrunning");

    /* ---- the window at 3840 x 2160, BGRX (the iMac) ---- */
    uint32_t W = 3840, H = 2160;
    uint32_t *fb = calloc((size_t)W * H, 4);
    size_t need = ck_allen_ui_mem_bytes(W, H);
    void *mem = malloc(need);
    CHECK(ck_allen_ui_start(fb, W, H, W, 1, mem, need - 1, 1000, 0, "x") == -1, "ui: short memory refused");
    CHECK(ck_allen_ui_start(fb, W, H, W, 2, mem, need, 1000, 0, "x") == -1, "ui: unknown pixel format refused");
    CHECK(ck_allen_ui_start(fb, W, H, W, 1, mem, need, 1000, 0, "This iMac restarts 60 s after the last key") == 0,
          "ui: starts at 3840x2160");
    CHECK(ck_allen_ui_on(), "ui: on");
    uint32_t corner = fb[0];
    CHECK((corner >> 16 & 255) < 60 && (corner & 255) < 90, "ui: background is dark");
    unsigned user0 = count(fb, W, H, 1, 0);
    unsigned bright0 = count(fb, W, H, 1, 1);
    CHECK(bright0 > 500, "ui: title and orb are drawn");
    if (dir) {
        snprintf(path, sizeof path, "%s/allen-start.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
    }
    const char *typed = "Hello Allen";
    uint64_t now = 1000;
    for (unsigned i = 1; i <= 11; i++, now += 120) {
        ck_allen_ui_input(typed, i, now);
        ck_allen_ui_tick(now);
    }
    unsigned bright_typed = count(fb, W, H, 1, 1);
    CHECK(bright_typed > bright0, "ui: typed letters appear in the input field");
    if (dir) {
        snprintf(path, sizeof path, "%s/allen-typing.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
    }
    ck_allen_ui_said(typed, ck_allen_reply(typed), now);
    CHECK(count(fb, W, H, 1, 0) > user0 + 2000, "ui: the person's bubble appears");
    unsigned before = count(fb, W, H, 1, 1);
    for (unsigned i = 0; i < 300; i++, now += 10)
        ck_allen_ui_tick(now);
    unsigned after = count(fb, W, H, 1, 1);
    CHECK(after > before, "ui: Allen's answer appears letter by letter");
    uint32_t snap_mid = fb[(size_t)(H / 2) * W + W / 2];
    for (unsigned i = 0; i < 60; i++, now += 33)
        ck_allen_ui_tick(now);
    CHECK(fb[(size_t)(H / 2) * W + W / 2] == snap_mid, "ui: the conversation stays put while the orb moves");
    if (dir) {
        snprintf(path, sizeof path, "%s/allen-answer.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 2);
        snprintf(path, sizeof path, "%s/allen-bg-detail.png", dir);
        png(path, fb, W, 1, 0, 0, 1600, 1000, 1);
        snprintf(path, sizeof path, "%s/allen-answer-detail.png", dir);
        png(path, fb, W, 1, 840, 1100, 2160, 820, 1);
    }

    /* ---- 1280 x 800 RGBX (QEMU's usual mode) ---- */
    uint32_t w2 = 1280, h2 = 800, pitch2 = 1344;
    uint32_t *fb2 = calloc((size_t)pitch2 * h2, 4);
    size_t need2 = ck_allen_ui_mem_bytes(w2, h2);
    void *mem2 = malloc(need2);
    CHECK(ck_allen_ui_start(fb2, w2, h2, pitch2, 0, mem2, need2, 1000, 0, "footer") == 0, "ui: starts at 1280x800");
    ck_allen_ui_said("Hello Allen", "Hello Humanity.", 10);
    for (unsigned i = 0; i < 300; i++)
        ck_allen_ui_tick(10 + i * 10);
    unsigned user2 = 0, padding_clean = 1;
    for (uint32_t y = 0; y < h2; y++)
        for (uint32_t x = 0; x < pitch2; x++) {
            uint32_t c = fb2[(size_t)y * pitch2 + x];
            if (x >= w2 && c)
                padding_clean = 0;
            if (x < w2 && (c >> 16 & 255) > 200 && (c & 255) < 170 && (c & 255) > 60 && (c >> 8 & 255) < 150)
                user2++; /* RGBX: blue lands in the top byte */
        }
    CHECK(user2 > 300, "ui: RGBX screens get red and blue swapped");
    CHECK(padding_clean, "ui: nothing written past the visible width");
    if (dir) {
        snprintf(path, sizeof path, "%s/allen-1280x800.png", dir);
        uint32_t *tmp = malloc((size_t)w2 * h2 * 4);
        for (uint32_t y = 0; y < h2; y++)
            memcpy(tmp + (size_t)y * w2, fb2 + (size_t)y * pitch2, w2 * 4);
        png(path, tmp, w2, 0, 0, 0, w2, h2, 1);
        free(tmp);
    }
    free(fb), free(mem), free(fb2), free(mem2);
    printf("ALLEN_UI_TEST: %s (%d checks%s)\n", fails ? "FAIL" : "PASS", checks, fails ? ", some failed" : "");
    return fails ? 1 : 0;
}
