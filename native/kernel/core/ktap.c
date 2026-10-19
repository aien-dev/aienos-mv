/* ktap.c -- KTAP version 1 recorder for the in-kernel self tests. See
 * ktap.h for the document shape and the rules. */
#include "ktap.h"
#include "fmt.h"

struct ktap_case {
    const char *name;
    const char *note;
    unsigned char suite;
    unsigned char result;
};

static const char *suites[CK_KTAP_MAX_SUITES];
static unsigned nsuites;
static struct ktap_case cases[CK_KTAP_MAX_CASES];
static unsigned ncases;
static unsigned dropped;

void ck_ktap_reset(void)
{
    nsuites = ncases = dropped = 0;
}

void ck_ktap_suite(const char *name)
{
    if (nsuites >= CK_KTAP_MAX_SUITES) {
        dropped++;
        return;
    }
    suites[nsuites++] = name;
}

void ck_ktap_case(const char *name, enum ck_ktap_result r, const char *note)
{
    if (nsuites == 0)
        ck_ktap_suite("boot");
    /* A case for a suite that did not fit is dropped with it. */
    if (dropped || ncases >= CK_KTAP_MAX_CASES) {
        dropped++;
        return;
    }
    cases[ncases].name = name;
    cases[ncases].note = note;
    cases[ncases].suite = (unsigned char)(nsuites - 1);
    cases[ncases].result = (unsigned char)r;
    ncases++;
}

void ck_ktap_check(const char *name, int ok, const char *note)
{
    ck_ktap_case(name, ok ? CK_KTAP_OK : CK_KTAP_FAIL, ok ? 0 : note);
}

int ck_ktap_emit(ck_ktap_put_fn put, void *ctx)
{
    char line[160];
    int any_fail = 0;
    put(ctx, "KTAP version 1");
    ck_snprintf(line, sizeof line, "1..%u", nsuites + (dropped ? 1u : 0u));
    put(ctx, line);
    for (unsigned s = 0; s < nsuites; s++) {
        unsigned n = 0;
        int suite_fail = 0;
        for (unsigned i = 0; i < ncases; i++)
            n += cases[i].suite == s;
        put(ctx, "  KTAP version 1");
        ck_snprintf(line, sizeof line, "  # Subtest: %s", suites[s]);
        put(ctx, line);
        ck_snprintf(line, sizeof line, "  1..%u", n);
        put(ctx, line);
        unsigned k = 0;
        for (unsigned i = 0; i < ncases; i++) {
            const struct ktap_case *c = &cases[i];
            if (c->suite != s)
                continue;
            k++;
            if (c->result == CK_KTAP_SKIP) {
                ck_snprintf(line, sizeof line, "  ok %u %s # SKIP %s", k, c->name,
                            c->note ? c->note : "");
            } else if (c->result == CK_KTAP_FAIL) {
                suite_fail = 1;
                if (c->note) {
                    ck_snprintf(line, sizeof line, "  # %s: %s", c->name, c->note);
                    put(ctx, line);
                }
                ck_snprintf(line, sizeof line, "  not ok %u %s", k, c->name);
            } else {
                ck_snprintf(line, sizeof line, "  ok %u %s", k, c->name);
            }
            put(ctx, line);
        }
        any_fail |= suite_fail;
        ck_snprintf(line, sizeof line, "%s %u %s", suite_fail ? "not ok" : "ok", s + 1, suites[s]);
        put(ctx, line);
    }
    if (dropped) {
        any_fail = 1;
        ck_snprintf(line, sizeof line, "not ok %u ktap_capacity # ERROR %u records dropped", nsuites + 1,
                    dropped);
        put(ctx, line);
    }
    return any_fail ? -1 : 0;
}
