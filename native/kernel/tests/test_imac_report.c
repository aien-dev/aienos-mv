/* test_imac_report.c -- host test for the iMac kernel's network report
 * frames and allowlist filter (x86/report_x86.c, built with
 * -DCK_X86_REPORT_HOST).
 *
 *   test_imac_report                    unit checks on the frames
 *   test_imac_report send PORT LOGFILE  also writes a sample report to
 *       LOGFILE and sends every part's UDP payload to 127.0.0.1:PORT (last
 *       part first, then all again), so scripts/ck_imac_report_test.sh can
 *       check that ck_imac_report_listen rebuilds LOGFILE byte for byte. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "aienos_net.h"
#include "ck_test.h"
#include "report_x86.h"

static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static char log_text[3 * CK_X86_REPORT_CHUNK + 300];

/* Unwraps one frame; returns the UDP payload view or 0. */
static const uint8_t *unwrap(const uint8_t *f, size_t n, size_t *plen)
{
    net_eth_frame e;
    net_ipv4_header ih;
    net_udp_header uh;
    const uint8_t *ip, *pl;
    size_t il;
    static const uint8_t bc[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const net_ipv4 any = {{0, 0, 0, 0}}, bip = {{255, 255, 255, 255}};
    if (net_eth_parse(f, n, &e) != NET_OK || memcmp(e.destination.b, bc, 6) || memcmp(e.source.b, mac, 6) ||
        e.ethertype != NET_ETHERTYPE_IPV4)
        return 0;
    if (net_ipv4_parse(e.payload, e.payload_len, &ih, &ip, &il) != NET_OK || ih.protocol != NET_IPPROTO_UDP ||
        ih.ttl != 1 || memcmp(ih.source.b, any.b, 4) || memcmp(ih.destination.b, bip.b, 4))
        return 0;
    if (net_udp_parse(ip, il, ih.source, ih.destination, &uh, &pl, plen) != NET_OK ||
        uh.source_port != CK_X86_REPORT_PORT || uh.destination_port != CK_X86_REPORT_PORT)
        return 0;
    if (ip[6] == 0 && ip[7] == 0) /* the checksum must be present, not skipped */
        return 0;
    return pl;
}

/* Filter checks: allowlisted kernel lines pass unchanged, everything else
 * (sentinel chat, key and memory values, control bytes, overlong or
 * unfinished lines, a tag that only starts like an allowed one) is dropped. */
static void filter_checks(void)
{
    static const char screen[] = "cpu: GenuineIntel\n"
                                 "chat: SENTINEL-CHAT-7f3a hello\n"
                                 "pci: nvme 02:00.0 id=144d:a806\n"
                                 "key=SENTINEL-KEY-91c2\n"
                                 "memory_dump: SENTINEL-MEM-44e0\n"
                                 "netx: SENTINEL-NETX\n"
                                 " kernel: SENTINEL-INDENT\n"
                                 "kernel: alive\x1b[2J SENTINEL-CTRL\n"
                                 "\n"
                                 "summary: cores=4/4 writes=none\n"
                                 "AIENOS-IMAC-KERNEL-0 DONE\n"
                                 "power: SENTINEL-UNFINISHED";
    static const char want[] = "cpu: GenuineIntel\n"
                               "pci: nvme 02:00.0 id=144d:a806\n"
                               "summary: cores=4/4 writes=none\n"
                               "AIENOS-IMAC-KERNEL-0 DONE\n";
    char out[sizeof screen];
    unsigned dropped = 0;
    size_t n = ck_x86_report_filter(screen, sizeof screen - 1, out, sizeof out, &dropped);
    CHECK(n == sizeof want - 1 && memcmp(out, want, n) == 0);
    CHECK(dropped == 8);
    for (size_t i = 0; i + 8 <= n; i++)
        CHECK(memcmp(out + i, "SENTINEL", 8) != 0);

    char longl[CK_X86_REPORT_LINE_MAX + 2];
    memset(longl, 'x', sizeof longl);
    memcpy(longl, "kernel: ", 8);
    longl[CK_X86_REPORT_LINE_MAX] = '\n'; /* exactly LINE_MAX bytes: kept */
    CHECK(ck_x86_report_filter(longl, CK_X86_REPORT_LINE_MAX + 1, out, sizeof out, &dropped) ==
              CK_X86_REPORT_LINE_MAX + 1 &&
          dropped == 0);
    longl[CK_X86_REPORT_LINE_MAX] = 'x';
    longl[CK_X86_REPORT_LINE_MAX + 1] = '\n'; /* one byte over: dropped */
    CHECK(ck_x86_report_filter(longl, sizeof longl, out, sizeof out, &dropped) == 0 && dropped == 1);
    CHECK(ck_x86_report_filter(want, sizeof want - 1, out, 10, &dropped) == 0 && dropped == 4);
    CHECK(ck_x86_report_filter(0, 5, out, sizeof out, &dropped) == 0 && dropped == 0);
}

int main(int argc, char **argv)
{
    filter_checks();
    for (size_t i = 0; i < sizeof log_text; i++)
        log_text[i] = (i % 61 == 60) ? '\n' : (char)('A' + i % 26);
    size_t len = sizeof log_text;
    uint64_t run = 0x0123456789abcdefull;
    uint8_t f[1600];
    unsigned parts = ck_x86_report_parts(len);

    CHECK(ck_x86_report_parts(0) == 0);
    CHECK(ck_x86_report_parts(1) == 1);
    CHECK(ck_x86_report_parts(CK_X86_REPORT_CHUNK) == 1);
    CHECK(ck_x86_report_parts(CK_X86_REPORT_CHUNK + 1) == 2);
    CHECK(ck_x86_report_parts(CK_X86_REPORT_MAX) == CK_X86_REPORT_MAX / CK_X86_REPORT_CHUNK);
    CHECK(ck_x86_report_parts(CK_X86_REPORT_MAX + 1) == 0);
    CHECK(parts == 4);
    CHECK(ck_x86_report_frame(mac, run, log_text, len, parts, f, sizeof f) == 0);
    CHECK(ck_x86_report_frame(mac, run, log_text, len, 0, f, 100) == 0);
    CHECK(ck_x86_report_frame(0, run, log_text, len, 0, f, sizeof f) == 0);

    char rebuilt[sizeof log_text];
    size_t got = 0;
    for (unsigned p = 0; p < parts; p++) {
        size_t n = ck_x86_report_frame(mac, run, log_text, len, p, f, sizeof f);
        CHECK(n >= 60 && n <= 1514);
        size_t pl = 0;
        const uint8_t *d = unwrap(f, n, &pl);
        CHECK(d != 0);
        if (!d)
            continue;
        char want[128];
        int h = snprintf(want, sizeof want, "AIENOS-IMAC-REPORT v=1 run=0123456789abcdef part=%u/%u bytes=%zu\n", p,
                         parts, len);
        CHECK(pl > (size_t)h && memcmp(d, want, (size_t)h) == 0);
        if (pl > (size_t)h && got + pl - h <= sizeof rebuilt) {
            memcpy(rebuilt + got, d + h, pl - h);
            got += pl - h;
        }
    }
    CHECK(got == len && memcmp(rebuilt, log_text, len) == 0);

    if (argc == 4 && !strcmp(argv[1], "send")) {
        int lfd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        FILE *lf = lfd < 0 ? 0 : fdopen(lfd, "w");
        CHECK(lf && fwrite(log_text, 1, len, lf) == len && fclose(lf) == 0);
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)atoi(argv[2]));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(s >= 0);
        for (unsigned round = 0; round < 2; round++)
            for (unsigned k = 0; k < parts; k++) {
                unsigned p = round ? k : parts - 1 - k;
                size_t n = ck_x86_report_frame(mac, run, log_text, len, p, f, sizeof f), pl = 0;
                const uint8_t *d = unwrap(f, n, &pl);
                CHECK(d && sendto(s, d, pl, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)pl);
                nanosleep(&(struct timespec){0, 20000000}, 0);
            }
        close(s);
    }
    return ck_t_verdict("test_imac_report");
}
