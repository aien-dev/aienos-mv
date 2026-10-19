/* test_osc_resgen.c -- host test for the per-resource incarnation counter (artifact/osc_resgen.c, ADR 0019,
 * aienos#285) and for its use by the kernel's own admission policy (core/osc_admit.c, qualification build with the
 * CK_RESGEN_TEST self-test compiled in). Checks every counter rule (0 never issued, first holder 1, +1 per reuse,
 * retired at UINT32_MAX, floor written ahead, untrusted fails closed), reboot and rollback restores (never lower),
 * and the admission verdicts of vector a02 (domain 1, object 1, pin 7): match, mismatch, reused number, reboot,
 * rollback. The kernel's table as booted must still refuse every pin (CAP_GENERATION_STALE 29). ck_printf is
 * replaced by a buffer so the self-test lines can be checked. Keys are the spec's THROWAWAY TEST keys. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ck_test.h"
#include "osc_admit.h"
#include "osc_resgen.h"
#include "osc_unit.h"

#ifndef OSC_FIXTURE_DIR
#define OSC_FIXTURE_DIR "native/kernel/tests/fixtures/osc_unit"
#endif

static char out[16384];
static size_t outn;

void ck_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + outn, sizeof out - outn, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof out - outn) outn += (size_t)n;
}

int ck_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static uint8_t *slurp(const char *vec, size_t *n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/vectors/%s", OSC_FIXTURE_DIR, vec);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(l > 0 ? (size_t)l : 1);
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = 0; }
    fclose(f);
    *n = b ? (size_t)l : 0;
    return b;
}

/* RAM floor: what a durable floor write would leave behind. fail_next makes the next write fail. */
static uint8_t floor_rec[OSC_RESGEN_REC_MAX];
static size_t floor_len;
static unsigned floor_writes;
static int fail_next;

static int floor_write(void *ctx, const uint8_t *rec, size_t len)
{
    (void)ctx;
    if (fail_next) { fail_next = 0; return 1; }
    memcpy(floor_rec, rec, len);
    floor_len = len;
    floor_writes++;
    return 0;
}

static uint64_t look(struct osc_resgen *t, unsigned kind, uint32_t id)
{
    uint64_t c = 0;
    return osc_resgen_lookup(t, 1, kind, id, &c) == 0 ? c : 0; /* 0: no such resource (0 is never issued) */
}

static struct osc_resgen t;

static void counter_rules(void)
{
    uint32_t g = 99;
    uint64_t c = 0;
    /* a fresh table knows no history: untrusted, nothing issued, nothing found */
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_create(&t, 3, 5, 0, &g) == OSC_RESGEN_UNTRUSTED && g == 0);
    CHECK(osc_resgen_lookup(&t, 1, 3, 5, &c) != 0);
    CHECK(osc_resgen_destroy(&t, 3, 5) == OSC_RESGEN_UNTRUSTED);
    /* genesis writes an empty floor first */
    CHECK(osc_resgen_genesis(&t) == OSC_RESGEN_OK && t.trusted == 1 && floor_writes == 1 && floor_len == 48);
    /* domain-1 kinds only */
    CHECK(osc_resgen_create(&t, 1, 5, 0, &g) == OSC_RESGEN_BAD_KIND);
    CHECK(osc_resgen_create(&t, 4, 5, 0, &g) == OSC_RESGEN_BAD_KIND);
    /* first holder at 1 (0 never issued), busy while live, +1 per reuse */
    CHECK(osc_resgen_create(&t, 3, 5, 0, &g) == OSC_RESGEN_OK && g == 1 && look(&t, 3, 5) == 1);
    CHECK(floor_writes == 2);
    CHECK(osc_resgen_create(&t, 3, 5, 0, &g) == OSC_RESGEN_BUSY && g == 0 && look(&t, 3, 5) == 1);
    CHECK(osc_resgen_destroy(&t, 3, 5) == OSC_RESGEN_OK && look(&t, 3, 5) == 0);
    CHECK(osc_resgen_destroy(&t, 3, 5) == OSC_RESGEN_NOT_LIVE);
    CHECK(osc_resgen_destroy(&t, 3, 77) == OSC_RESGEN_NOT_LIVE);
    CHECK(osc_resgen_create(&t, 3, 5, 0, &g) == OSC_RESGEN_OK && g == 2 && look(&t, 3, 5) == 2);
    /* channel 5 and object 5 are different numbers */
    CHECK(look(&t, 2, 5) == 0);
    CHECK(osc_resgen_create(&t, 2, 5, 0, &g) == OSC_RESGEN_OK && g == 1 && look(&t, 2, 5) == 1 && look(&t, 3, 5) == 2);
    /* only domain 1 is answered */
    CHECK(osc_resgen_lookup(&t, 2, 3, 5, &c) != 0 && osc_resgen_lookup(&t, 0, 3, 5, &c) != 0);
    CHECK(osc_resgen_lookup(&t, 1, 1, 5, &c) != 0);
    CHECK(osc_resgen_lookup(0, 1, 3, 5, &c) != 0);
    /* never wraps: the holder at UINT32_MAX is the last; the number is retired when it goes */
    CHECK(osc_resgen_destroy(&t, 3, 5) == OSC_RESGEN_OK);
    t.ent[0].gen = UINT32_MAX - 1u; /* white box: as if the number had been reused that often */
    CHECK(osc_resgen_create(&t, 3, 5, 0, &g) == OSC_RESGEN_OK && g == UINT32_MAX && look(&t, 3, 5) == UINT32_MAX);
    CHECK(osc_resgen_destroy(&t, 3, 5) == OSC_RESGEN_OK);
    CHECK(osc_resgen_create(&t, 3, 5, 0, &g) == OSC_RESGEN_RETIRED && g == 0 && look(&t, 3, 5) == 0);
    CHECK(t.ent[0].gen == UINT32_MAX);
    /* a failed floor write issues nothing and leaves the table untrusted */
    unsigned n0 = t.n;
    fail_next = 1;
    CHECK(osc_resgen_create(&t, 3, 6, 0, &g) == OSC_RESGEN_FLOOR_IO && g == 0 && t.n == n0 && t.trusted == 0);
    CHECK(look(&t, 2, 5) == 0);
    CHECK(osc_resgen_create(&t, 3, 6, 0, &g) == OSC_RESGEN_UNTRUSTED);
    /* reuse after a failed floor write does not advance the counter either */
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_genesis(&t) == OSC_RESGEN_OK);
    CHECK(osc_resgen_create(&t, 3, 9, 0, &g) == OSC_RESGEN_OK && g == 1 && osc_resgen_destroy(&t, 3, 9) == 0);
    fail_next = 1;
    CHECK(osc_resgen_create(&t, 3, 9, 0, &g) == OSC_RESGEN_FLOOR_IO && t.ent[0].gen == 1 && t.ent[0].flags == 0);
    /* no floor writer: genesis fails, nothing can ever be issued */
    osc_resgen_init(&t, 0, 0);
    CHECK(osc_resgen_genesis(&t) == OSC_RESGEN_FLOOR_IO && t.trusted == 0);
    /* table full */
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_genesis(&t) == OSC_RESGEN_OK);
    int ok = 1;
    for (uint32_t i = 0; i < OSC_RESGEN_MAX; i++)
        ok &= osc_resgen_create(&t, 2, 100 + i, 0, &g) == OSC_RESGEN_OK && g == 1;
    CHECK(ok);
    CHECK(osc_resgen_create(&t, 2, 999, 0, &g) == OSC_RESGEN_FULL);
    CHECK(osc_resgen_destroy(&t, 2, 100) == OSC_RESGEN_OK);
    CHECK(osc_resgen_create(&t, 2, 100, 0, &g) == OSC_RESGEN_OK && g == 2); /* a tracked number still reuses */
}

static void reboot_and_rollback(void)
{
    static uint8_t snap_old[OSC_RESGEN_REC_MAX], snap_new[OSC_RESGEN_REC_MAX], floor_old[OSC_RESGEN_REC_MAX],
        bad[OSC_RESGEN_REC_MAX];
    size_t old_len, new_len, floor_old_len;
    uint32_t g = 0;
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_genesis(&t) == OSC_RESGEN_OK);
    /* object 1 persistent, reused to 3; channel 4 not persistent, at 2 */
    for (int i = 0; i < 3; i++) {
        if (i) osc_resgen_destroy(&t, 3, 1);
        CHECK(osc_resgen_create(&t, 3, 1, 1, &g) == OSC_RESGEN_OK && g == (uint32_t)i + 1);
    }
    CHECK(osc_resgen_create(&t, 2, 4, 0, &g) == OSC_RESGEN_OK && osc_resgen_destroy(&t, 2, 4) == 0);
    CHECK(osc_resgen_create(&t, 2, 4, 0, &g) == OSC_RESGEN_OK && g == 2);
    old_len = osc_resgen_encode(&t, OSC_RESGEN_ROLE_SNAPSHOT, snap_old, sizeof snap_old);
    CHECK(old_len == 16 + 2 * 12 + 32);
    memcpy(floor_old, floor_rec, floor_len);
    floor_old_len = floor_len;
    CHECK(osc_resgen_encode(&t, OSC_RESGEN_ROLE_SNAPSHOT, snap_old, 10) == 0); /* too small: nothing written */
    /* the floor carries counters only, never live flags */
    CHECK(floor_len == 16 + 2 * 12 + 32 && floor_rec[16 + 2] == 0 && floor_rec[16 + 3] == 0);

    /* reboot: the persistent object is the same incarnation; the channel is gone, its number is free at 2 */
    osc_resgen_init(&t, floor_write, 0);
    CHECK(look(&t, 3, 1) == 0); /* before the restore the booted table answers nothing */
    CHECK(osc_resgen_restore(&t, snap_old, old_len, floor_rec, floor_len) == OSC_RESGEN_OK && t.trusted == 1);
    CHECK(look(&t, 3, 1) == 3);
    CHECK(look(&t, 2, 4) == 0);
    CHECK(osc_resgen_create(&t, 2, 4, 0, &g) == OSC_RESGEN_OK && g == 3); /* not 1: the counter came back */

    /* the object is reused (4), then the store is rolled back to snap_old (object at 3) with the newer floor */
    CHECK(osc_resgen_destroy(&t, 3, 1) == 0 && osc_resgen_create(&t, 3, 1, 1, &g) == OSC_RESGEN_OK && g == 4);
    new_len = osc_resgen_encode(&t, OSC_RESGEN_ROLE_SNAPSHOT, snap_new, sizeof snap_new);
    unsigned w = floor_writes;
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_restore(&t, snap_old, old_len, floor_rec, floor_len) == OSC_RESGEN_OK && t.trusted == 1);
    CHECK(look(&t, 3, 1) == 5);          /* a new incarnation: neither the old 3 nor the newer 4 */
    CHECK(floor_writes == w + 1);        /* the re-issue was written ahead */
    CHECK(osc_resgen_create(&t, 2, 4, 0, &g) == OSC_RESGEN_OK && g == 4); /* channel: floor 3, next 4 */
    /* booting the newer snapshot now: its object (4) is below the floor (5), so it is re-issued again */
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_restore(&t, snap_new, new_len, floor_rec, floor_len) == OSC_RESGEN_OK && look(&t, 3, 1) == 6);

    /* the floor itself rolled back (behind the snapshot): nothing can be shown monotonic */
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_restore(&t, snap_new, new_len, floor_old, floor_old_len) == OSC_RESGEN_BAD_RECORD);
    CHECK(t.trusted == 0 && look(&t, 3, 1) == 0 && osc_resgen_create(&t, 3, 2, 0, &g) == OSC_RESGEN_UNTRUSTED);
    /* a snapshot number the floor never issued: same (genesis below rewrites the RAM floor; keep the real one) */
    static uint8_t floor_cur[OSC_RESGEN_REC_MAX];
    size_t floor_cur_len = floor_len;
    memcpy(floor_cur, floor_rec, floor_len);
    static struct osc_resgen u;
    osc_resgen_init(&u, floor_write, 0);
    CHECK(osc_resgen_genesis(&u) == OSC_RESGEN_OK);
    uint8_t empty_floor[OSC_RESGEN_REC_MAX];
    size_t ef = osc_resgen_encode(&u, OSC_RESGEN_ROLE_FLOOR, empty_floor, sizeof empty_floor);
    CHECK(osc_resgen_restore(&t, snap_old, old_len, empty_floor, ef) == OSC_RESGEN_BAD_RECORD && t.trusted == 0);

    /* corrupt, truncated, wrong-role and missing records fail closed */
    /* control: the uncorrupted pair restores (so each refusal below is the corruption, not the pair) */
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_restore(&t, snap_new, new_len, floor_cur, floor_cur_len) == OSC_RESGEN_OK && look(&t, 3, 1) == 7);
    memcpy(floor_cur, floor_rec, floor_len); /* that restore re-issued 7: its floor is the current one */
    floor_cur_len = floor_len;
    memcpy(bad, floor_cur, floor_cur_len);
    bad[16 + 8] ^= 1; /* a counter byte: only the checksum can tell */
    CHECK(osc_resgen_restore(&t, snap_new, new_len, bad, floor_cur_len) == OSC_RESGEN_BAD_RECORD && t.trusted == 0);
    memcpy(bad, snap_new, new_len);
    bad[16 + 2] ^= 2; /* the persistent flag of the object: only the checksum can tell */
    CHECK(osc_resgen_restore(&t, bad, new_len, floor_cur, floor_cur_len) == OSC_RESGEN_BAD_RECORD && t.trusted == 0);
    memcpy(bad, floor_cur, floor_cur_len);
    bad[20] ^= 1;
    CHECK(osc_resgen_restore(&t, snap_new, new_len, bad, floor_cur_len) == OSC_RESGEN_BAD_RECORD && t.trusted == 0);
    memcpy(bad, snap_new, new_len);
    bad[new_len - 1] ^= 0x80;
    CHECK(osc_resgen_restore(&t, bad, new_len, floor_cur, floor_cur_len) == OSC_RESGEN_BAD_RECORD && t.trusted == 0);
    CHECK(osc_resgen_restore(&t, snap_new, new_len - 1, floor_cur, floor_cur_len) == OSC_RESGEN_BAD_RECORD);
    CHECK(osc_resgen_restore(&t, snap_new, new_len, snap_new, new_len) == OSC_RESGEN_BAD_RECORD);
    CHECK(osc_resgen_restore(&t, floor_cur, floor_cur_len, floor_cur, floor_cur_len) == OSC_RESGEN_BAD_RECORD);
    CHECK(osc_resgen_restore(&t, 0, 0, floor_cur, floor_cur_len) == OSC_RESGEN_BAD_RECORD && t.trusted == 0);
    CHECK(osc_resgen_restore(&t, snap_new, new_len, 0, 0) == OSC_RESGEN_BAD_RECORD && t.trusted == 0);
    CHECK(look(&t, 3, 1) == 0);

    /* rollback onto a retired number: the old resource does not come back under it */
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_genesis(&t) == OSC_RESGEN_OK);
    CHECK(osc_resgen_create(&t, 3, 8, 1, &g) == OSC_RESGEN_OK && g == 1);
    old_len = osc_resgen_encode(&t, OSC_RESGEN_ROLE_SNAPSHOT, snap_old, sizeof snap_old);
    CHECK(osc_resgen_destroy(&t, 3, 8) == 0);
    t.ent[0].gen = UINT32_MAX - 1u;
    CHECK(osc_resgen_create(&t, 3, 8, 1, &g) == OSC_RESGEN_OK && g == UINT32_MAX);
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_restore(&t, snap_old, old_len, floor_rec, floor_len) == OSC_RESGEN_OK);
    CHECK(look(&t, 3, 8) == 0 && t.ent[0].gen == UINT32_MAX);
    CHECK(osc_resgen_create(&t, 3, 8, 1, &g) == OSC_RESGEN_RETIRED);

    /* a re-issue whose floor write fails leaves the table untrusted */
    osc_resgen_init(&t, floor_write, 0);
    CHECK(osc_resgen_genesis(&t) == OSC_RESGEN_OK);
    CHECK(osc_resgen_create(&t, 3, 2, 1, &g) == OSC_RESGEN_OK);
    old_len = osc_resgen_encode(&t, OSC_RESGEN_ROLE_SNAPSHOT, snap_old, sizeof snap_old);
    CHECK(osc_resgen_destroy(&t, 3, 2) == 0 && osc_resgen_create(&t, 3, 2, 1, &g) == OSC_RESGEN_OK && g == 2);
    osc_resgen_init(&t, floor_write, 0);
    fail_next = 1;
    CHECK(osc_resgen_restore(&t, snap_old, old_len, floor_rec, floor_len) == OSC_RESGEN_FLOOR_IO && t.trusted == 0);
    CHECK(look(&t, 3, 2) == 0);
}

static void kernel_admission(void)
{
    size_t n = 0, n18 = 0;
    uint8_t *a02 = slurp("a02_valid_caps_kernel_domain.unit", &n);
    uint8_t *r18 = slurp("r18_gen_not_representable.unit", &n18);
    CHECK(a02 != 0 && r18 != 0);
    if (!a02 || !r18) return;
    struct osc_policy p;
    ck_osc_policy(&p);
    CHECK(p.gen_lookup == osc_resgen_lookup && p.ctx == ck_osc_resgen());
    /* as booted the kernel table is untrusted: every nonzero pin stays STALE (behavior before aienos#285) */
    CHECK(ck_osc_resgen()->trusted == 0);
    CHECK(osc_unit_admit(a02, n, &p, 0) == OSC_CAP_GENERATION_STALE);
    CHECK(osc_unit_admit(r18, n18, &p, 0) == OSC_CAP_GEN_NOT_REPRESENTABLE); /* step order unchanged */

    /* the CK_RESGEN_TEST self-test the QEMU gate runs, through the same candidate path */
    outn = 0; out[0] = 0;
    CHECK(ck_osc_candidate("a02_valid_caps_kernel.unit", a02, n) == OSC_CAP_GENERATION_STALE);
    static const char *const want[] = {
        "osc_unit: a02_valid_caps_kernel.unit REFUSED code=29 name=CAP_GENERATION_STALE\n",
        "osc_resgen_selftest: unit=a02 domain=1 kind=3 id=1 pin=7 floor=ram;",
        "osc_resgen: booted_untrusted object=1 counter=none pin=7 -> REFUSED code=29 name=CAP_GENERATION_STALE;",
        "osc_resgen: genesis -> 0\n",
        "osc_resgen: mismatch_below object=1 counter=6 pin=7 -> REFUSED code=29 name=CAP_GENERATION_STALE;",
        "osc_resgen: match object=1 counter=7 pin=7 -> ACCEPT;",
        "osc_resgen: restore -> 0 trusted=1\n",
        "osc_resgen: reboot_same_incarnation object=1 counter=7 pin=7 -> ACCEPT;",
        "osc_resgen: reused_number object=1 counter=8 pin=7 -> REFUSED code=29 name=CAP_GENERATION_STALE;",
        "osc_resgen: restore -> 0 trusted=1\n",
        "osc_resgen: rollback_old_snapshot object=1 counter=9 pin=7 -> REFUSED code=29 name=CAP_GENERATION_STALE;",
        "osc_resgen: restore -> 8 trusted=0\n",
        "osc_resgen: corrupt_floor object=1 counter=none pin=7 -> REFUSED code=29 name=CAP_GENERATION_STALE;",
        "osc_resgen: reset_untrusted object=1 counter=none pin=7 -> REFUSED code=29 name=CAP_GENERATION_STALE;",
        "osc_resgen_floor_writes: 10\n",
    };
    const char *at = out;
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        const char *hit = strstr(at, want[i]);
        CHECK(hit != 0);
        if (!hit) { printf("  missing in order: %s\n", want[i]); continue; }
        at = hit + strlen(want[i]);
    }
    /* the self-test leaves the kernel table as booted */
    CHECK(ck_osc_resgen()->trusted == 0 && ck_osc_resgen()->n == 0);
    CHECK(osc_unit_admit(a02, n, &p, 0) == OSC_CAP_GENERATION_STALE);
    free(a02);
    free(r18);
}

int main(void)
{
    counter_rules();
    reboot_and_rollback();
    kernel_admission();
    return ck_t_verdict("test_osc_resgen");
}
