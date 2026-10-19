/* osh_test.c -- OSH-AIENOS-0 kernel self-test (TEST ONLY, qualification build with CK_OSH_TEST=1).
 *
 * Runs the Omega-native shell core (three compiled OSC units: lex_run, parse_run, expand_run) as admitted EL0 tasks
 * through the launch interface, over the embedded fixture scripts, with the SAME sequencing as omega's
 * tests/osh/osh_trace.c (which mirrors src/osh/host/osh_shell.c run_src / lex_parse / run_list), and prints the
 * same trace lines on the serial console:
 *     step <n> unit=<lex|parse|expand> in_len=<n> ret=<u64> ws_sha256=<hex of WS_CELLS*8 bytes>
 *     request <n> sha256=<hex of the request record cells>
 *     event <text>   (input_window_exceeded, internal_error, request_ncmds_out_of_range, fake_status_exhausted)
 *     event exit=<status>   last line of every fixture (2 after a refusal, 70 after a unit fault status)
 * Nothing is executed: on PIPELINE_READY the driver only records the request and takes the fixture's next fake
 * status (its .status sidecar, then 0); NEED_VAR is answered from a fixed table (HOME=/home/t, X="a b", EMPTY=""),
 * $? = the last fake status, $0 = "osh_trace", $1.. from the fixture's .args sidecar. No environment, no files, no exec.
 * One caller-owned workspace of 23 pages (11456 cells) lives across every call. Input goes through the read-only
 * input window (at most 4096 bytes). A list buffer that would exceed 4096 is refused by the driver exactly as
 * osh_trace.c does (event line), documented in the gate.
 *
 * Then five negative controls (lines "osh_neg"), see qemu_ck_osh_core_test.sh.
 * QEMU (aarch64 virt), TEST signer, not physical. */
#include "ck_internal.h"
#include "fmt.h"
#include "osc_task.h"
#include "osc_admit.h"
#include "sha256.h"
#include "../tests/fixtures/osh/osh_layout.h"
#include "osh_fixtures.h"
#ifdef CK_OSH_OP_TEST
#include "osh_op.h"
#include "task_osc.h"
#endif

#define LABEL "QEMU (aarch64 virt), TEST signer, not physical"
#define CKOSH_WS_CELLS 11456u
#define WS_PAGES 23u
#define INPUT_MAX 4096u
#define REQ_HDR_CELLS 8u
#define CMD_CELLS 164u

/* the constants the shell was built against (omega src/osh/osh_layout.h at the pinned commit) */
_Static_assert(OSH_WS_CELLS == CKOSH_WS_CELLS, "workspace cells differ from the pinned omega layout");
_Static_assert((uint64_t)CKOSH_WS_CELLS * 8 <= (uint64_t)WS_PAGES * 4096, "23 pages hold the workspace");
_Static_assert((uint64_t)CKOSH_WS_CELLS * 8 > (uint64_t)(WS_PAGES - 1) * 4096, "22 pages do not");
_Static_assert(OSH_LINE_CAP == INPUT_MAX, "input window equals the lexer line cap");
_Static_assert(OSH_REQ_CMD_CELLS == CMD_CELLS, "request command record size");

enum { ST_MORE = 100, ST_BUDGET = 101, ST_NEED_VAR = 102, ST_PIPE = 103, ST_COMPLETE = 104 };
enum { U_LEX = 0, U_PARSE = 1, U_EXPAND = 2, U_COUNT = 3 };
static const char *const u_file[U_COUNT] = { "osh_lex.unit", "osh_parse.unit", "osh_expand.unit" };
static const char *const u_entry[U_COUNT] = { "lex_run", "parse_run", "expand_run" };
static const char *const u_label[U_COUNT] = { "lex", "parse", "expand" };
/* each unit's realization receipt in the Store (aienos#383, tests/fixtures/osc_receipt; minted by omega checker2) */
static const char *const u_rcpt[U_COUNT] = { "osh_lex.rcpt", "osh_parse.rcpt", "osh_expand.rcpt" };

/* ck_osc_launch with the unit's receipt from the Store (none if it did not arrive: refused RECEIPT_MISSING) */
static void launch_rcpt(const char *rname, const uint8_t *b, size_t len, const struct osc_accept *acc,
                        const struct ck_osc_launch_req *rq, struct osc_result *r, struct ck_osc_launch_info *info)
{
    size_t rl = 0;
    const uint8_t *rb = ck_osc_receipt_find(rname, &rl);
    ck_osc_launch(b, len, acc, rb, rl, rq, r, info);
}

#define UNIT_BYTES 131072u /* the packed containers (IR + code + manifest) pass 64 KiB; the code stays under CK_OSC_CODE_MAX_BYTES */
static struct {
    uint8_t bytes[UNIT_BYTES];
    size_t len;
    int have;
    struct osc_accept acc;
} unit[U_COUNT];

static int streq(const char *a, const char *b)
{
    size_t n = strlen(a);
    return n == strlen(b) && memcmp(a, b, n) == 0;
}

/* the launch gate's hand-assembled spin() unit, used only by control 4b */
static struct {
    uint8_t bytes[16384];
    size_t len;
    int have;
    struct osc_accept acc;
} spin_unit;

#ifdef CK_OSH_OP_TEST
/* aienos-mv#430: the task gate's three admitted stage units (tests/fixtures/task), the adapter's native operations */
enum { SU_GEN = 0, SU_XFORM = 1, SU_SINK = 2, SU_COUNT = 3 };
static const char *const su_file[SU_COUNT] = { "tgen.unit", "txform.unit", "tsink.unit" };
static const char *const su_rcpt[SU_COUNT] = { "tgen.rcpt", "txform.rcpt", "tsink.rcpt" };
static const char *const su_entry[SU_COUNT] = { "gen_step", "xform_step", "sink_step" };
#define STAGE_UNIT_MAX 8192u
static struct {
    uint8_t bytes[STAGE_UNIT_MAX];
    size_t len;
    int have;
    struct osc_accept acc;
} sunit[SU_COUNT];
#endif

void ck_osh_save(const char *name, const uint8_t *b, size_t len, const struct osc_accept *acc)
{
#ifdef CK_OSH_OP_TEST
    for (unsigned u = 0; u < SU_COUNT; u++)
        if (streq(name, su_file[u]) && len <= STAGE_UNIT_MAX) {
            memcpy(sunit[u].bytes, b, len);
            sunit[u].len = len;
            sunit[u].acc = *acc;
            sunit[u].have = 1;
        }
#endif
    if (streq(name, "l01_launch_fns.unit") && len <= sizeof spin_unit.bytes) {
        memcpy(spin_unit.bytes, b, len);
        spin_unit.len = len;
        spin_unit.acc = *acc;
        spin_unit.have = 1;
        return;
    }
    for (unsigned u = 0; u < U_COUNT; u++) {
        if (!streq(name, u_file[u]))
            continue;
        if (len > UNIT_BYTES) {
            ck_printf("osh_test: %s is %u bytes, over the %u byte test buffer; not saved\n", name, (unsigned)len, UNIT_BYTES);
            return;
        }
        memcpy(unit[u].bytes, b, len);
        unit[u].len = len;
        unit[u].acc = *acc;
        unit[u].have = 1;
    }
}

static uint8_t ws_mem[WS_PAGES * 4096] __attribute__((aligned(4096)));
static uint8_t in_buf[INPUT_MAX];
static size_t blen;
static unsigned long step_no, req_no;
static const unsigned char *src_text;
static unsigned long src_len, src_pos;
static int stop; /* a launch did not RETURN: the script is abandoned and the gate fails the diff */

static uint64_t *const w = (uint64_t *)ws_mem;

static void hex(const uint8_t *p, size_t n, char *o)
{
    static const char d[] = "0123456789abcdef";
    uint8_t dg[SHA256_DIGEST_SIZE];
    sha256_hash(p, n, dg);
    for (unsigned i = 0; i < SHA256_DIGEST_SIZE; i++) {
        o[2 * i] = d[dg[i] >> 4];
        o[2 * i + 1] = d[dg[i] & 15];
    }
    o[64] = 0;
}

static unsigned long launches, slots_held; /* every launch is one EL0 task; a task slot not free after its call is a leak */

/* One entry call through the launch interface. 0 = RETURNED (value in *ret), else the result is printed and 1. */
static int launch(unsigned u, size_t in_len, uint64_t ws_cells, struct ck_osc_ws *wsr, uint64_t max_ticks, struct osc_result *r,
                  struct ck_osc_launch_info *info)
{
    struct ck_osc_launch_req rq;
    memset(&rq, 0, sizeof rq);
    rq.name = u_entry[u];
    rq.name_len = strlen(u_entry[u]);
    rq.nargs = 4;
    rq.args[0] = ck_osc_va_in();
    rq.args[1] = in_len;
    rq.args[2] = ck_osc_va_ws();
    rq.args[3] = ws_cells;
    rq.in = in_buf;
    rq.in_len = in_len;
    rq.ws = wsr;
    rq.max_ticks = max_ticks;
    launch_rcpt(u_rcpt[u], unit[u].bytes, unit[u].len, &unit[u].acc, &rq, r, info);
    launches++;
    if (!info->slot_free_after)
        slots_held++;
    return r->cls != OSC_RES_RETURNED;
}

static struct ck_osc_ws main_ws = { ws_mem, WS_PAGES };

static uint64_t call(unsigned u)
{
    struct osc_result r;
    struct ck_osc_launch_info info;
    char h[65];
    if (launch(u, blen, CKOSH_WS_CELLS, &main_ws, 0, &r, &info)) {
        char rs[96];
        ck_osc_result_str(&r, rs, sizeof rs);
        ck_printf("osh_fault: %s step %lu -> %s; %s\n", u_label[u], step_no + 1, rs, LABEL);
        stop = 1;
        return ~(uint64_t)0;
    }
    hex(ws_mem, (size_t)CKOSH_WS_CELLS * 8, h);
    ck_printf("step %lu unit=%s in_len=%u ret=%llu ws_sha256=%s\n", ++step_no, u_label[u], (unsigned)blen,
              (unsigned long long)r.value, h);
    return r.value;
}

static void reset_list(void)
{
    memset(ws_mem, 0, sizeof ws_mem);
    w[OSH_S_MAGIC] = OSH_ABI_MAGIC;
    w[OSH_S_VERSION] = OSH_ABI_VERSION;
    w[OSH_S_WS_CELLS] = OSH_WS_CELLS;
    blen = 0;
}

static uint64_t run_entry(unsigned u)
{
    for (int i = 0; i < 100000; i++) {
        uint64_t st = call(u);
        if (stop || st != ST_BUDGET)
            return st;
    }
    return ~(uint64_t)0;
}

static uint64_t lex_parse(void)
{
    uint64_t st = run_entry(U_LEX);
    if (st != 0 || stop)
        return st;
    return run_entry(U_PARSE);
}

static int append(const uint8_t *p, size_t n)
{
    if (blen + n > INPUT_MAX) {
        ck_printf("event input_window_exceeded have=%u add=%u max=%u\n", (unsigned)blen, (unsigned)n, INPUT_MAX);
        return -1;
    }
    memcpy(in_buf + blen, p, n);
    blen += n;
    return 0;
}

static int read_line(void)
{
    if (src_pos >= src_len)
        return 0;
    const unsigned char *s = src_text + src_pos;
    unsigned long n = 0;
    while (src_pos + n < src_len && s[n] != '\n')
        n++;
    int nl = src_pos + n < src_len;
    if (append(s, n) != 0)
        return -1;
    if (append((const uint8_t *)"\n", 1) != 0)
        return -1;
    src_pos += n + (unsigned long)nl;
    return 1;
}

static size_t dec(uint64_t v, uint8_t *o)
{
    uint8_t t[24];
    size_t n = 0;
    do {
        t[n++] = (uint8_t)('0' + v % 10);
        v /= 10;
    } while (v);
    for (size_t i = 0; i < n; i++)
        o[i] = t[n - 1 - i];
    return n;
}

static const char *var_get(const char *name)
{
    if (streq(name, "HOME"))
        return "/home/t";
    if (streq(name, "X"))
        return "a b";
    if (streq(name, "EMPTY"))
        return "";
    return NULL;
}

/* Per fixture, as omega tests/osh/osh_trace.c load_fixture(): fake pipeline statuses from the .status sidecar, the
 * positionals $1.. from the .args sidecar (one per line), $0 = "osh_trace". */
#define MAX_FAKE 256
#define MAX_POS 9
static int last_status; /* the shell's $?: fake pipeline statuses, 2 after a refusal */
static int fake[MAX_FAKE], nfake, fake_i, have_status;
static char pos_buf[4096];
static const char *pos[MAX_POS];
static int npos;
static int exhausted; /* .status ran out: omega's osh_trace exits 2 after its event line, with no exit line */
static const char ARG0[] = "osh_trace";

static int next_fake(void)
{
    if (fake_i < nfake)
        return fake[fake_i++];
    if (have_status) {
        ck_printf("event fake_status_exhausted after=%d\n", nfake);
        exhausted = 1;
    }
    return 0;
}

static int is_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

/* 0, or -1 with a line printed: a malformed sidecar is a fixture error (omega's osh_trace exits 2 on it) */
static int load_fixture(unsigned i)
{
    nfake = fake_i = have_status = npos = exhausted = 0;
    last_status = 0;
    if (osh_fx[i].status) {
        const unsigned char *s = osh_fx[i].status;
        unsigned long n = osh_fx[i].status_len, q = 0;
        have_status = 1;
        for (;;) {
            while (q < n && is_ws(s[q]))
                q++;
            if (q >= n)
                break;
            unsigned long v = 0, d = 0;
            while (q < n && s[q] >= '0' && s[q] <= '9' && v <= 255) {
                v = v * 10 + (unsigned long)(s[q++] - '0');
                d++;
            }
            if (d == 0 || v > 255 || (q < n && !is_ws(s[q])) || nfake == MAX_FAKE) {
                ck_printf("osh_fixture_error: %s.status; %s\n", osh_fx[i].name, LABEL);
                return -1;
            }
            fake[nfake++] = (int)v;
        }
    }
    if (osh_fx[i].args) {
        const unsigned char *s = osh_fx[i].args;
        unsigned long n = osh_fx[i].args_len, q = 0, o = 0;
        if (n >= sizeof pos_buf) {
            ck_printf("osh_fixture_error: %s.args too long; %s\n", osh_fx[i].name, LABEL);
            return -1;
        }
        while (q < n) {
            if (npos == MAX_POS) {
                ck_printf("osh_fixture_error: %s.args has more than %d arguments; %s\n", osh_fx[i].name, MAX_POS, LABEL);
                return -1;
            }
            pos[npos++] = pos_buf + o;
            while (q < n && s[q] != '\n')
                pos_buf[o++] = (char)s[q++];
            pos_buf[o++] = 0;
            q++; /* the newline (or one past the end) */
        }
    }
    return 0;
}

static void answer(void)
{
    uint64_t kind = w[OSH_S_VR_KIND], a = w[OSH_S_VR_A], len = w[OSH_S_VR_LEN];
    const uint8_t *val = NULL;
    uint8_t num[24];
    size_t vl = 0;
    int found = 0;
    switch (kind) {
    case 1: {
        char name[256];
        if (a <= blen && len < sizeof name && len <= blen - a) {
            memcpy(name, in_buf + a, (size_t)len);
            name[len] = 0;
            const char *v = var_get(name);
            if (v) {
                found = 1;
                val = (const uint8_t *)v;
                vl = strlen(v);
            }
        }
        break;
    }
    case 2:
        found = 1;
        vl = dec((uint64_t)(unsigned)last_status, num);
        val = num;
        break;
    case 3: /* as osh_shell.c answer() */
        if (a == 0) {
            found = 1;
            val = (const uint8_t *)ARG0;
            vl = strlen(ARG0);
        } else if (a >= 1 && a <= (uint64_t)npos) {
            found = 1;
            val = (const uint8_t *)pos[a - 1];
            vl = strlen(pos[a - 1]);
        }
        break;
    case 4:
        found = 1;
        vl = dec((uint64_t)npos, num);
        val = num;
        break;
    default:
        break;
    }
    w[OSH_STAGING + 0] = (uint64_t)found;
    w[OSH_STAGING + 1] = vl;
    w[OSH_STAGING + 2] = (uint64_t)npos;
    w[OSH_STAGING + 3] = 0;
    for (size_t i = 0; i < vl && i < 1024; i++)
        w[OSH_STAGING + 4 + i] = val[i];
}

#ifdef CK_OSH_OP_TEST
static int real_mode, real_stop;
static int real_pipeline(void);
#endif

/* returns 1 when the driver must stop */
static int run_list(void)
{
    w[OSH_S_LAST_STATUS] = (uint64_t)(unsigned)last_status;
    for (int guard = 0; guard < 1000000; guard++) {
        uint64_t st = call(U_EXPAND);
        if (stop)
            return 1;
        switch (st) {
        case ST_BUDGET:
            continue;
        case ST_NEED_VAR:
            answer();
            continue;
        case ST_PIPE: {
            uint64_t nc = w[OSH_REQUEST + 1];
            if (nc > (OSH_OUT - OSH_REQUEST - REQ_HDR_CELLS) / CMD_CELLS) { /* never read past REQUEST */
                ck_printf("event request_ncmds_out_of_range ncmds=%llu\n", (unsigned long long)nc);
                last_status = 2;
                return 1;
            }
            size_t cells = (size_t)(REQ_HDR_CELLS + nc * CMD_CELLS);
#ifdef CK_OSH_OP_TEST
            if (real_mode) {
                last_status = real_pipeline();
                if (real_stop)
                    return 1;
                w[OSH_S_LAST_STATUS] = (uint64_t)(unsigned)last_status;
                continue;
            }
#endif
            char h[65];
            hex((const uint8_t *)(w + OSH_REQUEST), cells * 8, h);
            ck_printf("request %lu sha256=%s\n", ++req_no, h);
            last_status = next_fake(); /* nothing ran: the fixture's next status */
            if (exhausted)
                return 1;
            w[OSH_S_LAST_STATUS] = (uint64_t)(unsigned)last_status;
            continue;
        }
        case ST_COMPLETE:
            return 0;
        default: /* as osh_shell.c: a refusal stops a non-interactive shell with 2, a unit fault status with 70 */
            if (st == ~(uint64_t)0 || st < 200) {
                ck_printf("event internal_error status=%llu\n", (unsigned long long)st);
                last_status = 70;
                return 1;
            }
            last_status = 2;
            return 1;
        }
    }
    return 1;
}

static int run_src(void)
{
    for (;;) {
        reset_list();
        uint64_t st;
        int eof = 0;
        for (;;) {
            int r = read_line();
            if (r < 0) {
                last_status = 2;
                return 2;
            }
            if (r == 0) {
                eof = 1;
                if (blen == 0)
                    return 0;
                w[OSH_S_EOI] = 1;
            }
            st = lex_parse();
            if (stop)
                return 3;
            if (st == 0 && w[OSH_PX_STATE] != 4)
                st = eof ? ST_COMPLETE : ST_MORE;
            if (st == ST_MORE && !eof)
                continue;
            break;
        }
        if (st == ST_COMPLETE) {
        } else if (st == 0) {
            if (run_list())
                return 1;
        } else {
            last_status = 2; /* lexer or parser refusal */
            return 2;
        }
        if (eof)
            return 0;
    }
}

static void neg_line(const char *tag, const char *what, const struct osc_result *r, const struct ck_osc_launch_info *info, const char *extra)
{
    char rs[96];
    ck_osc_result_str(r, rs, sizeof rs);
    ck_printf("osh_neg%s: %s -> %s ticks=%llu budget=%llu pages_mapped=%u page_tables_zeroed=%u slot_free=%u%s%s; %s\n", tag, what,
              rs, (unsigned long long)r->ticks, (unsigned long long)info->budget, info->pages_mapped, (unsigned)info->tables_zeroed,
              (unsigned)info->slot_free_after, extra[0] ? " " : "", extra, LABEL);
}

/* the longest fixture, used by control 4 */
static unsigned long_fx;

static void negatives(void)
{
    struct osc_result r;
    struct ck_osc_launch_info info;
    char h0[65], h1[65], x[160];

    /* 1: 22 pages, pointer range claiming 11456 cells -> refused 41 before the first instruction */
    reset_list();
    memcpy(in_buf, "echo hi\n", 8);
    struct ck_osc_ws ws22 = { ws_mem, WS_PAGES - 1 };
    launch(U_LEX, 8, CKOSH_WS_CELLS, &ws22, 0, &r, &info);
    neg_line("1", "lex_run(in+0,8,ws+0,11456) with a 22-page workspace", &r, &info, "");

    /* 2a: workspace passed with len 512 cells (23 pages mapped): emit_tok with t=127 would store at cell 572 */
    reset_list();
    w[17] = 127;
    hex(ws_mem + 512 * 8, sizeof ws_mem - 512 * 8, h0); /* every cell from 512 up */
    struct ck_osc_launch_req rq;
    memset(&rq, 0, sizeof rq);
    rq.name = "emit_tok";
    rq.name_len = 8;
    rq.nargs = 6;
    rq.args[0] = ck_osc_va_ws();
    rq.args[1] = 512; /* cells: len 512, far smaller than the unit needs */
    rq.args[2] = 0x1111;
    rq.args[3] = 0x2222;
    rq.args[4] = 0x3333;
    rq.args[5] = 0x4444;
    rq.ws = &main_ws;
    launch_rcpt(u_rcpt[U_LEX], unit[U_LEX].bytes, unit[U_LEX].len, &unit[U_LEX].acc, &rq, &r, &info);
    hex(ws_mem + 512 * 8, sizeof ws_mem - 512 * 8, h1);
    ck_snprintf(x, sizeof x, "cell17=%llu cells_512_up_unchanged=%u", (unsigned long long)w[17], (unsigned)streq(h0, h1));
    neg_line("2a", "lex emit_tok(ws+0,len 512 cells,..) with w[17]=127", &r, &info, x);

    /* 2b: the entry point with len 512: the unit's own ABI check refuses (WORKSPACE_SIZE 213), no write past 512 */
    reset_list();
    hex(ws_mem + 512 * 8, sizeof ws_mem - 512 * 8, h0);
    memcpy(in_buf, "echo hi\n", 8);
    launch(U_LEX, 8, 512, &main_ws, 0, &r, &info);
    hex(ws_mem + 512 * 8, sizeof ws_mem - 512 * 8, h1);
    ck_snprintf(x, sizeof x, "cells_512_up_unchanged=%u", (unsigned)streq(h0, h1));
    neg_line("2b", "lex_run(in+0,8,ws+0,512) with a 23-page workspace", &r, &info, x);

    /* 3: a unit with one flipped code byte is refused at admission by the Store stage; here: nothing of it can run */
    int ran = 0;
    for (unsigned u = 0; u < U_COUNT; u++)
        if (streq(u_file[u], "osh_lex_flipped.unit") && unit[u].have)
            ran = 1;
    ck_printf("osh_neg3: osh_lex_flipped.unit -> NOT_RUN (unit was not admitted)%s; %s\n", ran ? " BUG" : "", LABEL);

    /* 4: caller max_ticks 1 on the longest fixture */
    reset_list();
    src_text = osh_fx[long_fx].text;
    src_len = osh_fx[long_fx].len;
    src_pos = 0;
    blen = 0;
    {
        /* the whole fixture as one list buffer (it fits the window) */
        unsigned long n = src_len < INPUT_MAX ? src_len : INPUT_MAX - 1;
        memcpy(in_buf, src_text, n);
        in_buf[n++] = '\n';
        blen = n;
    }
    w[OSH_S_EOI] = 1;
    launch(U_LEX, blen, CKOSH_WS_CELLS, &main_ws, 1, &r, &info);
    ck_snprintf(x, sizeof x, "fixture=%s in_len=%u max_ticks=1", osh_fx[long_fx].name, (unsigned)blen);
    neg_line("4a", "lex_run over the long fixture", &r, &info, x);
    /* 4b: the shell units are loop-bounded (every loop has a bound, every entry call a step budget), so no single call of
     * theirs lasts a whole 10 ms tick under QEMU TCG: 4a returns inside the tick. The same cap through the same launch
     * path, on the launch gate's spin() unit, shows the budget bites. */
    if (spin_unit.have) {
        memset(&rq, 0, sizeof rq);
        rq.name = "spin";
        rq.name_len = 4;
        rq.max_ticks = 1;
        launch_rcpt("l01.rcpt", spin_unit.bytes, spin_unit.len, &spin_unit.acc, &rq, &r, &info);
        neg_line("4b", "l01 spin() with the same max_ticks=1 cap", &r, &info, "");
    } else {
        ck_printf("osh_neg4b: NOT_RUN (l01_launch_fns.unit was not admitted); %s\n", LABEL);
    }
    /* and the next launch works */
    reset_list();
    memcpy(in_buf, "echo hi\n", 8);
    w[OSH_S_EOI] = 1;
    launch(U_LEX, 8, CKOSH_WS_CELLS, &main_ws, 0, &r, &info);
    neg_line("4n", "after the overrun, lex_run(in+0,8) on the same workspace", &r, &info, "");
}

#ifdef CK_OSH_OP_TEST
/* ---- OSH-AIENOS-OP: a real, capability-authorized operation through the same shell units ----
 * The fixture driver above is unchanged and is NOT run in this build (the OSH_CORE gate runs it). Here each script goes
 * through the same lex/parse/expand EL0 tasks; on PIPELINE_READY the request record goes to core/osh_op.c instead of
 * a fake status. Operation: `klog <words>` writes "osh_op_out: <words>" to the console; it needs WRITE on the console-log
 * resource in the session's kernel capability table. aienos-mv#430 adds native stage pipelines (below): gen, xform and sink
 * are admitted stage units run as EL0 tasks over native pipes, with redirections into TEST locations. QEMU (aarch64 virt),
 * TEST signer, not physical. */
static struct osh_session sess;
static const char *case_name;
static unsigned sink_calls;
static int real_crashed;

/* ---- aienos-mv#431: the effect journal (core/osh_journal.c) ----
 * Spec read: aien-protocols-mv main fb5fca98e667 (OSH Platform ABI v1 sections 9, 10, 13) and feat/osh-stream-abi-43
 * 181855c9ff3e (STREAM_TASK ST8). Two backends, one at a time on one Cortex heap: the canonical Cortex over the boot
 * disk's sealed TEST-key Store (durable=1, svc/store_boot.c keeps the Store open in this image only) and a labelled RAM
 * journal (durable=0, accepted only because the session says allow_volatile) for the #429/#430 cases, which test other
 * things. A RAM journal is never counted as durable. The Store journal holds at most 64 chunks (CK_CJ_MAX_CHUNKS, one
 * per record plus the Cortex header), so the durable cases are few and small and the last ones fill it. */
const CxJournalOps *ck_store_osh_journal(void **ctx);
unsigned ck_store_osh_chunks(unsigned *max);
void ck_store_osh_release(void);
void ck_cx_heap_init(void *base, size_t size);
#define JHEAP_PAGES 1024u /* CK_CX_HEAP_PAGES (svc/cortex_journal.h): the whole journal and its replay */
enum { JB_RAM = 0, JB_STORE = 1 };
static uint64_t jheap_pa;
static uint8_t jram_buf[256u * 1024u];
static struct oshj_ram jram;
static struct osh_journal gj;
static int jbackend;   /* what run_case attaches */
static int powercut;   /* halt the machine where the adapter is lost (the host kills QEMU there) */

/* (Re)open the one journal handle. The Cortex heap is a bump region that frees nothing: it starts over at every open,
 * which is safe because only this handle uses it. A backend that cannot open leaves the handle closed (unusable). */
static int jopen(int backend, int ro)
{
    osh_journal_close(&gj);
    memset(&gj, 0, sizeof gj);
    if (!jheap_pa)
        return CX_ERR_NOMEM;
    ck_cx_heap_init((void *)(uintptr_t)jheap_pa, (size_t)JHEAP_PAGES * 4096u);
    if (backend == JB_RAM)
        return osh_journal_open(&gj, &oshj_ram_ops, &jram, 0, "ram_volatile_test", ro);
    void *ctx = 0;
    const CxJournalOps *ops = ck_store_osh_journal(&ctx);
    if (!ops)
        return CX_ERR_IO;
    return osh_journal_open(&gj, ops, ctx, 1, "store_cortex", ro);
}

static void jattach(struct osh_session *s, int backend)
{
    (void)jopen(backend, 0);
    osh_session_journal(s, &gj, backend == JB_RAM);
}

static unsigned jchunks(void)
{
    unsigned max;
    return ck_store_osh_chunks(&max);
}

/* one line: what a read-only look at the journal shows (no OSH session needed) */
static void insp(const char *when, int rc)
{
    struct oshj_report rp;
    memset(&rp, 0, sizeof rp);
    if (rc == CX_OK)
        oshj_inspect(&gj.cx, &rp, 0, 0);
    ck_printf("osh_op_inspect: case=%s when=%s rc=%d label=%s durable=%u readonly=%u records=%llu osh=%u other=%u bad=%u "
              "intents=%u started=%u outcomes=%u receipts=%u reconciles=%u dup_outcomes=%u finished=%u unknown_intent=%u "
              "unknown_started=%u recovered=%u reconciled=%u pipelines=%u sealed=%u unsealed_finished=%u chain_ok=%d "
              "chunks=%u osh_units_launched=%lu; %s\n",
              case_name, when, rc, gj.label ? gj.label : "none", gj.durable, gj.readonly, (unsigned long long)rp.records,
              rp.osh, rp.other, rp.bad, rp.intents, rp.started, rp.outcomes, rp.receipts, rp.reconciles, rp.dup_outcomes,
              rp.cat[OSHJ_CAT_FINISHED], rp.cat[OSHJ_CAT_UNKNOWN_INTENT], rp.cat[OSHJ_CAT_UNKNOWN_STARTED],
              rp.cat[OSHJ_CAT_RECOVERED], rp.cat[OSHJ_CAT_RECONCILED], rp.pipelines, rp.sealed, rp.unsealed_finished,
              rp.chain_ok, jbackend == JB_STORE ? jchunks() : 0u, launches, LABEL);
}

static int op_sink(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    (void)len;
    sink_calls++;
    ck_printf("%s", line);
    return 0;
}

static void op_log(void *ctx, const char *line)
{
    (void)ctx;
    ck_printf("%s case=%s; %s\n", line, case_name, LABEL);
}

/* ---- aienos-mv#430: native stage pipelines through the same shell units ----
 * Registry: gen (tgen.unit), xform (txform.unit), sink (tsink.unit), each admitted with its own receipt (the task gate's
 * stages, tests/fixtures/task); nounit is tsink presented with NO receipt (refused at its spawn: RECEIPT_MISSING); foreign
 * names a class this adapter does not provide (UNAVAILABLE for the whole pipeline). Every stage is an EL0 task of the
 * kernel runner (core/task_osc.c) over native pipes (core/pipe.c). The last command's stdout comes back through
 * op_stdout. QEMU (aarch64 virt), TEST signer, not physical. */
enum { SI_GEN = 0, SI_XFORM = 1, SI_SINK = 2, SI_NORCPT = 3, SI_COUNT = 4 };
static struct ck_task_osc_image simg[SI_COUNT];
static const struct osh_op_def sops[] = {
    /* aienos-mv#431: the stage identity in every spawn intent is the admitted unit's UnitDigest */
    { "gen", OSH_CLASS_NATIVE_OP, 2, SI_GEN, 0, sunit[SU_GEN].acc.unit_digest },
    { "xform", OSH_CLASS_NATIVE_OP, 3, SI_XFORM, 0, sunit[SU_XFORM].acc.unit_digest },
    { "sink", OSH_CLASS_NATIVE_OP, 2, SI_SINK, 512, sunit[SU_SINK].acc.unit_digest },
    { "nounit", OSH_CLASS_NATIVE_OP, 2, SI_NORCPT, 512, sunit[SU_SINK].acc.unit_digest },
    { "foreign", OSH_CLASS_LINUX, 0, 0, 0, 0 },
};
static struct ck_task_tab stab;
static struct ck_pipe_pool spool;
static struct osh_native snat;
static struct ck_task_osc_stats sost;
static uint8_t case_key;

static void stage_image(unsigned i, unsigned u, int with_rcpt)
{
    simg[i].unit = sunit[u].bytes;
    simg[i].unit_len = sunit[u].len;
    simg[i].acc = &sunit[u].acc;
    simg[i].rcpt_len = 0;
    simg[i].rcpt = with_rcpt ? ck_osc_receipt_find(su_rcpt[u], &simg[i].rcpt_len) : 0;
    if (!simg[i].rcpt)
        simg[i].rcpt_len = 0;
    simg[i].entry = su_entry[u];
}

static uint8_t pat(uint64_t i) /* tgen.osc byte i, computed here independently of the unit */
{
    return i % 5 == 0 ? 0 : (uint8_t)((((i * 2654435761ull) & 0xFFFFFFFFull) >> 13) & 255u);
}

static uint64_t fnv(uint64_t total, uint8_t key) /* what tsink reports for `total` bytes of tgen through xform `key` */
{
    uint64_t h = 14695981039346656037ull;
    for (uint64_t i = 0; i < total; i++)
        h = (h ^ (uint8_t)(pat(i) ^ key)) * 1099511628211ull;
    return h;
}

static uint64_t le64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | b[i];
    return v;
}

/* the session's stdout: the byte count and the first 16 bytes (tsink's count and hash) */
static uint64_t so_bytes;
static uint8_t so_head[16];

static int op_stdout(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n && so_bytes + i < sizeof so_head; i++)
        so_head[so_bytes + i] = b[i];
    so_bytes += n;
    return 0;
}

static const struct osh_env op_env = { 0, op_sink, op_log, op_stdout, op_log };

static void hex32(const uint8_t d[32], char o[65])
{
    static const char x[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        o[2 * i] = x[d[i] >> 4];
        o[2 * i + 1] = x[d[i] & 15];
    }
    o[64] = 0;
}

static char sl[640];

static void sl_add(const char *s)
{
    size_t n = strlen(sl), k = strlen(s);
    if (n + k < sizeof sl) {
        memcpy(sl + n, s, k + 1);
    }
}

/* one line per stage pipeline: how each command ended, the stdout, the TEST locations */
static void stages_line(const struct osh_result *r)
{
    char t[96];
    sl[0] = 0;
    for (unsigned k = 0; k < r->ncmds && k < OSH_MAX_CMDS; k++) {
        ck_snprintf(t, sizeof t, " c%u=%d/%s/%s", k, r->cmd_status[k], osh_outcome_name(r->cmd_outcome[k]),
                    osh_err_name(r->cmd_error[k]));
        sl_add(t);
    }
    for (unsigned i = 0; i < OSH_LOC_MAX; i++) {
        const struct osh_loc *l = &sess.locs[i];
        if (!l->used)
            continue;
        char nm[OSH_LOC_NAME + 1];
        memcpy(nm, l->name, l->name_len);
        nm[l->name_len] = 0;
        ck_snprintf(t, sizeof t, " loc_%s=%u", nm, (unsigned)l->len);
        sl_add(t);
    }
    uint64_t cnt = so_bytes == 16 ? le64(so_head) : 0;
    int hash_ok = so_bytes == 16 && le64(so_head + 8) == fnv(cnt, case_key);
    ck_printf("osh_op_stages: case=%s n=%u launch=%s started=%u cmds=%u%s stdout_bytes=%llu sink_count=%llu "
              "sink_hash_ok=%d max_in_pipes=%llu mid_alive=%u mid_in_flight=%llu; %s\n",
              case_name, (unsigned)r->seq, osh_err_name(r->launch_error), r->started, r->ncmds, sl,
              (unsigned long long)so_bytes, (unsigned long long)cnt, hash_ok, (unsigned long long)r->max_resident, r->mid_alive,
              (unsigned long long)r->mid_in_flight, LABEL);
}

static void pipeline_line(const struct osh_result *r)
{
    char h[65];
    hex32(r->digest, h);
    ck_printf("osh_op_pipeline: case=%s n=%u status=%d error=%s outcome=%s stop=%d crashed=%d request_digest=%s; %s\n", case_name,
              (unsigned)r->seq, r->status, osh_err_name(r->error), osh_outcome_name(r->outcome), r->stop, r->crashed, h, LABEL);
    if (sess.nat)
        stages_line(r);
}

static int real_pipeline(void)
{
    struct osh_result r;
    so_bytes = 0;
    memset(so_head, 0, sizeof so_head);
    osh_op_exec(&sess, &op_env, w + OSH_REQUEST, w + OSH_OUT, (size_t)(CKOSH_WS_CELLS - OSH_OUT), &r);
    pipeline_line(&r);
    if (r.crashed && powercut) {
        /* aienos-mv#431: the machine stops here, as at a power cut: no close, no reap, no recovery in this boot. What is
         * on the disk is exactly what the journal made durable before this point. */
        ck_printf("osh_op_powercut: HALT case=%s n=%u crash_point=%u spawned=%llu chunks=%u; %s\n", case_name,
                  (unsigned)r.seq, r.crash_point, (unsigned long long)stab.stats.spawned, jchunks(), LABEL);
        for (;;)
            __asm__ volatile("wfi");
    }
    if (r.stop)
        real_stop = 1;
    if (r.crashed)
        real_crashed = 1;
    return r.status;
}

/* a native session: the spawn capability (rights, 0 = none), the location capability, a deadline, xform's key */
struct ncfg {
    unsigned spawn, loc;
    uint64_t deadline;
    uint8_t key;
};

static void native_attach(const struct ncfg *nc)
{
    if (nc->spawn)
        (void)osh_session_grant_spawn(&sess, nc->spawn);
    if (nc->loc)
        (void)osh_session_grant_loc(&sess, nc->loc);
    memset(&snat, 0, sizeof snat);
    snat.tab = &stab;
    snat.pool = &spool;
    ck_task_osc_runner(&snat.run, &sost);
    for (unsigned i = 0; i < SI_COUNT; i++)
        snat.images[i] = &simg[i];
    snat.ops = sops;
    snat.nops = (unsigned)(sizeof sops / sizeof sops[0]);
    snat.step_ticks = 100; /* per step: 1 s of 10 ms ticks, far above what a step needs (as the task gate) */
    snat.deadline_ticks = nc->deadline;
    case_key = nc->key;
    osh_session_attach(&sess, &snat);
}

static struct osh_session fresh; /* static: a session carries its TEST locations */

static void run_case(const char *name, unsigned rights, const char *script, struct osh_hooks hk, const struct ncfg *nc)
{
    case_name = name;
    osh_session_init(&sess, rights);
    if (nc)
        native_attach(nc);
    jattach(&sess, jbackend);
    uint64_t sp0 = stab.stats.spawned;
    sess.hooks = hk;
    sink_calls = 0;
    real_mode = 1;
    real_stop = 0;
    real_crashed = 0;
    stop = 0;
    step_no = req_no = 0;
    nfake = fake_i = have_status = npos = exhausted = 0;
    last_status = 0;
    src_text = (const unsigned char *)script;
    src_len = strlen(script);
    src_pos = 0;
    unsigned long l0 = launches, s0 = slots_held;
    ck_printf("osh_op_begin: case=%s cap_rights=%u; %s\n", name, rights, LABEL);
    int rc = script[0] ? run_src() : 0;
    struct ck_handle held = sess.console_cap;
    int empty = osh_session_close(&sess);
    int released = ck_cap_lookup(&sess.caps, held, 0, 0) != CK_CAP_OK;
    ck_printf("osh_op_end: case=%s rc=%d final_status=%d sink_calls=%u launches=%lu slots_held=%lu cap_table_empty=%d cap_released=%d; %s\n",
              name, rc, last_status, sink_calls, launches - l0, slots_held - s0, empty, released, LABEL);
    if (nc)
        ck_printf("osh_op_native_end: case=%s tasks_empty=%d pipes_empty=%d stage_steps=%llu not_torn_down=%llu "
                  "admit_refused=%llu; %s\n",
                  name, ck_task_tab_empty(&stab), ck_pipe_pool_empty(&spool), (unsigned long long)sost.launches,
                  (unsigned long long)sost.not_torn_down, (unsigned long long)sost.admit_refused, LABEL);
    if (real_crashed) {
        /* the adapter was lost: its journal handle goes with it. A read-only look needs no OSH session and cannot write;
         * then a restart holds no capability, reopens the journal (what survives) and recovers: OUTCOME_UNKNOWN once per
         * open intent, never a replay (aienos-mv#431) */
        unsigned before = sink_calls;
        int rc = jopen(jbackend, 1);
        size_t len0 = jram.len;
        unsigned ch0 = jbackend == JB_STORE ? jchunks() : 0u;
        struct oshj_effect dummy;
        memset(&dummy, 0, sizeof dummy);
        int ro_refused = rc == CX_OK && oshj_put_outcome(&gj, 1, &dummy, OSHJ_O_COMPLETED, 0, 0, 0, 0) != CX_OK &&
                         jram.len == len0 && (jbackend == JB_STORE ? jchunks() : 0u) == ch0;
        insp("after_loss", rc);
        ck_printf("osh_op_readonly: case=%s write_refused=%d; %s\n", name, ro_refused, LABEL);
        osh_session_restart(&fresh, &sess);
        jattach(&fresh, jbackend);
        unsigned n = osh_op_recover(&fresh, &op_env);
        struct oshj_recovery rv = fresh.recovery; /* the first scan's work; the second must find nothing */
        unsigned again = osh_op_recover(&fresh, &op_env);
        ck_printf("osh_op_restart: case=%s open_intents=%u second_scan=%u sink_calls_before=%u sink_calls_after=%u held_cap=%u; %s\n",
                  name, n, again, before, sink_calls, (unsigned)fresh.console_cap.generation, LABEL);
        ck_printf("osh_op_recovery: case=%s unknown_intent=%u unknown_started=%u outcomes_written=%u receipts_written=%u "
                  "failed=%u second_written=%u spawned_since_case=%llu epoch=%u; %s\n",
                  name, rv.unknown_intent, rv.unknown_started, rv.outcomes_written, rv.receipts_written, rv.failed,
                  fresh.recovery.outcomes_written + fresh.recovery.receipts_written,
                  (unsigned long long)(stab.stats.spawned - sp0), fresh.epoch, LABEL);
        insp("after_recovery", gj.open ? CX_OK : CX_ERR_IO);
    }
    if (jbackend == JB_STORE)
        ck_printf("osh_op_durable_end: case=%s spawned=%llu chunks=%u; %s\n", name,
                  (unsigned long long)(stab.stats.spawned - sp0), jchunks(), LABEL);
    real_mode = 0;
}

static void scenario(const char *name, unsigned rights, const char *script, struct osh_hooks hk)
{
    run_case(name, rights, script, hk, 0);
}

/* the direct-argv route (section 11): a record built here, not by the shell units, into the same executor */
static uint64_t dreq[OSH_REQ_HDR + 2u * OSH_REQ_CMD_CELLS];
static uint64_t dout[64];
static uint64_t dused;

static void dword(uint64_t *blk, const char *s)
{
    size_t l = strlen(s);
    blk[4 + 2 * blk[0]] = dused;
    blk[5 + 2 * blk[0]] = l;
    blk[0]++;
    for (size_t i = 0; i < l; i++)
        dout[dused++] = (uint8_t)s[i];
}

static void direct_record(void) /* gen 4096 512 | sink, flagged DIRECT */
{
    memset(dreq, 0, sizeof dreq);
    memset(dout, 0, sizeof dout);
    dused = 0;
    dreq[0] = OSH_REQ_MAGIC;
    dreq[1] = 2;
    dreq[3] = OSH_REQ_F_DIRECT;
    dreq[5] = 1;
    uint64_t *c0 = dreq + OSH_REQ_HDR, *c1 = c0 + OSH_REQ_CMD_CELLS;
    dword(c0, "gen");
    dword(c0, "4096");
    dword(c0, "512");
    dword(c1, "sink");
    dreq[4] = dused;
}

static void direct_exec(void)
{
    struct osh_result r;
    so_bytes = 0;
    memset(so_head, 0, sizeof so_head);
    osh_op_exec(&sess, &op_env, dreq, dout, sizeof dout / sizeof dout[0], &r);
    pipeline_line(&r);
}

/* direct: the record as submitted; malicious: the same record with a builtin on command 0 (malformed on the direct
 * route), then with command 1's argv[0] outside OUT; direct_no_cap: the same record, no spawn capability */
static void direct_case(const char *name, int variant, const struct ncfg *nc)
{
    case_name = name;
    osh_session_init(&sess, CK_R_WRITE);
    native_attach(nc);
    jattach(&sess, jbackend);
    ck_printf("osh_op_begin: case=%s cap_rights=%u; %s\n", name, (unsigned)CK_R_WRITE, LABEL);
    direct_record();
    if (variant == 1) {
        dreq[OSH_REQ_HDR + 3] = 3;
        direct_exec();
        direct_record();
        dreq[OSH_REQ_HDR + OSH_REQ_CMD_CELLS + 4] = 9999;
    }
    direct_exec();
    int empty = osh_session_close(&sess);
    ck_printf("osh_op_native_end: case=%s tasks_empty=%d pipes_empty=%d stage_steps=%llu not_torn_down=%llu "
              "admit_refused=%llu cap_table_empty=%d; %s\n",
              name, ck_task_tab_empty(&stab), ck_pipe_pool_empty(&spool), (unsigned long long)sost.launches,
              (unsigned long long)sost.not_torn_down, (unsigned long long)sost.admit_refused, empty, LABEL);
}

/* ---- aienos-mv#431: durable cases on the Store journal, chosen by what the journal on the disk already holds ----
 *   fresh     no OSH record yet: the #429/#430 cases (RAM journal), then the durable cases below; every loss is
 *             recovered in this boot. Ends normally.
 *   powercut  OSH records, none open: a two-stage pipeline loses its adapter after both spawns are recorded STARTED
 *             and the machine halts there (the host kills QEMU: a real power cut, no reap, no recovery).
 *   recover   open intents (after the power cut): a read-only look before any shell unit runs, recovery
 *             (OUTCOME_UNKNOWN, never a replay), a fresh decision, then the journal is filled to its 64-chunk limit
 *             and the next effects are refused by name (FAILED_NO_EFFECT) with no spawn.
 * UNVERIFIED U22: the three-boot sequence and the case list are this gate's choice; the spec pins the rules, not the
 * test. */
enum { JM_NONE = 0, JM_FRESH = 1, JM_POWERCUT = 2, JM_RECOVER = 3 };
static const char *const jm_name[] = { "store_unavailable", "fresh", "powercut", "recover" };

static int journal_boot(void)
{
    oshj_ram_init(&jram, jram_buf, sizeof jram_buf);
    if (ck_mm_frames_alloc(JHEAP_PAGES, &jheap_pa) != 0)
        jheap_pa = 0;
    case_name = "journal_boot";
    jbackend = JB_STORE;
    int mode = JM_NONE, created = 0;
    int rc = jopen(JB_STORE, 1);
    struct oshj_report rp;
    memset(&rp, 0, sizeof rp);
    if (rc == CX_OK) {
        oshj_inspect(&gj.cx, &rp, 0, 0);
        unsigned open = rp.cat[OSHJ_CAT_UNKNOWN_INTENT] + rp.cat[OSHJ_CAT_UNKNOWN_STARTED] + rp.unsealed_finished;
        mode = rp.osh == 0 ? JM_FRESH : open ? JM_RECOVER : JM_POWERCUT;
    } else if (jheap_pa && ck_store_osh_journal(&(void *){ 0 })) {
        /* an empty journal has no header yet, and a read-only open cannot write one: the first writable open does */
        rc = jopen(JB_STORE, 0);
        created = rc == CX_OK;
        mode = created ? JM_FRESH : JM_NONE;
    }
    insp("boot", rc);
    ck_printf("osh_op_journal_boot: mode=%s created=%d heap=%d; %s\n", jm_name[mode], created, jheap_pa != 0, LABEL);
    osh_journal_close(&gj);
    jbackend = JB_RAM;
    return mode;
}

struct dcrash {
    const char *name, *script;
    unsigned cp, nth;
    int interrupt;
};

static uint64_t last_recovered;

static void find_recovered(void *u, const struct oshj_item *it)
{
    (void)u;
    if (it->cat == OSHJ_CAT_RECOVERED)
        last_recovered = it->intent_id;
}

/* operator reconciliation of the effect recovery left OUTCOME_UNKNOWN: a principal without the capability is DENIED,
 * the operator's statement is one RECONCILE record, a second statement about the same effect is refused */
static void reconcile_case(void)
{
    uint8_t opid[32], other[32];
    sha256_hash((const uint8_t *)"osh-op-test-operator", 20, opid);
    sha256_hash((const uint8_t *)"osh-op-test-not-operator", 24, other);
    last_recovered = 0;
    struct oshj_report rp;
    oshj_inspect(&gj.cx, &rp, find_recovered, 0);
    case_name = "d_reconcile";
    unsigned g = (unsigned)osh_session_grant_reconcile(&fresh, opid, CK_R_WRITE);
    unsigned ch0 = jchunks();
    unsigned e1 = osh_op_reconcile(&fresh, &op_env, other, last_recovered, OSHJ_V_OBSERVED_DONE);
    unsigned ch1 = jchunks();
    unsigned e2 = osh_op_reconcile(&fresh, &op_env, opid, last_recovered, OSHJ_V_OBSERVED_DONE);
    unsigned ch2 = jchunks();
    unsigned e3 = osh_op_reconcile(&fresh, &op_env, opid, last_recovered, OSHJ_V_ACCEPT_UNKNOWN);
    unsigned ch3 = jchunks();
    ck_printf("osh_op_reconcile: case=d_reconcile intent=%llu grant=%u not_operator=%s operator=%s again=%s "
              "records_added=%u/%u/%u; %s\n",
              (unsigned long long)last_recovered, g, osh_err_name(e1), osh_err_name(e2), osh_err_name(e3), ch1 - ch0,
              ch2 - ch1, ch3 - ch2, LABEL);
    insp("after_reconcile", CX_OK);
    (void)osh_session_close(&fresh);
}

static void durable_fresh(const struct ncfg *k0)
{
    struct osh_hooks none = { 0 };
    jbackend = JB_STORE;
    run_case("d_ok", CK_R_WRITE, "gen 4096 512 | sink\n", none, k0);
    run_case("d_write", CK_R_WRITE, "gen 100 >o\n", none, k0);
    static const struct dcrash dc[] = {
        { "d_before_intent", "gen 4096 512\n", OSH_CP_BEFORE_INTENT, 1, 0 },
        { "d_after_intent", "gen 4096 512\n", OSH_CP_AFTER_INTENT, 1, 0 },
        { "d_after_spawn", "gen 4096 512\n", OSH_CP_AFTER_EFFECT, 1, 0 },
        { "d_after_started", "gen 4096 512\n", OSH_CP_AFTER_STARTED, 1, 0 },
        { "d_after_finish", "gen 4096 512\n", OSH_CP_AFTER_FINISH, 0, 0 },
        { "d_during_cancel", "gen 0\n", OSH_CP_DURING_CANCEL, 0, 1 },
        { "d_during_receipt", "gen 4096 512\n", OSH_CP_DURING_RECEIPT, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof dc / sizeof dc[0]; i++) {
        struct osh_hooks h = { 0 };
        h.crash_at = 1;
        h.crash_point = dc[i].cp;
        h.crash_nth = dc[i].nth;
        if (dc[i].interrupt) {
            h.interrupt_at = 1;
            h.interrupt_after_turns = 30;
        }
        run_case(dc[i].name, CK_R_WRITE, dc[i].script, h, k0);
        if (dc[i].cp == OSH_CP_AFTER_EFFECT)
            reconcile_case();
    }
    case_name = "d_end";
    insp("end", gj.open ? CX_OK : CX_ERR_IO);
    ck_printf("osh_op_durable_done: mode=fresh cases=10; %s\n", LABEL);
}

static void durable_powercut(const struct ncfg *k0)
{
    struct osh_hooks h = { 0 };
    h.crash_at = 1;
    h.crash_point = OSH_CP_AFTER_STARTED;
    h.crash_nth = 2;
    jbackend = JB_STORE;
    powercut = 1;
    run_case("powercut", CK_R_WRITE, "gen 4096 512 | sink\n", h, k0);
    powercut = 0;
    ck_printf("osh_op_powercut: NOT_HALTED (the loss point was not reached); %s\n", LABEL);
}

static void durable_recover(const struct ncfg *k0)
{
    struct osh_hooks none = { 0 };
    jbackend = JB_STORE;
    /* the adapter that was lost at the power cut is gone: a new one holds no capability and recovers */
    case_name = "powercut";
    osh_session_init(&fresh, 0);
    jattach(&fresh, JB_STORE);
    uint64_t sp0 = stab.stats.spawned;
    unsigned n = osh_op_recover(&fresh, &op_env);
    struct oshj_recovery rv = fresh.recovery;
    unsigned again = osh_op_recover(&fresh, &op_env);
    ck_printf("osh_op_restart: case=powercut open_intents=%u second_scan=%u unknown_intent=%u unknown_started=%u "
              "outcomes_written=%u receipts_written=%u failed=%u second_written=%u spawned=%llu held_cap=%u epoch=%u; %s\n",
              n, again, rv.unknown_intent, rv.unknown_started, rv.outcomes_written, rv.receipts_written, rv.failed,
              fresh.recovery.outcomes_written + fresh.recovery.receipts_written, (unsigned long long)(stab.stats.spawned - sp0),
              (unsigned)fresh.console_cap.generation, fresh.epoch, LABEL);
    insp("after_recovery", gj.open ? CX_OK : CX_ERR_IO);
    (void)osh_session_close(&fresh);
    /* a new request after the loss is a new decision: it runs once, durably */
    run_case("pc_fresh", CK_R_WRITE, "gen 4096 512 | sink\n", none, k0);
    /* fill the journal to its limit with single-stage pipelines (4 records each), then effects are refused by name */
    unsigned max, fills = 0;
    while (ck_store_osh_chunks(&max) + 4u <= max && fills < 16u) {
        run_case("fill", CK_R_WRITE, "gen 1\n", none, k0);
        fills++;
    }
    unsigned ch = ck_store_osh_chunks(&max);
    ck_printf("osh_op_fill: pipelines=%u chunks=%u max=%u; %s\n", fills, ch, max, LABEL);
    run_case("full", CK_R_WRITE, "gen 1\ngen 1 | sink\n", none, k0);
    case_name = "d_end";
    insp("end", gj.open ? CX_OK : CX_ERR_IO);
    ck_printf("osh_op_durable_done: mode=recover cases=%u; %s\n", 2u + fills, LABEL);
}

static void op_scenarios(void)
{
    int mode = journal_boot();
    int old = mode == JM_FRESH || mode == JM_NONE;
    struct osh_hooks none = { 0 };
    struct osh_hooks rv = { .revoke_before_perm_at = 1 }, ir = { .interrupt_before_effect_at = 2 },
                     cr = { .crash_after_effect_at = 1 };
    if (old) {
        scenario("allow", CK_R_WRITE, "klog hello from osh\nklog \"status=$?\"\n", none);
        scenario("deny_no_cap", 0, "klog SECRET-no-cap\n", none);
        scenario("deny_read_only", CK_R_READ, "klog SECRET-read-only\n", none);
        scenario("revoked", CK_R_WRITE, "klog SECRET-revoked\n", rv);
        scenario("interrupt", CK_R_WRITE, "klog one; klog two; klog three\n", ir);
        scenario("lost", CK_R_WRITE, "klog LOST-MARKER\n", cr);
        scenario("after_restart", CK_R_WRITE, "klog fresh decision\n", none);
        scenario("unsupported", CK_R_WRITE, "klog a | klog SECRET-pipe\nklog SECRET-redir >f\nnosuchcmd\n", none);
    }

    /* aienos-mv#430: native stage pipelines */
    unsigned have = 0;
    for (unsigned u = 0; u < SU_COUNT; u++)
        have += (unsigned)sunit[u].have;
    if (have != SU_COUNT) {
        ck_printf("osh_op_stages: NOT_RUN (a stage unit was not admitted, %u of %u); %s\n", have, SU_COUNT, LABEL);
        ck_printf("osh_op_done: cases=8; %s\n", LABEL);
        ck_store_osh_release();
        return;
    }
    stage_image(SI_GEN, SU_GEN, 1);
    stage_image(SI_XFORM, SU_XFORM, 1);
    stage_image(SI_SINK, SU_SINK, 1);
    stage_image(SI_NORCPT, SU_SINK, 0);
    const unsigned RW = CK_R_READ | CK_R_WRITE;
    const struct ncfg k90 = { CK_R_WRITE, RW, 0, 90 }, k7 = { CK_R_WRITE, RW, 0, 7 }, k0 = { CK_R_WRITE, RW, 0, 0 };
    const struct ncfg nospawn = { 0, RW, 0, 0 }, roloc = { CK_R_WRITE, CK_R_READ, 0, 0 }, dl = { CK_R_WRITE, RW, 20, 0 };
    /* three distinct admitted stages at once, 2.5 pipe capacities through the middle one; $? is the last command's */
    if (mode == JM_POWERCUT)
        durable_powercut(&k0);
    if (mode == JM_RECOVER)
        durable_recover(&k0);
    if (!old) {
        ck_store_osh_release();
        return;
    }
    struct osh_hooks snap = { .snapshot_at = 1, .snapshot_after_turns = 20 }; /* who is alive mid-run */
    run_case("stream3", CK_R_WRITE, "gen 163840 4096 | xform 90 | sink\nklog \"stream3 status=$?\"\n", snap, &k90);
    /* the pipeline status is the last command's: 3 when xform (stopping after 1000 bytes with 3) is last, 0 when sink follows */
    run_case("last_status", CK_R_WRITE, "gen 4096 | xform 0 1000 3\nklog \"last=$?\"\ngen 4096 | xform 0 1000 3 | sink\n"
                                        "klog \"last=$?\"\n", none, &k0);
    /* redirections in record order, then a location round trip through a second pipeline */
    run_case("redir", CK_R_WRITE, "gen 100 >p >o\ngen 1000 >o\nxform 7 <o | sink\ngen 10 >>o\n", none, &k7);
    run_case("redir_dup", CK_R_WRITE, "gen 100 2>&1 >o\n", none, &k0);
    /* refusals of one command or of the whole pipeline */
    run_case("not_found", CK_R_WRITE, "gen 4096 | nosuch | sink\n", none, &k0);
    run_case("nounit", CK_R_WRITE, "gen 4096 | nounit\n", none, &k0);
    run_case("foreign", CK_R_WRITE, "gen 10 | foreign\n", none, &k0);
    run_case("no_spawn_cap", CK_R_WRITE, "gen 10 | sink\n", none, &nospawn);
    run_case("loc_read_only", CK_R_WRITE, "gen 10 >o\n", none, &roloc);
    struct osh_hooks rs = { .revoke_spawn_at = 1, .revoke_spawn_cmd = 1 };
    run_case("revoke_between", CK_R_WRITE, "gen 4096 | sink\n", rs, &k0);
    /* a launch failure after two commands started: they are cancelled and reaped, PARTIAL_LAUNCH, status 1 */
    struct osh_hooks fs = { .fail_spawn_at = 1, .fail_spawn_cmd = 2, .fail_spawn_err = OSH_E_LIMIT };
    run_case("partial", CK_R_WRITE, "gen 0 | xform 1 | sink\n", fs, &k0);
    /* lost right after the first spawn: OUTCOME_UNKNOWN, the kernel reaps the stage, a restart never replays */
    struct osh_hooks cs = { .crash_after_effect_at = 1, .crash_cmd = 0 };
    run_case("lost_stage", CK_R_WRITE, "gen 0 | sink\nklog SECRET-after-loss\n", cs, &k0);
    /* an endless producer: an interrupt after 30 scheduler turns, then a deadline of 20 ticks */
    struct osh_hooks it = { .interrupt_at = 1, .interrupt_after_turns = 30 };
    run_case("interrupt_stages", CK_R_WRITE, "gen 0 | sink\nklog SECRET-after-interrupt\n", it, &k0);
    run_case("deadline", CK_R_WRITE, "gen 0 | sink\n", none, &dl);
    /* the direct-argv route */
    direct_case("direct", 0, &k0);
    direct_case("malicious", 1, &k0);
    direct_case("direct_no_cap", 0, &nospawn);
    ck_printf("osh_op_done: cases=25; %s\n", LABEL);
    if (mode == JM_FRESH) {
        durable_fresh(&k0);
    } else {
        /* no Store journal: an effect that needs one is refused by name, nothing spawns */
        jbackend = JB_STORE;
        run_case("d_unavailable", CK_R_WRITE, "gen 10\n", none, &k0);
        jbackend = JB_RAM;
    }
    osh_journal_close(&gj);
    ck_store_osh_release();
}
#endif

void ck_osh_all(void)
{
    unsigned have = 0;
    for (unsigned u = 0; u < U_COUNT; u++)
        have += (unsigned)unit[u].have;
    ck_printf("osh_core: units=%u ws_cells=%u ws_pages=%u input_max=%u; %s\n", have, CKOSH_WS_CELLS, WS_PAGES, INPUT_MAX, LABEL);
    if (have != U_COUNT) {
        ck_printf("osh_core: NOT_RUN (a shell unit was not admitted); %s\n", LABEL);
        return;
    }
#ifdef CK_OSH_OP_TEST
    op_scenarios();
    return;
#endif
    unsigned nfx = (unsigned)(sizeof osh_fx / sizeof osh_fx[0]);
    unsigned long best = 0;
    for (unsigned i = 0; i < nfx; i++) {
        if (osh_fx[i].len > best && osh_fx[i].len <= INPUT_MAX) {
            best = osh_fx[i].len;
            long_fx = i;
        }
        stop = 0;
        step_no = 0;
        req_no = 0;
        src_text = osh_fx[i].text;
        src_len = osh_fx[i].len;
        src_pos = 0;
        ck_printf("osh_script_begin %s\n", osh_fx[i].name);
        int rc = load_fixture(i) != 0 ? 4 : run_src();
        /* omega's osh_trace ends every trace with the exit line, except after fake_status_exhausted (it exits 2 there);
         * after a launch that did not RETURN (stop) the trace is incomplete and the gate's diff fails it */
        if (rc != 4 && !stop && !exhausted)
            ck_printf("event exit=%d\n", last_status);
        ck_printf("osh_script_end %s rc=%d\n", osh_fx[i].name, rc);
    }
    negatives();
    ck_printf("osh_core_done: scripts=%u; %s\n", nfx, LABEL);
}
