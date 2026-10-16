/* Spark-side peer for the Pi <-> Spark session test (aienos-mv#398). Hosted Linux, TEST identities only.
 *
 *   pispark_peer <pi-ipv4> <seconds>
 *
 * Binds UDP PSS_PORT_BASE..+5 (one pss_resp each, see pispark.h), answers only datagrams whose source is
 * <pi-ipv4>, logs every datagram and every protocol event (with the SHA-256 of each request as received)
 * on stdout, and exits after <seconds>. Fresh randomness from getrandom(2) for every datagram handled. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../pispark.h"
#include "../../argus/sha256.h"

static uint64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t T0;
static volatile sig_atomic_t STOP;
static void on_term(int sig) { (void)sig; STOP = 1; }
static void hex(const uint8_t *b, size_t n, char *o)
{
    static const char d[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        o[2 * i] = d[b[i] >> 4];
        o[2 * i + 1] = d[b[i] & 15];
    }
    o[2 * n] = 0;
}

static const char *kind_name(int k)
{
    switch (k) {
    case PSS_EV_HS1_NEW: return "HS1_NEW";
    case PSS_EV_SEC: return "SEC_REFUSED";
    case PSS_EV_ESTABLISHED: return "ESTABLISHED";
    case PSS_EV_REQUEST: return "REQUEST";
    case PSS_EV_INJECT: return "INJECT";
    case PSS_EV_CTL: return "CTL_REFUSED";
    case PSS_EV_CLOSED: return "PEER_CLOSED";
    case PSS_EV_SILENT_DROP: return "SILENT_DROP";
    case PSS_EV_BAD_REQ: return "BAD_REQUEST";
    default: return "?";
    }
}

static int fill_random(uint8_t *b, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(b + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static pss_resp R[PSS_NSCN];
static pss_stale STALE;
static pss_out O;
static int FD[PSS_NSCN];
static struct sockaddr_in PI;
static int have_pi[PSS_NSCN];
static struct sockaddr_in LAST[PSS_NSCN];
static unsigned long n_rx, n_tx, n_foreign, n_err, n_ka;

/* The Spark host firewall drops new inbound UDP (input policy drop, "ct state established,related accept"). Instead of
 * changing it, every port sends a 4-byte "PSKA" keepalive to the Pi port every 5 s, so the Pi's datagrams to that
 * port are replies of an existing flow. The Pi counts and drops PSKA (dev/pi5_peer.c). */
#define KA_MS 5000u
static void keepalive(uint64_t now, uint64_t *next)
{
    struct sockaddr_in to = PI;
    int s;
    if (now < *next)
        return;
    *next = now + KA_MS;
    to.sin_family = AF_INET;
    to.sin_port = htons(PSS_PI_PORT);
    for (s = 0; s < PSS_NSCN; s++)
        if (sendto(FD[s], "PSKA", 4, 0, (struct sockaddr *)&to, sizeof to) == 4)
            n_ka++;
}

static void emit(int s, const pss_out *o)
{
    uint32_t i;
    char h[65], sid[17];
    for (i = 0; i < o->nev; i++) {
        const pss_event *e = &o->ev[i];
        printf("EV t_ms=%llu port=%u scn=%s kind=%s code=%d index=%u len=%u", (unsigned long long)(ms_now() - T0),
               (unsigned)(PSS_PORT_BASE + (unsigned)s), pss_scenario_name(s), kind_name(e->kind), e->code, e->index,
               e->len);
        if (e->kind == PSS_EV_REQUEST) {
            hex(e->hash, 32, h);
            printf(" sha256=%s", h);
        }
        if (e->kind == PSS_EV_ESTABLISHED) {
            hex(R[s].sec.session_id, 8, sid);
            printf(" session=%s", sid);
        }
        printf("\n");
    }
    for (i = 0; i < o->n; i++) {
        ssize_t w;
        if (!have_pi[s])
            break;
        w = sendto(FD[s], o->buf[i], o->len[i], 0, (struct sockaddr *)&LAST[s], sizeof LAST[s]);
        if (w == (ssize_t)o->len[i]) {
            n_tx++;
            printf("TX t_ms=%llu port=%u len=%zu type=0x%02x\n", (unsigned long long)(ms_now() - T0),
                   (unsigned)(PSS_PORT_BASE + (unsigned)s), o->len[i], o->len[i] > 5 ? o->buf[i][5] : 0);
        } else {
            n_err++;
            printf("TX_ERR t_ms=%llu port=%u errno=%d\n", (unsigned long long)(ms_now() - T0),
                   (unsigned)(PSS_PORT_BASE + (unsigned)s), errno);
        }
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    struct pollfd pf[PSS_NSCN];
    uint64_t end;
    int s;
    static uint8_t buf[2048];
    if (argc != 3) {
        fprintf(stderr, "usage: pispark_peer <pi-ipv4> <seconds>\n");
        return 2;
    }
    memset(&PI, 0, sizeof PI);
    if (inet_pton(AF_INET, argv[1], &PI.sin_addr) != 1) {
        fprintf(stderr, "bad pi address\n");
        return 2;
    }
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    T0 = ms_now();
    end = T0 + (uint64_t)strtoul(argv[2], NULL, 10) * 1000u;
    for (s = 0; s < PSS_NSCN; s++) {
        struct sockaddr_in a;
        int one = 1;
        if (pss_resp_init(&R[s], s, &STALE, 200, 8) != PSS_OK)
            return 1;
        FD[s] = socket(AF_INET, SOCK_DGRAM, 0);
        if (FD[s] < 0)
            return 1;
        setsockopt(FD[s], SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)(PSS_PORT_BASE + (unsigned)s));
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(FD[s], (struct sockaddr *)&a, sizeof a) != 0) {
            perror("bind");
            return 1;
        }
        pf[s].fd = FD[s];
        pf[s].events = POLLIN;
    }
    printf("PEER start pi=%s ports=%u..%u rto_ms=200 max_retries=8 identity=TEST(%s) accepts=TEST(%s)\n", argv[1],
           PSS_PORT_BASE, PSS_PORT_BASE + PSS_NSCN - 1, PSS_LABEL_SPARK, PSS_LABEL_PI);
    fflush(stdout);
    uint64_t ka_next = 0;
    while (ms_now() < end && !STOP) {
        int k = poll(pf, PSS_NSCN, 5);
        uint64_t now = ms_now() - T0;
        keepalive(now, &ka_next);
        if (k < 0 && errno != EINTR)
            break;
        for (s = 0; s < PSS_NSCN; s++) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t n;
            uint8_t e[80], dh[32];
            char h[17];
            if (!(pf[s].revents & POLLIN))
                continue;
            n = recvfrom(FD[s], buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n < 0)
                continue;
            if (from.sin_addr.s_addr != PI.sin_addr.s_addr) {
                n_foreign++;
                printf("RX_FOREIGN t_ms=%llu port=%u from=%s len=%zd dropped\n", (unsigned long long)now,
                       (unsigned)(PSS_PORT_BASE + (unsigned)s), inet_ntoa(from.sin_addr), n);
                continue;
            }
            n_rx++;
            sha256_hash(buf, (size_t)n, dh);
            hex(dh, 8, h);
            printf("RX t_ms=%llu port=%u from=%s:%u len=%zd type=0x%02x dg_sha256_8=%s\n", (unsigned long long)now,
                   (unsigned)(PSS_PORT_BASE + (unsigned)s), inet_ntoa(from.sin_addr), ntohs(from.sin_port), n,
                   n > 5 ? buf[5] : 0, h);
            LAST[s] = from;
            have_pi[s] = 1;
            if (fill_random(e, sizeof e) != 0) {
                printf("RNG_FAIL: refusing to answer\n");
                continue;
            }
            pss_resp_input(&R[s], now, buf, (size_t)n, e, &O);
            memset(e, 0, sizeof e);
            emit(s, &O);
        }
        for (s = 0; s < PSS_NSCN; s++) {
            pss_resp_poll(&R[s], ms_now() - T0, &O);
            if (O.n || O.nev)
                emit(s, &O);
        }
    }
    printf("PEER end rx=%lu tx=%lu foreign=%lu tx_err=%lu keepalive_sent=%lu\n", n_rx, n_tx, n_foreign, n_err, n_ka);
    for (s = 0; s < PSS_NSCN; s++)
        printf("PEER port=%u scn=%s requests=%u replies=%u sec_rx_records=%llu sec_refused=%llu sec_replay=%llu "
               "sec_identity=%llu sec_bad_tag=%llu sec_malformed=%llu\n",
               (unsigned)(PSS_PORT_BASE + (unsigned)s), pss_scenario_name(s), R[s].requests, R[s].replies,
               (unsigned long long)R[s].sec.c.rx_records, (unsigned long long)R[s].sec.c.rx_refused,
               (unsigned long long)R[s].sec.c.rx_replay, (unsigned long long)R[s].sec.c.rx_identity,
               (unsigned long long)R[s].sec.c.rx_bad_tag, (unsigned long long)R[s].sec.c.rx_malformed);
    return 0;
}
