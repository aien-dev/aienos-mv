/* ck_lan_hub.c -- host-side virtual Ethernet hub for scripts/qemu_ck_interplane_test.sh
 * (gate AIENOS_CK_INTERPLANE). Hosted C (POSIX sockets), test tooling only,
 * never in the kernel image. No Python, no outside tool, no multicast (the
 * Spark's loopback has no MULTICAST flag).
 *
 *   ck_lan_hub HUB_PORT LOGFILE SECONDS PORT1 [PORT2 ...]
 *
 * Binds 127.0.0.1:HUB_PORT. Every QEMU instance uses
 *   -netdev socket,udp=127.0.0.1:HUB_PORT,localaddr=127.0.0.1:PORTi
 * so each Ethernet frame it sends arrives here as one datagram from PORTi.
 * The hub copies every frame to every other listed port (a dumb hub: one
 * shared LAN segment, broadcast and unicast alike) and drops datagrams from
 * ports it does not know. Writes "ready port=HUB_PORT" first and a per-port
 * frame count at the end. Exit 0 at the deadline or on SIGTERM (counts are still written), 2 usage, 3 socket error. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXP 16

static volatile sig_atomic_t g_stop;
static void on_term(int sig) { (void)sig; g_stop = 1; }

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int port_arg(const char *s)
{
    char *e = 0;
    long v = strtol(s, &e, 10);
    return (e && *e == 0 && v > 0 && v < 65536) ? (int)v : -1;
}

int main(int argc, char **argv)
{
    if (argc < 5 || argc - 4 > MAXP) {
        fprintf(stderr, "usage: ck_lan_hub HUB_PORT LOGFILE SECONDS PORT1 [PORT2 ...] (at most %d)\n", MAXP);
        return 2;
    }
    int hub = port_arg(argv[1]), n = argc - 4, ports[MAXP];
    long secs = strtol(argv[3], 0, 10);
    unsigned long in[MAXP] = {0}, dropped = 0;
    if (hub < 0 || secs <= 0) return 2;
    for (int i = 0; i < n; i++)
        if ((ports[i] = port_arg(argv[4 + i])) < 0) return 2;
    FILE *log = fopen(argv[2], "w");
    if (!log) return 3;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)hub);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(log, "bind failed\n");
        fclose(log);
        return 3;
    }
    fprintf(log, "ready port=%d\n", hub);
    fflush(log);
    double end = now_s() + (double)secs;
    static unsigned char buf[65536];
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    while (!g_stop && now_s() < end) {
        struct pollfd p = {s, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t r = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        int src = -1;
        for (int i = 0; i < n; i++)
            if (from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(from.sin_port) == ports[i]) src = i;
        if (src < 0) { dropped++; continue; }
        in[src]++;
        for (int i = 0; i < n; i++) {
            if (i == src) continue;
            struct sockaddr_in to = a;
            to.sin_port = htons((uint16_t)ports[i]);
            (void)sendto(s, buf, (size_t)r, 0, (struct sockaddr *)&to, sizeof to);
        }
    }
    for (int i = 0; i < n; i++) fprintf(log, "port %d frames_in=%lu\n", ports[i], in[i]);
    fprintf(log, "dropped_unknown=%lu\n", dropped);
    fclose(log);
    close(s);
    return 0;
}
