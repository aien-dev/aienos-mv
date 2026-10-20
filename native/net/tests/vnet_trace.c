/* Differential trace (ADR 0021): one fixed scenario mix, driven through the
 * vnet_* API against the generated chip model (contract/gen/vnet_model.c).
 * Prints every register access the driver makes, every API result and a
 * digest of the shared DMA region after each call. Built twice, once with the
 * hand-written ../aienos_virtio_net.c and once with the generated
 * contract/gen/vnet_contract.c; `make contract-test` requires the two outputs
 * to be byte-identical and the model to report zero contract violations.
 * Not proven here: timing, interrupts, a real device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aienos_virtio_net.h"
#include "vnet_model.h"

static uint8_t region[1 << 20] __attribute__((aligned(4096)));
static const uint64_t DMA = 0x40000000ull;
static size_t region_len;
static unsigned violations;

static void line(void *ctx, const char *l) { (void)ctx; printf("  %s\n", l); }

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static void after(const char *what, vnet_err e, const vnet_dev *d, const vnet_model *m)
{
    printf("%s -> %s broken=%u feat=%016llx mac=%02x%02x%02x%02x%02x%02x q=%u/%u inflight=%u/%u region=%016llx viol=%u%s%s\n",
           what, vnet_err_name(e), d->broken, (unsigned long long)d->features, d->mac[0], d->mac[1], d->mac[2], d->mac[3],
           d->mac[4], d->mac[5], d->q[0].size, d->q[1].size, d->q[0].in_flight, d->q[1].in_flight,
           (unsigned long long)fnv(region, region_len), m->violations, m->violations ? " " : "",
           m->violations ? m->last_violation : "");
}

static void device(vnet_model *m, uint64_t feat, uint16_t qmax)
{
    memset(m, 0, sizeof *m);
    m->dev_features = feat;
    m->numq = 2;
    m->mult = 4;
    m->qmax[0] = m->qmax[1] = qmax;
    static const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    memcpy(m->config, mac, 6);
    m->trace = line;
}
static virtio_pci_caps caps_ok(uint32_t mult)
{
    virtio_pci_caps c;
    memset(&c, 0, sizeof c);
    c.common_cfg = (virtio_pci_region){1, 4, 0, 0x1000};
    c.isr_cfg = (virtio_pci_region){1, 4, 0x1000, 0x1000};
    c.device_cfg = (virtio_pci_region){1, 4, 0x2000, 0x1000};
    c.notify_cfg = (virtio_pci_region){1, 4, 0x3000, 0x1000};
    c.has_notify_off_multiplier = 1;
    c.notify_off_multiplier = mult;
    return c;
}

int main(void)
{
    const uint64_t v1 = 1ull << 32, mac = 1ull << 5, ap = 1ull << 33;
    static vnet_model m;
    vnet_dev d;
    vnet_ops ops;
    virtio_pci_caps caps = caps_ok(4);
    struct { const char *name; uint64_t feat; uint16_t qmax, qsize; uint32_t flags; int knob; } inits[] = {
        {"init normal", v1 | mac | ap | (1ull << 0) | (1ull << 17), 256, 64, 0, 0},
        {"init require-ap offered", v1 | mac | ap, 256, 16, VNET_INIT_REQUIRE_ACCESS_PLATFORM, 0},
        {"init require-ap missing", v1 | mac, 256, 16, VNET_INIT_REQUIRE_ACCESS_PLATFORM, 0},
        {"init no version 1", mac, 256, 16, 0, 0},
        {"init no mac", v1, 256, 16, 0, 0},
        {"init small device queue", v1 | mac, 8, 64, 0, 0},
        {"init queue too small", v1 | mac, 1, 64, 0, 0},
        {"init features refused", v1 | mac, 256, 16, 0, 1},
        {"init reset stuck", v1 | mac, 256, 16, 0, 2},
        {"init cfggen unstable", v1 | mac, 256, 16, 0, 3},
        {"init queue pre-enabled", v1 | mac, 256, 16, 0, 4},
        {"init one queue", v1 | mac, 256, 16, 0, 5},
    };
    for (size_t k = 0; k < sizeof inits / sizeof inits[0]; k++) {
        device(&m, inits[k].feat, inits[k].qmax);
        m.refuse_features = inits[k].knob == 1;
        m.reset_stuck = inits[k].knob == 2;
        m.cfggen_unstable = inits[k].knob == 3;
        m.preenabled = inits[k].knob == 4;
        if (inits[k].knob == 5) m.numq = 1;
        region_len = vnet_mem_size(inits[k].qsize);
        vnet_model_init(&m, region, DMA, region_len);
        ops = vnet_model_ops(&m);
        printf("== %s\n", inits[k].name);
        vnet_err e = vnet_init_flags(&d, &ops, &caps, region, DMA, region_len, inits[k].qsize, inits[k].flags);
        after(inits[k].name, e, &d, &m);
        violations += m.violations;
    }

    /* data path on a live device */
    device(&m, v1 | mac, 256);
    region_len = vnet_mem_size(16);
    vnet_model_init(&m, region, DMA, region_len);
    ops = vnet_model_ops(&m);
    printf("== data path\n");
    vnet_err e = vnet_init(&d, &ops, &caps, region, DMA, region_len, 16);
    after("init", e, &d, &m);
    uint8_t f[VNET_FRAME_MAX], out[VNET_FRAME_MAX];
    uint32_t r = 0x5eed;
    for (int i = 0; i < 40; i++) {
        size_t n = 14 + (size_t)(r % (VNET_FRAME_MAX - 13));
        for (size_t j = 0; j < n; j++) { r = r * 1103515245u + 12345u; f[j] = (uint8_t)(r >> 16); }
        char what[48];
        snprintf(what, sizeof what, "tx %d len %zu", i, n);
        after(what, vnet_tx(&d, f, n), &d, &m);
        if (i % 3 == 0) {
            printf("  device tx %u\n", vnet_model_tx(&m));
            unsigned rec = 0;
            e = vnet_tx_reclaim(&d, &rec);
            snprintf(what, sizeof what, "reclaim %u", rec);
            after(what, e, &d, &m);
        }
        if (i % 2 == 0) {
            printf("  device rx %d\n", vnet_model_rx(&m, f, n));
            size_t len = 0;
            int got = 0;
            e = vnet_rx(&d, out, sizeof out, &len, &got);
            snprintf(what, sizeof what, "rx got %d len %zu same %d", got, len, got && len == n && !memcmp(out, f, n));
            after(what, e, &d, &m);
        }
    }
    /* hostile completions: each on a fresh live device */
    for (int h = 0; h < 5; h++) {
        device(&m, v1 | mac, 256);
        vnet_model_init(&m, region, DMA, region_len);
        ops = vnet_model_ops(&m);
        printf("== hostile %d\n", h);
        after("init", vnet_init(&d, &ops, &caps, region, DMA, region_len, 16), &d, &m);
        after("tx", vnet_tx(&d, f, 60), &d, &m);
        switch (h) {
        case 0: vnet_model_push_used(&m, 0, 99, 0); break;              /* id out of range */
        case 1: vnet_model_push_used(&m, 0, 0, VNET_BUF_LEN + 1); break; /* too long */
        case 2: vnet_model_push_used(&m, 0, 0, 4); break;                /* shorter than the header */
        case 3: vnet_model_bump_used(&m, 1, 7); break;                   /* used idx runs ahead */
        default: vnet_model_push_used(&m, 1, 5, 0); break;               /* id never posted */
        }
        size_t len = 0;
        int got = 0;
        after(h == 3 || h == 4 ? "reclaim" : "rx", h == 3 || h == 4 ? vnet_tx_reclaim(&d, 0) : vnet_rx(&d, out, sizeof out, &len, &got), &d, &m);
        after("tx after", vnet_tx(&d, f, 60), &d, &m);
        violations += m.violations;
    }
    printf("VNET_TRACE: violations=%u\n", violations);
    return violations ? 1 : 0;
}
