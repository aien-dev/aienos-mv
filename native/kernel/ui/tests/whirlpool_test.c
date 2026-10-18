/* whirlpool_test.c -- host test for the startup picture (ui/whirlpool.c).
 * Puts a stand-in maker's logo (a ring with a bar, white on black, and dark
 * on grey) on a screen in RAM, lets the whirlpool take it down, checks the
 * pixels, and writes PNG pictures to the directory given as argv[1] (if any)
 * for a person to look at. Final line: WHIRLPOOL_TEST: PASS|FAIL (N checks). */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../whirlpool.h"

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

static uint32_t swap_rb(uint32_t c) { return (c & 0xff00u) | (c >> 16 & 0xffu) | (c & 0xffu) << 16; }

/* The stand-in logo: a ring with a bar through it, edges softened over 1.5 px
 * the way firmware pictures are; fg on bg, both 0x00RRGGBB. */
static void logo(uint32_t *px, uint32_t w, uint32_t h, int fmt, uint32_t fg, uint32_t bg)
{
    int32_t cx = (int32_t)w / 2, cy = (int32_t)h / 2, R = (int32_t)h / 14, t = R / 3;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            int32_t dx = ((int32_t)x - cx) * 4, dy = ((int32_t)y - cy) * 4; /* quarter pixels */
            int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
            int32_t d = 0;
            while ((int64_t)(d + 1) * (d + 1) <= d2)
                d++;
            int32_t ring = abs(d - 4 * R) - 2 * t, bar = (abs(dy) - t) > (abs(dx) - 4 * R) ? abs(dy) - t : abs(dx) - 4 * R;
            int32_t e = ring < bar ? ring : bar; /* < 0 inside, quarter pixels */
            uint32_t a = e <= -3 ? 256 : e >= 3 ? 0 : (uint32_t)(3 - e) * 256 / 6, c = 0;
            for (int s = 0; s <= 16; s += 8)
                c |= (((fg >> s & 255) * a + (bg >> s & 255) * (256 - a)) >> 8) << s;
            px[(size_t)y * w + x] = fmt ? c : swap_rb(c);
        }
}

/* pixels (as 0x00RRGGBB) more than `tol` from `ref` in some channel */
static size_t differ(const uint32_t *a, const uint32_t *b, size_t n, uint32_t tol)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        for (int s = 0; s <= 16; s += 8) {
            int32_t d = (int32_t)(a[i] >> s & 255) - (int32_t)(b[i] >> s & 255);
            if (d > (int32_t)tol || d < -(int32_t)tol) {
                k++;
                break;
            }
        }
    return k;
}

static size_t white(const uint32_t *px, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        k += (px[i] >> 16 & 255) > 200 && (px[i] >> 8 & 255) > 200 && (px[i] & 255) > 200;
    return k;
}

static size_t water(const uint32_t *px, size_t n) /* clearly blue, violet or teal */
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t r = px[i] >> 16 & 255, b = px[i] & 255;
        k += b > 90 && b > r + 30;
    }
    return k;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    char path[512];
    crc_init();
    uint32_t W = 1280, H = 800, nlogo, block;
    size_t n = (size_t)W * H, need = ck_whirl_mem_bytes(W, H);
    uint32_t *fb = malloc(n * 4), *orig = malloc(n * 4), *fb0 = malloc(n * 4);
    void *mem = malloc(need);

    CHECK(ck_whirl_start(0, W, H, W, 1, mem, need, &nlogo, &block) == -1, "no screen is refused");
    CHECK(ck_whirl_start(fb, W, H, W, 2, mem, need, &nlogo, &block) == -1, "an unknown pixel format is refused");
    CHECK(ck_whirl_start(fb, W, H, W, 1, mem, need - 4, &nlogo, &block) == -1, "too little memory is refused");
    CHECK(ck_whirl_start(fb, W, H, W - 1, 1, mem, need, &nlogo, &block) == -1, "a pitch under the width is refused");

    /* white on black, 1280 x 800, 0x00RRGGBB */
    logo(fb, W, H, 1, 0xffffff, 0);
    memcpy(orig, fb, n * 4);
    CHECK(ck_whirl_start(fb, W, H, W, 1, mem, need, &nlogo, &block) == 0, "1280x800 starts");
    CHECK(block == 1, "1280 across is drawn pixel by pixel");
    CHECK(nlogo > 1000, "the logo is found on the screen");
    CHECK(differ(fb, orig, n, 0) == 0, "starting does not draw");
    ck_whirl_frame(0);
    size_t d0 = differ(fb, orig, n, 48);
    printf("note: frame 0 differs from the firmware's screen in %zu of %u logo pixels\n", d0, nlogo);
    CHECK(d0 * 50 < nlogo, "frame 0 is the firmware's screen (under 2% of the logo's pixels off)");
    CHECK(water(fb, n) == 0, "no water yet at 0 ms");
    ck_whirl_frame(CK_WHIRL_LOGO_FROM_MS);
    size_t moved = 0;
    for (size_t i = 0; i < n; i++)
        moved += white(orig + i, 1) && !white(fb + i, 1);
    CHECK(moved * 50 < nlogo, "the logo has not moved at 300 ms");
    CHECK(differ(fb, orig, n, 8) > n / 4, "the water is coming in at 300 ms");
    ck_whirl_frame(1500);
    size_t w1500 = white(fb, n);
    CHECK(w1500 > 50 && w1500 < white(orig, n), "at 1.5 s part of the logo is still there, smaller");
    CHECK(water(fb, n) > n / 5, "the water is turning at 1.5 s");
    uint32_t gone = 0;
    for (uint32_t t = CK_WHIRL_LOGO_FROM_MS; t <= CK_WHIRL_MS && !gone; t += 20)
        if (!ck_whirl_frame(t))
            gone = t;
    printf("note: the logo is gone at %u ms\n", gone);
    CHECK(gone >= CK_WHIRL_MERGE_MS - 200 && gone <= CK_WHIRL_MERGE_MS, "the logo goes at the plunge, and the frame says when");
    ck_whirl_frame(CK_WHIRL_MS);
    CHECK(white(fb, n) == 0, "the logo is gone by the end");
    memcpy(fb0, fb, n * 4);
    ck_whirl_frame(CK_WHIRL_MS + 400);
    CHECK(differ(fb, fb0, n, 8) > n / 10, "the water keeps turning after the logo is gone");
    ck_whirl_frame(600000);
    CHECK(water(fb, n) > n / 5 && white(fb, n) == 0, "ten minutes on, still only water");

    if (dir) {
        static const uint32_t at[] = {0, 700, 1300, 1900, 2400, 2750, 2900, 3150, 3450, 5000};
        logo(fb, W, H, 1, 0xffffff, 0);
        ck_whirl_start(fb, W, H, W, 1, mem, need, &nlogo, &block);
        for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) {
            ck_whirl_frame(at[i]);
            snprintf(path, sizeof path, "%s/whirl-%04u.png", dir, at[i]);
            png(path, fb, W, 1, 0, 0, W, H, 1);
        }
    }

    /* the same screen in the other pixel order draws the same picture */
    uint32_t *fbs = malloc(n * 4);
    logo(fbs, W, H, 0, 0xffffff, 0);
    ck_whirl_start(fbs, W, H, W, 0, mem, need, &nlogo, &block);
    ck_whirl_frame(1500);
    logo(fb, W, H, 1, 0xffffff, 0);
    ck_whirl_start(fb, W, H, W, 1, mem, need, &nlogo, &block);
    ck_whirl_frame(1500);
    size_t same = 0;
    for (size_t i = 0; i < n; i++)
        same += swap_rb(fbs[i]) == fb[i];
    CHECK(same == n, "both pixel orders draw the same picture");

    /* a blank screen: no logo, only the whirlpool */
    for (size_t i = 0; i < n; i++)
        fb[i] = 0x101010;
    ck_whirl_start(fb, W, H, W, 1, mem, need, &nlogo, &block);
    CHECK(nlogo == 0, "a blank screen has no logo");
    ck_whirl_frame(0);
    size_t blank = 0;
    for (size_t i = 0; i < n; i++)
        blank += fb[i] == 0x101010;
    CHECK(blank == n, "a blank screen stays as it was at 0 ms");
    ck_whirl_frame(2000);
    CHECK(water(fb, n) > n / 5, "a blank screen still gets the whirlpool");

    /* dark grey on light grey, as some firmware draws it */
    logo(fb, W, H, 1, 0x303030, 0xbfbfbf);
    memcpy(orig, fb, n * 4);
    ck_whirl_start(fb, W, H, W, 1, mem, need, &nlogo, &block);
    CHECK(nlogo > 1000, "a dark logo on grey is found");
    ck_whirl_frame(0);
    CHECK(differ(fb, orig, n, 48) * 50 < nlogo, "frame 0 is the grey screen");
    ck_whirl_frame(1500);
    if (dir) {
        snprintf(path, sizeof path, "%s/whirl-grey-1500.png", dir);
        png(path, fb, W, 1, 0, 0, W, H, 1);
    }
    free(fbs);

    /* 3840 x 2160, the screen this was first seen on: blocks of 3, timed */
    W = 3840, H = 2160, n = (size_t)W * H, need = ck_whirl_mem_bytes(W, H);
    fb = realloc(fb, n * 4), orig = realloc(orig, n * 4), mem = realloc(mem, need);
    logo(fb, W, H, 0, 0xffffff, 0);
    memcpy(orig, fb, n * 4);
    CHECK(ck_whirl_start(fb, W, H, W, 0, mem, need, &nlogo, &block) == 0, "3840x2160 starts");
    CHECK(block == 3, "3840 across is drawn in blocks of 3");
    ck_whirl_frame(0);
    d0 = differ(fb, orig, n, 96);
    printf("note: 4K frame 0 differs in %zu of %u logo pixels (blocks of 3 at the edges)\n", d0, nlogo);
    CHECK(d0 * 4 < nlogo, "4K frame 0 is the firmware's screen apart from block edges");
    double t0 = now_ms();
    unsigned frames = 0;
    for (uint32_t t = 0; t <= CK_WHIRL_MS; t += 100, frames++)
        ck_whirl_frame(t);
    printf("note: 4K: %u frames in %.0f ms on this host (not the target machine)\n", frames, now_ms() - t0);
    for (size_t i = 0; i < n; i++)
        fb[i] = swap_rb(fb[i]);
    CHECK(white(fb, n) == 0, "4K: the logo is gone by the end");
    if (dir) {
        logo(fb, W, H, 0, 0xffffff, 0);
        ck_whirl_start(fb, W, H, W, 0, mem, need, &nlogo, &block);
        ck_whirl_frame(1800);
        snprintf(path, sizeof path, "%s/whirl-4k-1800.png", dir);
        png(path, fb, W, 0, 0, 0, W, H, 3);
        snprintf(path, sizeof path, "%s/whirl-4k-1800-detail.png", dir);
        png(path, fb, W, 0, W / 2 - 600, H / 2 - 400, 1200, 800, 1);
    }

    free(fb), free(orig), free(fb0), free(mem);
    printf("WHIRLPOOL_TEST: %s (%d checks)\n", fails ? "FAIL" : "PASS", checks);
    return fails != 0;
}
