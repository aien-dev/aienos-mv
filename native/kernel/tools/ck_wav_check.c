/* ck_wav_check.c -- host tool for the HDA QEMU gate (L207): reads the WAV file
 * QEMU's wav audio backend wrote while the guest kernel played its sine tone
 * and checks what actually reached the (emulated) codec output:
 *   ck_wav_check FILE EXPECT_HZ EXPECT_PEAK MIN_MS
 * 16-bit PCM only. The tone region is the longest run of frames whose
 * magnitude exceeds a noise floor; inside it the frequency is counted from
 * zero crossings (rising edges), the peak is the largest magnitude, and both
 * channels must match. Exit 0 and "WAV_CHECK: PASS ..." or exit 1 and
 * "WAV_CHECK: FAIL (reason)". Hosted tooling only; never part of an image. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int fail(const char *why)
{
    printf("WAV_CHECK: FAIL (%s)\n", why);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: %s FILE EXPECT_HZ EXPECT_PEAK MIN_MS\n", argv[0]); return 2; }
    double want_hz = atof(argv[2]);
    int want_peak = atoi(argv[3]);
    double min_ms = atof(argv[4]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) return fail("cannot open the wav file");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44) { fclose(f); return fail("file shorter than a WAV header"); }
    uint8_t *b = malloc((size_t)size);
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) { fclose(f); return fail("read error"); }
    fclose(f);
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return fail("not a RIFF/WAVE file");
    /* walk chunks; QEMU can leave sizes unpatched when it is stopped, so the data
     * chunk runs to the end of the file whenever its size is 0 or too big */
    long pos = 12, data = -1, dlen = 0;
    unsigned rate = 0, chans = 0, bits = 0, fmt = 0;
    while (pos + 8 <= size) {
        uint32_t n = le32(b + pos + 4);
        if (!memcmp(b + pos, "fmt ", 4) && pos + 8 + 16 <= size) {
            fmt = le16(b + pos + 8);
            chans = le16(b + pos + 10);
            rate = le32(b + pos + 12);
            bits = le16(b + pos + 22);
        } else if (!memcmp(b + pos, "data", 4)) {
            data = pos + 8;
            dlen = (n == 0 || (long)n > size - data) ? size - data : (long)n;
            break;
        }
        pos += 8 + (long)n + (n & 1);
    }
    if (data < 0) return fail("no data chunk");
    printf("WAV: format=%u channels=%u rate=%u bits=%u data_bytes=%ld\n", fmt, chans, rate, bits, dlen);
    if (fmt != 1 || bits != 16 || chans < 1 || chans > 2 || rate < 8000) return fail("not 16-bit PCM mono/stereo");
    long frames = dlen / (long)(2 * chans);
    const int16_t *s = (const int16_t *)(void *)(b + data);
    const int floor_ = 200;
    long best_start = 0, best_len = 0, start = -1;
    long quiet = 0;
    for (long i = 0; i <= frames; i++) {
        int loud = i < frames && abs(s[i * chans]) > floor_;
        if (loud) {
            if (start < 0) start = i;
            quiet = 0;
        } else if (start >= 0) {
            quiet++;
            /* a gap of 20 ms or more ends the run (the tone is continuous) */
            if (i == frames || quiet > (long)rate / 50) {
                long end = i - quiet;
                if (end - start > best_len) best_len = end - start, best_start = start;
                start = -1;
                quiet = 0;
            }
        }
    }
    double ms = 1000.0 * (double)best_len / rate;
    printf("WAV: tone_region_frames=%ld tone_ms=%.0f\n", best_len, ms);
    if (ms < min_ms) return fail("tone shorter than required");
    /* use the middle of the region: skip 100 ms at each end */
    long skip = (long)rate / 10, a = best_start + skip, e = best_start + best_len - skip;
    if (e - a < (long)rate / 4) return fail("tone region too short to measure");
    long rising = 0, first = -1, last = -1;
    int peak = 0, mismatch = 0;
    for (long i = a + 1; i < e; i++) {
        int p = s[(i - 1) * chans], c = s[i * chans];
        if (p < 0 && c >= 0) {
            rising++;
            if (first < 0) first = i;
            last = i;
        }
        if (abs(c) > peak) peak = abs(c);
        if (chans == 2 && s[i * 2] != s[i * 2 + 1]) mismatch++;
    }
    if (rising < 10) return fail("no oscillation measured");
    double hz = (double)(rising - 1) * rate / (double)(last - first);
    printf("WAV: freq_hz=%.2f peak=%d channel_mismatch=%d\n", hz, peak, mismatch);
    if (hz < want_hz * 0.99 || hz > want_hz * 1.01) return fail("frequency off");
    if (peak < want_peak * 0.9 || peak > want_peak * 1.1) return fail("amplitude off");
    if (mismatch) return fail("channels differ");
    printf("WAV_CHECK: PASS (%.2f Hz, peak %d, %.0f ms)\n", hz, peak, ms);
    return 0;
}
