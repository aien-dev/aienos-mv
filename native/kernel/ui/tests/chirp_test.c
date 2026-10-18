/* chirp_test.c -- host test of the startup sound (ui/chirp.c). Writes the
 * whole sound as chirp.wav into the folder given, for a person to listen to.
 * Prints CHIRP_TEST: PASS (n checks) or the first failure. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../chirp.h"

static int checks, failed;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok && !failed) {
        failed = 1;
        printf("CHIRP_TEST: FAIL %s\n", what);
    }
}

static uint32_t ms(uint32_t t) { return CK_CHIRP_RATE / 1000u * t; }

/* the pitch heard over [a, b): rising zero crossings over the time */
static double heard_hz(const int16_t *s, uint32_t a, uint32_t b)
{
    uint32_t first = 0, last = 0, n = 0;
    for (uint32_t f = a + 1; f < b; f++)
        if (s[2 * (f - 1)] < 0 && s[2 * f] >= 0) {
            if (!n)
                first = f;
            last = f;
            n++;
        }
    return n < 2 ? 0 : (double)(n - 1) * CK_CHIRP_RATE / (last - first);
}

static void put32(FILE *f, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        fputc(v >> 8 * i & 255, f);
}

static void put16(FILE *f, uint32_t v)
{
    fputc(v & 255, f);
    fputc(v >> 8 & 255, f);
}

static int write_wav(const char *path, const int16_t *s, uint32_t frames)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    uint32_t bytes = frames * 4;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1); /* PCM */
    put16(f, 2);
    put32(f, CK_CHIRP_RATE);
    put32(f, CK_CHIRP_RATE * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, bytes);
    for (uint32_t i = 0; i < 2 * frames; i++)
        put16(f, (uint16_t)s[i]);
    return fclose(f);
}

int main(int argc, char **argv)
{
    const uint32_t N = CK_CHIRP_FRAMES, extra = ms(200);
    int16_t *s = calloc(2 * (size_t)(N + extra), sizeof *s), *p = calloc(2 * (size_t)(N + extra), sizeof *p);
    if (!s || !p)
        return 2;
    ck_chirp_fill(s, 0, N + extra);

    /* silence while the stars come out, and after the end */
    int quiet = 1;
    for (uint32_t f = 0; f < ms(CK_NIGHT_HOLES_MS); f++)
        quiet &= s[2 * f] == 0 && s[2 * f + 1] == 0;
    check(quiet, "silent before the black holes appear");
    quiet = 1;
    for (uint32_t f = N; f < N + extra; f++)
        quiet &= s[2 * f] == 0;
    check(quiet, "silent after the end");
    check(ck_chirp_level(N - 1) < 2, "faded out by the end");

    /* both channels the same, nothing past the peak */
    int same = 1, top = 0;
    for (uint32_t f = 0; f < N; f++) {
        same &= s[2 * f] == s[2 * f + 1];
        int v = abs(s[2 * f]);
        top = v > top ? v : top;
    }
    check(same, "left and right the same");
    check(top <= CK_CHIRP_PEAK && top > CK_CHIRP_PEAK * 98 / 100, "loudest sample at the peak level");

    /* no jump big enough to click: a sine at TO_HZ moves at most 2 pi f / rate of its height a frame */
    int jump = 0;
    for (uint32_t f = 1; f < N + extra; f++) {
        int d = abs(s[2 * f] - s[2 * (f - 1)]);
        jump = d > jump ? d : jump;
    }
    printf("note: largest step between frames %d (a %u Hz sine at the peak: %d)\n", jump, CK_CHIRP_TO_HZ,
           (int)(6.2832 * CK_CHIRP_TO_HZ / CK_CHIRP_RATE * CK_CHIRP_PEAK));
    check(jump <= (int)(6.2832 * CK_CHIRP_TO_HZ / CK_CHIRP_RATE * CK_CHIRP_PEAK) + 64, "no clicks");

    /* loudness: comes in, then only rises to the merger, then falls */
    uint32_t a = ck_chirp_level(ms(CK_NIGHT_HOLES_MS)), b = ck_chirp_level(ms(CK_NIGHT_SPIRAL_MS) - 1);
    check(a < 4 && b > 1000, "the tone comes in as the holes appear");
    check(ck_chirp_level(ms(CK_NIGHT_SPIRAL_MS)) - b < 8, "no step in loudness when the spiral starts");
    int up = 1, down = 1;
    for (uint32_t f = ms(CK_NIGHT_HOLES_MS) + 1; f < ms(CK_NIGHT_MERGE_MS); f++)
        up &= ck_chirp_level(f) >= ck_chirp_level(f - 1);
    for (uint32_t f = ms(CK_NIGHT_MERGE_MS) + 1; f < N; f++)
        down &= ck_chirp_level(f) <= ck_chirp_level(f - 1);
    check(up, "louder all the way to the merger");
    check(down, "quieter all the way after it");
    check(ck_chirp_level(ms(CK_NIGHT_MERGE_MS) - 1) == CK_CHIRP_PEAK, "the peak at the merger");
    check(ck_chirp_level(ms(CK_NIGHT_MERGE_MS) + ms(60)) < CK_CHIRP_PEAK * 40 / 100, "the ring fades in tens of ms");

    /* pitch: steady, then only rises, reaching TO_HZ in the last moments */
    check(ck_chirp_hz(ms(4000)) == CK_CHIRP_FROM_HZ && ck_chirp_hz(ms(CK_NIGHT_SPIRAL_MS)) == CK_CHIRP_FROM_HZ,
          "steady tone while the holes circle");
    up = 1;
    uint32_t reach = 0;
    for (uint32_t f = ms(CK_NIGHT_SPIRAL_MS) + 1; f < ms(CK_NIGHT_MERGE_MS); f++) {
        up &= ck_chirp_hz(f) >= ck_chirp_hz(f - 1);
        if (!reach && ck_chirp_hz(f) == CK_CHIRP_TO_HZ)
            reach = f;
    }
    check(up, "pitch only rises in the spiral");
    printf("note: pitch at 10000 ms %u Hz, 11800 ms %u Hz; the top reached %u ms before the merger\n",
           ck_chirp_hz(ms(10000)), ck_chirp_hz(ms(11800)), (ms(CK_NIGHT_MERGE_MS) - reach) * 1000 / CK_CHIRP_RATE);
    check(reach && ms(CK_NIGHT_MERGE_MS) - reach < ms(60), "the top pitch only in the last 60 ms");

    /* what is heard matches: zero crossings */
    double h0 = heard_hz(s, ms(4000), ms(5000)), h1 = heard_hz(s, ms(9950), ms(10050)),
           h2 = heard_hz(s, ms(CK_NIGHT_MERGE_MS), ms(CK_NIGHT_MERGE_MS) + ms(100));
    printf("note: heard %.1f Hz while the holes circle, %.1f Hz at 10000 ms (asked %u), %.1f Hz in the ring\n", h0, h1,
           ck_chirp_hz(ms(10000)), h2);
    check(h0 > CK_CHIRP_FROM_HZ - 1.0 && h0 < CK_CHIRP_FROM_HZ + 1.0, "heard pitch while the holes circle");
    check(h1 > ck_chirp_hz(ms(9950)) - 2.0 && h1 < ck_chirp_hz(ms(10050)) + 2.0, "heard pitch in the spiral");
    check(h2 > CK_CHIRP_TO_HZ - 5.0 && h2 < CK_CHIRP_TO_HZ + 5.0, "heard pitch in the ring");

    /* any frame in any order: backwards in odd pieces gives the same sound */
    for (uint32_t end = N + extra; end;) {
        uint32_t n = end < 997 ? end : 997;
        end -= n;
        ck_chirp_fill(p + 2 * (size_t)end, end, n);
    }
    check(!memcmp(s, p, 4 * (size_t)(N + extra)), "pieces in any order give the same sound");

    if (argc > 1) {
        char path[4096];
        snprintf(path, sizeof path, "%s/chirp.wav", argv[1]);
        check(write_wav(path, s, N) == 0, "wav written");
        printf("note: wrote %s (%u frames, %u ms)\n", path, N, N * 1000 / CK_CHIRP_RATE);
    }
    free(s);
    free(p);
    if (failed)
        return 1;
    printf("CHIRP_TEST: PASS (%d checks)\n", checks);
    return 0;
}
