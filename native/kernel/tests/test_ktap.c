/* test_ktap.c -- host tests for the KTAP recorder (core/ktap.c): the exact
 * document text, failure and skip lines, the empty document and the
 * capacity overflow. */
#include <string.h>

#include "ck_test.h"
#include "ktap.h"

static char doc[8192];
static size_t doc_len;

static void sink(void *ctx, const char *line)
{
    (void)ctx;
    size_t n = strlen(line);
    if (doc_len + n + 2 > sizeof doc)
        return;
    memcpy(doc + doc_len, line, n);
    doc_len += n;
    doc[doc_len++] = '\n';
    doc[doc_len] = 0;
}

static int emit(void)
{
    doc_len = 0;
    doc[0] = 0;
    return ck_ktap_emit(sink, 0);
}

int main(void)
{
    /* All pass, two suites, one skip. */
    ck_ktap_reset();
    ck_ktap_suite("mm");
    ck_ktap_check("mmu_enabled", 1, "unused");
    ck_ktap_check("guard_page", 1, 0);
    ck_ktap_suite("stages");
    ck_ktap_case("devices", CK_KTAP_SKIP, "not linked");
    CHECK(emit() == 0);
    CHECK(strcmp(doc, "KTAP version 1\n"
                      "1..2\n"
                      "  KTAP version 1\n"
                      "  # Subtest: mm\n"
                      "  1..2\n"
                      "  ok 1 mmu_enabled\n"
                      "  ok 2 guard_page\n"
                      "ok 1 mm\n"
                      "  KTAP version 1\n"
                      "  # Subtest: stages\n"
                      "  1..1\n"
                      "  ok 1 devices # SKIP not linked\n"
                      "ok 2 stages\n") == 0);

    /* A failing case fails its suite and the document, with its note as a
     * diagnostic line right before the result; other suites stay ok. */
    ck_ktap_reset();
    ck_ktap_suite("irq");
    ck_ktap_check("timer_ticks", 0, "fewer than five ticks");
    ck_ktap_check("timer_stats", 1, 0);
    ck_ktap_suite("entropy");
    ck_ktap_check("source_probe", 1, 0);
    CHECK(emit() == -1);
    CHECK(strstr(doc, "  # timer_ticks: fewer than five ticks\n  not ok 1 timer_ticks\n  ok 2 timer_stats\n"
                      "not ok 1 irq\n") != 0);
    CHECK(strstr(doc, "ok 2 entropy\n") != 0);

    /* Interleaved suites are not merged out of order: a case belongs to the
     * suite open when it was recorded. */
    ck_ktap_reset();
    ck_ktap_check("orphan", 1, 0); /* no suite yet: opens "boot" */
    CHECK(emit() == 0);
    CHECK(strstr(doc, "  # Subtest: boot\n  1..1\n  ok 1 orphan\nok 1 boot\n") != 0);

    /* Empty document: version and an empty plan, and it passes. */
    ck_ktap_reset();
    CHECK(emit() == 0);
    CHECK(strcmp(doc, "KTAP version 1\n1..0\n") == 0);

    /* Capacity: records past the limit are dropped, counted, and fail the
     * document through an extra top-level ERROR result in the plan. */
    ck_ktap_reset();
    ck_ktap_suite("big");
    for (int i = 0; i < CK_KTAP_MAX_CASES + 3; i++)
        ck_ktap_check("c", 1, 0);
    CHECK(emit() == -1);
    CHECK(strstr(doc, "KTAP version 1\n1..2\n") == doc);
    CHECK(strstr(doc, "not ok 2 ktap_capacity # ERROR 3 records dropped\n") != 0);
    ck_ktap_reset();
    for (int i = 0; i < CK_KTAP_MAX_SUITES + 1; i++)
        ck_ktap_suite("s");
    ck_ktap_check("late", 1, 0); /* belongs to the dropped suite: dropped */
    CHECK(emit() == -1);
    CHECK(strstr(doc, "not ok 9 ktap_capacity # ERROR 2 records dropped\n") != 0);

    return ck_t_verdict("test_ktap");
}
