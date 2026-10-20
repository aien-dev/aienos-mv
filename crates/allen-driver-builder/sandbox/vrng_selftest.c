/* vrng_selftest.c -- the class test every entropy-poll driver must pass in
 * Allen's sandbox (crates/allen-driver-builder/src/sandbox.rs, L268).
 *
 * Hand-written. Never generated and never written by a language model: it is
 * the fixed bar a model-written contract has to clear. It drives the generated
 * driver (vrng_contract.c) against a chip model (vrng_model.c) -- the one
 * generated from the candidate contract, or, in the reference step, the one
 * generated from a reference contract the model never saw.
 *
 * Cases (each prints PASS/FAIL; exit 0 only when all pass):
 *   1 bring-up on a well-behaved device with ACCESS_PLATFORM required: OK,
 *     zero protocol violations, the device ends DRIVER_OK
 *   2 data path: 200 fills of distinct patterns across ring wrap-around come
 *     back byte-exact through vrng_read, never more than asked for
 *   3 a device that does not offer ACCESS_PLATFORM is refused when the flag
 *     requires it, and the device is left FAILED or reset (never DRIVER_OK)
 *   4 a device that refuses FEATURES_OK is refused
 *   5 a stuck reset is refused (bounded, no hang)
 *   6 a hostile used entry (id outside the ring) is an error, never a crash
 *     or an out-of-region copy (ASan/UBSan watch)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aienos_virtio_rng.h"
#include "vrng_model.h"

#define DMA 0x40000000ull
#define QSIZE 8u
static uint8_t region[1u << 16] __attribute__((aligned(4096)));
static int fails;

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL  ");                                                                                          \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static virtio_pci_caps caps_for(uint32_t mult)
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

static vrng_model m;
static vrng_dev d;
static vrng_ops ops;

static vrng_err bring_up(uint64_t offer, uint32_t flags)
{
    memset(&m, 0, sizeof m);
    memset(region, 0, sizeof region);
    m.dev_features = offer;
    m.numq = 1;
    m.qmax[0] = 16;
    m.mult = 4;
    size_t need = vrng_mem_size(QSIZE);
    if (need == 0 || need > sizeof region) return VRNG_E_ARG;
    vrng_model_init(&m, region, DMA, need);
    ops = vrng_model_ops(&m);
    virtio_pci_caps caps = caps_for(m.mult);
    return vrng_init_flags(&d, &ops, &caps, region, DMA, need, QSIZE, flags);
}

#define S_DRIVER_OK 4u
#define S_FAILED 128u
#define F_VERSION_1 (1ull << 32)
#define F_ACCESS_PLATFORM (1ull << 33)

int main(void)
{
    /* 1 bring-up */
    vrng_err e = bring_up(F_VERSION_1 | F_ACCESS_PLATFORM, VRNG_INIT_REQUIRE_ACCESS_PLATFORM);
    CHECK(e == VRNG_OK, "1 bring-up returned %s", vrng_err_name(e));
    CHECK(m.violations == 0, "1 protocol violations=%u (%s)", m.violations, m.last_violation);
    CHECK(vrng_model_status(&m) & S_DRIVER_OK, "1 device status 0x%x lacks DRIVER_OK", vrng_model_status(&m));
    CHECK((d.features >> VRNG_F_ACCESS_PLATFORM) & 1, "1 ACCESS_PLATFORM not accepted");
    if (!fails) printf("PASS  1 bring-up, ACCESS_PLATFORM accepted, no protocol violations\n");

    /* 2 data path */
    int f0 = fails;
    if (e == VRNG_OK) {
        for (unsigned round = 0; round < 200; round++) {
            uint8_t want[64], got[64 + 8];
            for (unsigned i = 0; i < sizeof want; i++) want[i] = (uint8_t)(round * 31u + i * 7u + 1u);
            size_t n = 16 + round % 49; /* 16..64 bytes per fill */
            size_t put = vrng_model_fill(&m, want, n);
            CHECK(put == n, "2 round %u: device had no posted buffer (put %zu of %zu)", round, put, n);
            memset(got, 0xee, sizeof got);
            size_t have = 0, g = 0;
            for (int tries = 0; tries < 4 && have < n; tries++) {
                g = 0;
                e = vrng_read(&d, got + have, n - have, &g);
                if (e != VRNG_OK) break;
                have += g;
            }
            CHECK(e == VRNG_OK, "2 round %u: vrng_read returned %s", round, vrng_err_name(e));
            CHECK(have == n && memcmp(got, want, n) == 0, "2 round %u: got %zu bytes, want %zu exact", round, have, n);
            CHECK(got[n] == 0xee, "2 round %u: wrote past the requested length", round);
            if (fails > f0) break;
        }
        CHECK(m.violations == 0, "2 protocol violations=%u (%s)", m.violations, m.last_violation);
    }
    if (fails == f0) printf("PASS  2 200 fills byte-exact across ring wrap-around\n");

    /* 3 no ACCESS_PLATFORM offered, but required */
    f0 = fails;
    e = bring_up(F_VERSION_1, VRNG_INIT_REQUIRE_ACCESS_PLATFORM);
    CHECK(e != VRNG_OK, "3 a device without ACCESS_PLATFORM was accepted");
    CHECK(!(vrng_model_status(&m) & S_DRIVER_OK), "3 device reached DRIVER_OK");
    if (fails == f0) printf("PASS  3 refused a device without ACCESS_PLATFORM (%s)\n", vrng_err_name(e));

    /* 4 the device refuses FEATURES_OK */
    f0 = fails;
    memset(&m, 0, sizeof m);
    e = bring_up(F_VERSION_1 | F_ACCESS_PLATFORM, 0);
    CHECK(e == VRNG_OK, "4 (setup) plain bring-up returned %s", vrng_err_name(e));
    memset(&m, 0, sizeof m);
    m.refuse_features = 1;
    m.dev_features = F_VERSION_1 | F_ACCESS_PLATFORM;
    m.numq = 1;
    m.qmax[0] = 16;
    m.mult = 4;
    {
        size_t need = vrng_mem_size(QSIZE);
        vrng_model_init(&m, region, DMA, need);
        ops = vrng_model_ops(&m);
        virtio_pci_caps caps = caps_for(4);
        e = vrng_init_flags(&d, &ops, &caps, region, DMA, need, QSIZE, 0);
    }
    CHECK(e != VRNG_OK, "4 a device refusing FEATURES_OK was accepted");
    if (fails == f0) printf("PASS  4 refused a device that refuses FEATURES_OK (%s)\n", vrng_err_name(e));

    /* 5 stuck reset */
    f0 = fails;
    memset(&m, 0, sizeof m);
    m.reset_stuck = 1;
    m.dev_features = F_VERSION_1 | F_ACCESS_PLATFORM;
    m.numq = 1;
    m.qmax[0] = 16;
    m.mult = 4;
    {
        size_t need = vrng_mem_size(QSIZE);
        vrng_model_init(&m, region, DMA, need);
        ops = vrng_model_ops(&m);
        virtio_pci_caps caps = caps_for(4);
        e = vrng_init_flags(&d, &ops, &caps, region, DMA, need, QSIZE, 0);
    }
    CHECK(e != VRNG_OK, "5 a device whose reset never completes was accepted");
    if (fails == f0) printf("PASS  5 refused a stuck reset (%s)\n", vrng_err_name(e));

    /* 6 hostile used entry */
    f0 = fails;
    e = bring_up(F_VERSION_1 | F_ACCESS_PLATFORM, 0);
    CHECK(e == VRNG_OK, "6 (setup) bring-up returned %s", vrng_err_name(e));
    if (e == VRNG_OK) {
        vrng_model_push_used(&m, 0, 0xffffu, 64);
        uint8_t got[64];
        size_t g = 0;
        e = vrng_read(&d, got, sizeof got, &g);
        CHECK(e != VRNG_OK, "6 a used entry with id 0xffff was accepted");
        CHECK(g == 0, "6 bytes were returned from a hostile entry");
    }
    if (fails == f0) printf("PASS  6 refused a hostile used entry (%s)\n", vrng_err_name(e));

    printf("vrng_selftest: %s (%d failed checks)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
