/* Timing twin (ADR 0021): the same loop through the vnet_* API against the
 * generated chip model, built once with the hand-written driver and once with
 * the generated one (make contract-bench). Prints one line per repetition:
 *   bench impl=<name> rep=<r> init_ns=<per init> rtt_ns=<per tx+reclaim+rx round trip>
 * The model's own work is in both numbers equally; trace is off.
 * Usage: vnet_bench <impl-name> [reps] [round-trips per rep] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aienos_virtio_net.h"
#include "vnet_model.h"

static uint8_t region[1 << 20] __attribute__((aligned(4096)));

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int main(int argc, char **argv)
{
    const char *impl = argc > 1 ? argv[1] : "?";
    int reps = argc > 2 ? atoi(argv[2]) : 11;
    long trips = argc > 3 ? atol(argv[3]) : 200000;
    static vnet_model m;
    vnet_dev d;
    virtio_pci_caps caps;
    memset(&caps, 0, sizeof caps);
    caps.common_cfg = (virtio_pci_region){1, 4, 0, 0x1000};
    caps.isr_cfg = (virtio_pci_region){1, 4, 0x1000, 0x1000};
    caps.device_cfg = (virtio_pci_region){1, 4, 0x2000, 0x1000};
    caps.notify_cfg = (virtio_pci_region){1, 4, 0x3000, 0x1000};
    caps.has_notify_off_multiplier = 1;
    caps.notify_off_multiplier = 4;
    size_t len = vnet_mem_size(64);
    uint8_t f[VNET_FRAME_MAX], out[VNET_FRAME_MAX];
    for (size_t i = 0; i < sizeof f; i++) f[i] = (uint8_t)(i * 7u);
    unsigned long sink = 0;
    for (int r = 0; r < reps; r++) {
        const int inits = 2000;
        uint64_t t0 = now_ns();
        for (int k = 0; k < inits; k++) {
            memset(&m, 0, sizeof m);
            m.dev_features = 1ull << 32 | 1ull << 5;
            m.numq = 2;
            m.mult = 4;
            m.qmax[0] = m.qmax[1] = 256;
            vnet_model_init(&m, region, 0x40000000ull, len);
            vnet_ops ops = vnet_model_ops(&m);
            if (vnet_init(&d, &ops, &caps, region, 0x40000000ull, len, 64) != VNET_OK) { puts("init failed"); return 1; }
        }
        uint64_t t1 = now_ns();
        for (long i = 0; i < trips; i++) {
            size_t n = 60 + (size_t)(i % 1454);
            if (vnet_tx(&d, f, n) != VNET_OK) { puts("tx failed"); return 1; }
            vnet_model_tx(&m);
            unsigned rec = 0;
            vnet_tx_reclaim(&d, &rec);
            m.nwire = 0;
            vnet_model_rx(&m, f, n);
            size_t got_len = 0;
            int got = 0;
            if (vnet_rx(&d, out, sizeof out, &got_len, &got) != VNET_OK || !got || got_len != n) { puts("rx failed"); return 1; }
            sink += rec + out[n - 1];
        }
        uint64_t t2 = now_ns();
        if (m.violations) { printf("violation: %s\n", m.last_violation); return 1; }
        printf("bench impl=%s rep=%d init_ns=%llu rtt_ns=%llu\n", impl, r, (unsigned long long)((t1 - t0) / inits),
               (unsigned long long)((t2 - t1) / (uint64_t)trips));
    }
    fprintf(stderr, "sink=%lu\n", sink);
    return 0;
}
