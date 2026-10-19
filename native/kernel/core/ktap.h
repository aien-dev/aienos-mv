/* ktap.h -- in-kernel test results in KTAP version 1 (the Linux kernel's
 * test result format, docs.kernel.org/dev-tools/ktap.html; idea borrowed
 * from KUnit, no code copied).
 *
 * The boot path records each self test as it runs (ck_ktap_suite, then
 * ck_ktap_case per case) and prints the whole document once, in the final
 * report (ck_ktap_emit). Printing at the end, not as the tests run, keeps
 * the plan lines exact (a suite's case count is known only after it ran)
 * and keeps the block in one piece on the serial console. A boot that
 * panics or faults first prints no block; scripts/ktap_summary.sh reports
 * that as FAIL (no block), so a crash is never read as a pass.
 *
 * Document shape: "KTAP version 1", "1..<suites>", then per suite a
 * two-space indented block (version line, "# Subtest: <suite>", plan, one
 * result line per case), then the suite's own result line, "not ok" when
 * any case failed. A skipped case prints "ok <n> <name> # SKIP <reason>".
 *
 * Pure: no I/O. Lines leave through the caller's put callback, so the host
 * test (tests/test_ktap.c) checks the exact text. Names and notes must be
 * string literals or otherwise live until ck_ktap_emit; they are stored as
 * pointers. Records past the fixed capacity are counted, not stored, and
 * force the document to fail (an overflowed plan cannot be trusted). */
#ifndef AIENOS_CK_KTAP_H
#define AIENOS_CK_KTAP_H

#include <stddef.h>

#define CK_KTAP_MAX_SUITES 8
#define CK_KTAP_MAX_CASES 48

enum ck_ktap_result { CK_KTAP_OK = 0, CK_KTAP_FAIL = 1, CK_KTAP_SKIP = 2 };

typedef void (*ck_ktap_put_fn)(void *ctx, const char *line);

/* Forget every record (host tests; the kernel starts zeroed). */
void ck_ktap_reset(void);
/* Open a new suite; later cases belong to it until the next call. */
void ck_ktap_suite(const char *name);
/* Record one case in the open suite. note may be NULL; for CK_KTAP_SKIP it
 * is the skip reason, for CK_KTAP_FAIL a "# <note>" diagnostic line printed
 * before the result. A case with no open suite opens suite "boot". */
void ck_ktap_case(const char *name, enum ck_ktap_result r, const char *note);
/* Shorthand: ok != 0 records CK_KTAP_OK, else CK_KTAP_FAIL with note. */
void ck_ktap_check(const char *name, int ok, const char *note);
/* Print the document line by line (no trailing newline in a line). Returns
 * 0 when every recorded case passed or was skipped, -1 otherwise. */
int ck_ktap_emit(ck_ktap_put_fn put, void *ctx);

#endif
