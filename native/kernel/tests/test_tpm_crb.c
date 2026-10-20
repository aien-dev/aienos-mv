/* test_tpm_crb.c -- host tests of dev/tpm_crb.c (L200) against a register-level
 * simulator of the PTP CRB window.
 *
 *   test_tpm_crb              unit mode: the simulator answers from a tiny built-in
 *                             fake TPM; checks handshake order, wire bytes, every
 *                             refusal and timeout, ACPI TPM2 table parsing.
 *   test_tpm_crb swtpm SOCK   the simulator forwards each command to a REAL swtpm
 *                             (unix socket server, raw TPM 2.0 commands) and prints
 *                             the observable lines that
 *                             tests/fixtures/tpm_crb_swtpm_*_expected.txt pins.
 *                             Driven by scripts/tpm_crb_swtpm_test.sh.
 * Nothing here touches a real TPM. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "ck_test.h"
#include "tpm_crb.h"

#define BASE 0xfed40000ull

/* ---- CRB window simulator ---- */
struct sim {
    uint8_t win[0x1000];
    uint32_t now_us;
    /* knobs */
    uint32_t valid_after_us;   /* tpmRegValidSts appears after this (UINT32_MAX: never) */
    int grant_locality;        /* 0: never assigns */
    int locality_preassigned;
    uint32_t ready_latency_us, exec_latency_us, idle_latency_us;
    int never_start_clear, fatal, all_ones;
    uint32_t cmd_addr, rsp_addr, cmd_size, rsp_size;
    /* pending */
    int req_pending;           /* bit mask being acknowledged */
    uint32_t req_at_us;
    int start_pending;
    uint32_t start_at_us;
    /* backend */
    int (*exec)(struct sim *, const uint8_t *cmd, size_t n, uint8_t *rsp, size_t *rn);
    int swtpm_fd;
    /* log */
    char log[256];
    uint8_t last_cmd[1100];
    size_t last_cmd_len;
    unsigned fake_rc;          /* unit backend: respond with this TPM_RC */
    int fake_mode;             /* unit backend: malformed response selector */
    unsigned random_calls;
    unsigned retry_n;          /* unit backend: answer TPM_RC_RETRY this many times first */
    unsigned attempts;         /* unit backend: commands received */
};

static uint32_t w32(const struct sim *s, uint32_t off) { uint32_t v; memcpy(&v, s->win + off, 4); return v; }
static void s32(struct sim *s, uint32_t off, uint32_t v) { memcpy(s->win + off, &v, 4); }
static void logc(struct sim *s, char c) { size_t n = strlen(s->log); if (n + 1 < sizeof s->log) { s->log[n] = c; s->log[n + 1] = 0; } }

static void settle(struct sim *s)
{
    if (s->req_pending && s->now_us >= s->req_at_us) {
        uint32_t r = w32(s, 0x40) & ~(uint32_t)s->req_pending;
        s32(s, 0x40, r);
        uint32_t st = w32(s, 0x44);
        if (s->req_pending & 1) st &= ~2u; /* cmdReady: leave Idle */
        if (s->req_pending & 2) st |= 2u;  /* goIdle: Idle */
        s32(s, 0x44, st);
        s->req_pending = 0;
    }
    if (s->start_pending && s->now_us >= s->start_at_us && !s->never_start_clear) {
        uint32_t n = (uint32_t)(s->win[0x80 + 2] << 24 | s->win[0x80 + 3] << 16 | s->win[0x80 + 4] << 8 | s->win[0x80 + 5]);
        uint8_t rsp[2048];
        size_t rn = 0;
        uint32_t coff = s->cmd_addr - (uint32_t)BASE;
        n = (uint32_t)(s->win[coff + 2] << 24 | s->win[coff + 3] << 16 | s->win[coff + 4] << 8 | s->win[coff + 5]);
        s->last_cmd_len = n < sizeof s->last_cmd ? n : sizeof s->last_cmd;
        memcpy(s->last_cmd, s->win + coff, s->last_cmd_len);
        if (s->exec(s, s->win + coff, n, rsp, &rn) == 0) {
            uint32_t roff = s->rsp_addr - (uint32_t)BASE;
            memcpy(s->win + roff, rsp, rn);
        }
        s32(s, 0x4c, 0);
        s->start_pending = 0;
    }
}

static uint32_t sim_rd32(void *c, uint32_t off)
{
    struct sim *s = c;
    if (s->all_ones) return 0xffffffffu;
    settle(s);
    uint32_t v = w32(s, off);
    if (off == 0x00) {
        v &= ~0x80u;
        if (s->now_us >= s->valid_after_us) v |= 0x80u;
        if (s->locality_preassigned || (w32(s, 0x08) & 1 && s->grant_locality)) v |= 2u;
    }
    if (off == 0x44 && s->fatal) v |= 1u;
    return v;
}

static void sim_wr32(void *c, uint32_t off, uint32_t v)
{
    struct sim *s = c;
    settle(s);
    if (off == 0x40) {
        logc(s, v & 1 ? 'R' : 'G');
        s32(s, off, v);
        s->req_pending = (int)v;
        s->req_at_us = s->now_us + (v & 1 ? s->ready_latency_us : s->idle_latency_us);
        return;
    }
    if (off == 0x4c) {
        logc(s, 'S');
        s32(s, off, v);
        s->start_pending = 1;
        s->start_at_us = s->now_us + s->exec_latency_us;
        return;
    }
    s32(s, off, v);
}

static void sim_rd_buf(void *c, uint32_t off, uint8_t *d, size_t n)
{
    struct sim *s = c;
    logc(s, 'r');
    settle(s);
    memcpy(d, s->win + off, n);
}

static void sim_wr_buf(void *c, uint32_t off, const uint8_t *d, size_t n)
{
    struct sim *s = c;
    logc(s, 'w');
    memcpy(s->win + off, d, n);
}

static void sim_delay(void *c, uint32_t us) { ((struct sim *)c)->now_us += us; }

static void sim_init(struct sim *s)
{
    memset(s, 0, sizeof *s);
    s->grant_locality = 1;
    s->cmd_addr = (uint32_t)BASE + 0x80;
    s->rsp_addr = (uint32_t)BASE + 0x80;
    s->cmd_size = s->rsp_size = 0xf80;
    s32(s, 0x44, 2); /* Idle */
    s32(s, 0x30, 1); /* InterfaceId: CRB */
    s32(s, 0x34, 0x1014u | 0x0001u << 16); /* vid 0x1014, did 1 */
    s32(s, 0x58, s->cmd_size);
    s32(s, 0x5c, s->cmd_addr);
    s32(s, 0x64, s->rsp_size);
    s32(s, 0x68, s->rsp_addr);
}

static const struct ck_tpm_crb_ops *ops_for(struct sim *s, struct ck_tpm_crb_ops *o)
{
    o->rd32 = sim_rd32; o->wr32 = sim_wr32; o->rd_buf = sim_rd_buf; o->wr_buf = sim_wr_buf;
    o->delay_us = sim_delay; o->ctx = s;
    return o;
}

/* ---- unit backend: a minimal fake TPM ---- */
static void put_be(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * (n - 1 - i))); }

static int fake_exec(struct sim *s, const uint8_t *cmd, size_t n, uint8_t *rsp, size_t *rn)
{
    uint32_t cc = (uint32_t)cmd[6] << 24 | cmd[7] << 16 | cmd[8] << 8 | cmd[9];
    (void)n;
    memset(rsp, 0, 64);
    s->attempts++;
    if (s->retry_n) {
        s->retry_n--;
        put_be(rsp, cmd[0] << 8 | cmd[1], 2);
        put_be(rsp + 2, 10, 4);
        put_be(rsp + 6, CK_TPM_RC_RETRY, 4);
        *rn = 10;
        return 0;
    }
    put_be(rsp, cmd[0] << 8 | cmd[1], 2);
    put_be(rsp + 6, s->fake_rc, 4);
    *rn = 10;
    if (s->fake_rc) { put_be(rsp + 2, 10, 4); return 0; }
    switch (cc) {
    case 0x144: break;
    case 0x17b: {
        unsigned want = (unsigned)(cmd[10] << 8 | cmd[11]);
        unsigned give = s->fake_mode == 1 ? 0 : want;
        put_be(rsp + 10, give, 2);
        for (unsigned i = 0; i < give; i++) rsp[12 + i] = (uint8_t)(0x40 + s->random_calls * 7 + i);
        s->random_calls++;
        *rn = 12 + give;
        break;
    }
    case 0x17e: {
        unsigned pcr = 0;
        for (unsigned i = 0; i < 24; i++) if (cmd[10 + 4 + 2 + 1 + i / 8] & (1u << (i % 8))) pcr = i;
        put_be(rsp + 10, 0x2a, 4);          /* updateCounter */
        put_be(rsp + 14, 1, 4);             /* selection count */
        put_be(rsp + 18, 0x0b, 2); rsp[20] = 3; rsp[21] = cmd[17]; rsp[22] = cmd[18]; rsp[23] = cmd[19];
        put_be(rsp + 24, 1, 4);             /* digest count */
        put_be(rsp + 28, 32, 2);
        for (unsigned i = 0; i < 32; i++) rsp[30 + i] = (uint8_t)(pcr * 16 + i);
        *rn = 62;
        break;
    }
    case 0x14e: {
        unsigned sz = (unsigned)(cmd[10 + 12 + 9] << 8 | cmd[10 + 12 + 9 + 1]);
        unsigned off = (unsigned)(cmd[10 + 12 + 9 + 2] << 8 | cmd[10 + 12 + 9 + 3]);
        unsigned give = s->fake_mode == 2 ? sz + 1 : sz;
        put_be(rsp + 10, 2 + give + 5, 4);  /* parameterSize (not checked by the driver) */
        put_be(rsp + 14, give, 2);
        for (unsigned i = 0; i < give; i++) rsp[16 + i] = (uint8_t)(off + i);
        *rn = 16 + give + 5;
        break;
    }
    default: put_be(rsp + 6, 0x143, 4); break; /* TPM_RC_COMMAND_CODE */
    }
    put_be(rsp + 2, (uint32_t)*rn, 4);
    if (s->fake_mode == 3) put_be(rsp + 2, 0x2000, 4); /* size bigger than the buffer */
    if (s->fake_mode == 4) put_be(rsp, 0x1234, 2);     /* bad tag */
    return 0;
}

static struct sim S;
static struct ck_tpm_crb T;
static struct ck_tpm_crb_ops O;

static int open_sim(void) { return ck_tpm_crb_open(&T, ops_for(&S, &O), BASE); }
static void fresh_unit(void) { sim_init(&S); S.exec = fake_exec; }

static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void test_acpi(void)
{
    uint8_t t[76] = {'T', 'P', 'M', '2'};
    uint64_t base = 0;
    uint32_t method = 0;
    put_le32(t + 4, 76); put_le32(t + 40, 0xfed40040u); put_le32(t + 48, 7);
    CHECK(ck_tpm2_acpi_parse(t, sizeof t, &base, &method) == 0 && base == 0xfed40000ull && method == 7);
    put_le32(t + 44, 1); /* control area above 4 GiB */
    CHECK(ck_tpm2_acpi_parse(t, sizeof t, &base, &method) == 0 && base == 0x1fed40000ull);
    put_le32(t + 44, 0);
    put_le32(t + 48, 8);
    CHECK(ck_tpm2_acpi_parse(t, sizeof t, &base, &method) == CK_TPM_E_START && method == 8);
    put_le32(t + 48, 11);
    CHECK(ck_tpm2_acpi_parse(t, sizeof t, &base, &method) == CK_TPM_E_START && method == 11);
    put_le32(t + 48, 7);
    CHECK(ck_tpm2_acpi_parse(t, 51, &base, &method) == CK_TPM_E_ACPI);
    put_le32(t + 4, 200); /* length field larger than the buffer */
    CHECK(ck_tpm2_acpi_parse(t, sizeof t, &base, &method) == CK_TPM_E_ACPI);
    put_le32(t + 4, 76);
    t[0] = 'X';
    CHECK(ck_tpm2_acpi_parse(t, sizeof t, &base, &method) == CK_TPM_E_ACPI);
    t[0] = 'T';
    put_le32(t + 40, 0xfed40044u); /* not 16-byte aligned */
    CHECK(ck_tpm2_acpi_parse(t, sizeof t, &base, &method) == CK_TPM_E_ACPI);
    put_le32(t + 40, 0x10u);       /* below 40h */
    CHECK(ck_tpm2_acpi_parse(t, sizeof t, &base, &method) == CK_TPM_E_ACPI);
    CHECK(ck_tpm2_acpi_parse(NULL, 76, &base, &method) == CK_TPM_E_ACPI);
}

static void test_open(void)
{
    fresh_unit();
    CHECK(open_sim() == 0 && T.vid == 0x1014 && T.did == 1 && T.cmd_off == 0x80 && T.cmd_size == 0xf80);
    fresh_unit(); S.all_ones = 1;
    CHECK(open_sim() == CK_TPM_E_NO_TPM);
    fresh_unit(); S.valid_after_us = 0xffffffffu;
    CHECK(open_sim() == CK_TPM_E_NO_TPM);
    fresh_unit(); S.valid_after_us = 100000; /* valid late but inside TIMEOUT_A */
    CHECK(open_sim() == 0);
    fresh_unit(); S.grant_locality = 0;
    CHECK(open_sim() == CK_TPM_E_LOCALITY);
    fresh_unit(); S.locality_preassigned = 1; S.grant_locality = 0;
    CHECK(open_sim() == 0);
    fresh_unit(); s32(&S, 0x5c, (uint32_t)BASE + 0x2000); /* command buffer outside the window */
    CHECK(open_sim() == CK_TPM_E_LAYOUT);
    fresh_unit(); s32(&S, 0x68, 0x1000); /* response buffer below the window */
    CHECK(open_sim() == CK_TPM_E_LAYOUT);
    fresh_unit(); s32(&S, 0x5c, (uint32_t)BASE + 0x40); /* inside the window but on the registers */
    CHECK(open_sim() == CK_TPM_E_LAYOUT);
    fresh_unit(); s32(&S, 0x58, 0xf81); /* runs past the end of the window */
    CHECK(open_sim() == CK_TPM_E_LAYOUT);
    fresh_unit(); s32(&S, 0x64, 4);     /* too small for a response header */
    CHECK(open_sim() == CK_TPM_E_LAYOUT);
    CHECK(ck_tpm_crb_open(&T, NULL, BASE) == CK_TPM_E_ARG);
    struct ck_tpm_crb_ops bad = {0};
    CHECK(ck_tpm_crb_open(&T, &bad, BASE) == CK_TPM_E_ARG);
}

static void test_wire_and_handshake(void)
{
    static const uint8_t startup_clear[] = {0x80, 0x01, 0, 0, 0, 0x0c, 0, 0, 0x01, 0x44, 0, 0};
    static const uint8_t get_random_32[] = {0x80, 0x01, 0, 0, 0, 0x0c, 0, 0, 0x01, 0x7b, 0, 0x20};
    static const uint8_t pcr_read_7[] = {0x80, 0x01, 0, 0, 0, 0x14, 0, 0, 0x01, 0x7e, 0, 0, 0, 1, 0, 0x0b, 3, 0x80, 0, 0};
    static const uint8_t nv_read[] = {0x80, 0x02, 0, 0, 0, 0x23, 0, 0, 0x01, 0x4e, 0x01, 0x50, 0, 0x16, 0x01, 0x50, 0, 0x16,
                                      0, 0, 0, 9, 0x40, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0x20, 0, 0x04};
    uint8_t d[32], out[40];
    uint32_t cnt = 0;
    fresh_unit(); S.ready_latency_us = 1000; S.exec_latency_us = 3000; S.idle_latency_us = 500;
    CHECK(open_sim() == 0);
    CHECK(ck_tpm2_startup(&T, CK_TPM_SU_CLEAR) == 0);
    CHECK(S.last_cmd_len == sizeof startup_clear && !memcmp(S.last_cmd, startup_clear, sizeof startup_clear));
    CHECK(!strcmp(S.log, "RwSrG")); /* cmdReady, command in, Start, response out, goIdle */
    CHECK((w32(&S, 0x44) & 2) && !(w32(&S, 0x40) & 3)); /* back in Idle, requests acknowledged */
    CHECK(ck_tpm2_get_random(&T, out, 32) == 0);
    CHECK(S.last_cmd_len == sizeof get_random_32 && !memcmp(S.last_cmd, get_random_32, sizeof get_random_32));
    CHECK(ck_tpm2_pcr_read_sha256(&T, 7, d, &cnt) == 0 && cnt == 0x2a && d[0] == 7 * 16 && d[31] == 7 * 16 + 31);
    CHECK(S.last_cmd_len == sizeof pcr_read_7 && !memcmp(S.last_cmd, pcr_read_7, sizeof pcr_read_7));
    CHECK(ck_tpm2_pcr_read_sha256(&T, 23, d, NULL) == 0 && d[0] == (uint8_t)(23 * 16));
    CHECK(ck_tpm2_pcr_read_sha256(&T, 24, d, NULL) == CK_TPM_E_ARG);
    CHECK(ck_tpm2_nv_read(&T, 0x01500016u, 0x01500016u, 0, out, 32) == 0 && out[0] == 0 && out[31] == 31);
    CHECK(S.last_cmd_len == sizeof nv_read);
    CHECK(ck_tpm2_nv_read(&T, CK_TPM_RH_OWNER, 0x01500016u, 8, out, 4) == 0 && out[0] == 8 && out[3] == 11);
    CHECK(ck_tpm2_nv_read(&T, 1, 2, 0, out, 0) == CK_TPM_E_ARG);
    CHECK(ck_tpm2_nv_read(&T, 1, 2, 0, out, CK_TPM_NV_READ_MAX + 1) == CK_TPM_E_ARG);
    CHECK(ck_tpm2_get_random(&T, NULL, 4) == CK_TPM_E_ARG);
    {
        uint8_t big[70];
        unsigned before = S.random_calls;
        CHECK(ck_tpm2_get_random(&T, big, 70) == 0 && S.random_calls == before + 3);
        CHECK(big[0] == 0x40 + before * 7 && big[32] == 0x40 + (before + 1) * 7);
    }
    (void)nv_read;
}

static void test_refusals(void)
{
    uint8_t d[32], out[8];
    /* the TPM's own refusal comes back as a positive TPM_RC, not a driver error */
    fresh_unit(); S.fake_rc = CK_TPM_RC_INITIALIZE;
    CHECK(open_sim() == 0 && ck_tpm2_startup(&T, CK_TPM_SU_CLEAR) == (int)CK_TPM_RC_INITIALIZE);
    S.fake_rc = 0x14b; /* TPM_RC_NV_UNINITIALIZED style code, passed through */
    CHECK(ck_tpm2_nv_read(&T, 1, 2, 0, out, 4) == 0x14b);
    /* still usable and idle after a refusal */
    S.fake_rc = 0;
    CHECK(ck_tpm2_startup(&T, CK_TPM_SU_STATE) == 0 && (w32(&S, 0x44) & 2));
    /* TPM_RC_RETRY: resubmitted, counted, bounded */
    fresh_unit(); CHECK(open_sim() == 0);
    S.retry_n = 1;
    CHECK(ck_tpm2_startup(&T, 0) == 0 && T.retries == 1 && S.attempts == 2);
    S.retry_n = 3; S.attempts = 0;
    CHECK(ck_tpm2_get_random(&T, out, 4) == 0 && T.retries == 4 && S.attempts == 4);
    S.retry_n = 100; S.attempts = 0;
    CHECK(ck_tpm2_startup(&T, 0) == (int)CK_TPM_RC_RETRY && T.retries == 7 && S.attempts == 4); /* gives up after CK_TPM_RETRY_MAX */
    S.retry_n = 0;
    CHECK(ck_tpm2_startup(&T, 0) == 0 && (w32(&S, 0x44) & 2));
    /* malformed responses */
    fresh_unit(); CHECK(open_sim() == 0);
    S.fake_mode = 1; CHECK(ck_tpm2_get_random(&T, out, 4) == CK_TPM_E_PROTO);
    S.fake_mode = 2; CHECK(ck_tpm2_nv_read(&T, 1, 2, 0, out, 4) == CK_TPM_E_PROTO);
    S.fake_mode = 3; CHECK(ck_tpm2_startup(&T, 0) == CK_TPM_E_PROTO);
    S.fake_mode = 4; CHECK(ck_tpm2_startup(&T, 0) == CK_TPM_E_PROTO);
    S.fake_mode = 0; CHECK(ck_tpm2_pcr_read_sha256(&T, 1, d, NULL) == 0);
    /* a fatal TPM, a TPM that never finishes, one that never gets ready */
    fresh_unit(); CHECK(open_sim() == 0);
    S.fatal = 1; CHECK(ck_tpm2_startup(&T, 0) == CK_TPM_E_FATAL);
    fresh_unit(); CHECK(open_sim() == 0);
    S.never_start_clear = 1; CHECK(ck_tpm2_startup(&T, 0) == CK_TPM_E_TIMEOUT);
    fresh_unit(); CHECK(open_sim() == 0);
    S.ready_latency_us = 300000; /* beyond TIMEOUT_C (200 ms) */
    CHECK(ck_tpm2_startup(&T, 0) == CK_TPM_E_TIMEOUT);
    fresh_unit(); CHECK(open_sim() == 0);
    S.ready_latency_us = 150000; /* inside TIMEOUT_C */
    CHECK(ck_tpm2_startup(&T, 0) == 0);
    /* argument checks on the exchange itself */
    {
        uint8_t c[2000] = {0}, r[64];
        size_t rl;
        fresh_unit(); CHECK(open_sim() == 0);
        CHECK(ck_tpm_crb_transact(&T, c, 4, r, sizeof r, &rl) == CK_TPM_E_ARG);
        CHECK(ck_tpm_crb_transact(&T, c, 0xf81, r, sizeof r, &rl) == CK_TPM_E_ARG);
        CHECK(ck_tpm_crb_transact(&T, c, 12, r, 4, &rl) == CK_TPM_E_ARG);
    }
}

/* ---- swtpm mode ---- */
static int sw_exec(struct sim *s, const uint8_t *cmd, size_t n, uint8_t *rsp, size_t *rn)
{
    if (write(s->swtpm_fd, cmd, n) != (ssize_t)n) return -1;
    ssize_t got = read(s->swtpm_fd, rsp, 4096);
    if (got < 10) return -1;
    *rn = (size_t)got;
    return 0;
}

static void hex(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", p[i]); }

static int swtpm_mode(const char *sock, const char *mode)
{
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    strncpy(a.sun_path, sock, sizeof a.sun_path - 1);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) { printf("tpm_crb_swtpm: FAIL (cannot connect %s)\n", sock); return 1; }
    sim_init(&S);
    S.exec = sw_exec; S.swtpm_fd = fd;
    S.ready_latency_us = 200; S.exec_latency_us = 1000; S.idle_latency_us = 200;
    int rc = open_sim();
    printf("open: rc=%d vid=0x%04x did=0x%04x cmd_off=0x%x cmd_size=0x%x rsp_off=0x%x rsp_size=0x%x\n", rc, T.vid, T.did,
           T.cmd_off, T.cmd_size, T.rsp_off, T.rsp_size);
    if (rc) return 1;
    rc = ck_tpm2_startup(&T, CK_TPM_SU_CLEAR);
    printf("startup(%s): rc=0x%x\n", mode, (unsigned)rc);
    uint8_t r1[48], r2[48];
    rc = ck_tpm2_get_random(&T, r1, sizeof r1);
    int rc2 = ck_tpm2_get_random(&T, r2, sizeof r2);
    printf("getrandom: rc=%d rc2=%d bytes=%zu differ=%s allzero=%s\n", rc, rc2, sizeof r1, memcmp(r1, r2, sizeof r1) ? "yes" : "no",
           ({ int z = 1; for (unsigned i = 0; i < sizeof r1; i++) if (r1[i]) z = 0; z ? "yes" : "no"; }));
    uint8_t d[32];
    uint32_t cnt = 0;
    for (unsigned pcr = 0; pcr < 24; pcr += 16) {
        rc = ck_tpm2_pcr_read_sha256(&T, pcr, d, &cnt);
        printf("pcr%u: rc=%d ", pcr, rc);
        hex(d, 32);
        printf("\n");
    }
    rc = ck_tpm2_pcr_read_sha256(&T, 24, d, &cnt);
    printf("pcr24: rc=%d\n", rc);
    uint8_t nv[32];
    rc = ck_tpm2_nv_read(&T, 0x01500016u, 0x01500016u, 0, nv, sizeof nv);
    printf("nv_read(0x01500016,auth=index): rc=0x%x ", (unsigned)rc);
    hex(nv, rc == 0 ? sizeof nv : 0);
    printf("\n");
    rc = ck_tpm2_nv_read(&T, CK_TPM_RH_OWNER, 0x01500016u, 8, nv, 8);
    printf("nv_read(0x01500016,auth=owner,off=8,len=8): rc=0x%x ", (unsigned)rc);
    hex(nv, rc == 0 ? 8 : 0);
    printf("\n");
    rc = ck_tpm2_nv_read(&T, 0x01500099u, 0x01500099u, 0, nv, 4);
    printf("nv_read(undefined index 0x01500099): rc=0x%x\n", (unsigned)rc);
    rc = ck_tpm2_startup(&T, CK_TPM_SU_CLEAR);
    printf("startup again: rc=0x%x\n", (unsigned)rc);
    printf("retries: %u\n", T.retries);
    ck_tpm_crb_close(&T);
    printf("tpm_crb_swtpm: DONE\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "swtpm"))
        return swtpm_mode(argv[2], argv[3]);
    test_acpi();
    test_open();
    test_wire_and_handshake();
    test_refusals();
    return ck_t_verdict("test_tpm_crb");
}
