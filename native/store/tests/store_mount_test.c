/* Mount cost of a populated sealed Store: how many device calls and unit
 * reads the kernel boot sequence (store_boot.c run_policy_ex: unkeyed
 * st_open, then ss_open) makes, and how long it takes on a file-backed disk.
 * Hosted test code only.
 *
 *   store_mount_test            print the counts, check the ceilings below
 *   store_mount_test -v         also print the timing loop
 *
 * Ceilings are the contract of the faster mount: with N objects of U units
 * stored, mount reads each object at most MOUNT_PASSES times (st_open once,
 * ss_open's record and envelope check once) plus a constant for the roots.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "disk_file.h"
#include "store_sealed.h"
#include "store_test_util.h"

#define ANCHOR_UNITS 4u
#define STORE_UNITS 4096u
#define TXNS 40u

typedef struct {
    st_dev real;
    uint64_t read_calls, read_units, write_calls, write_units, flushes;
} counting;

static int c_read(void *ctx, uint64_t unit, uint8_t out[SV1_UNIT])
{
    counting *c = ctx;
    c->read_calls++;
    c->read_units++;
    return c->real.read_unit(c->real.ctx, unit, out);
}
static int c_write(void *ctx, uint64_t unit, const uint8_t in[SV1_UNIT])
{
    counting *c = ctx;
    c->write_calls++;
    c->write_units++;
    return c->real.write_unit(c->real.ctx, unit, in);
}
static int c_flush(void *ctx)
{
    counting *c = ctx;
    c->flushes++;
    return c->real.flush(c->real.ctx);
}

static ss_workspace g_ws;
static ss_store g_ss;
static ss_keys K;
static const uint8_t UUID[16] = {0x5e, 0xa1, 0xed, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* The kernel boot sequence, as store_boot.c run_policy_ex runs it. */
static int kernel_mount(const st_dev *sdev, const ts_device *tdev)
{
    int rc = st_open(&g_ss.st, sdev, &g_ws.st);
    if (rc) return rc;
    memset(&g_ss, 0, sizeof g_ss);
    return ss_open(&g_ss, sdev, tdev, 0, &K, &g_ws);
}

int main(int argc, char **argv)
{
    int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    char path[] = "/tmp/store_mount_test.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 2;
    close(fd);
    unlink(path);

    K.identity_class = M5_ID_TEST;
    K.key_generation = 1;
    for (int i = 0; i < 32; i++) {
        K.k_root_auth[i] = (uint8_t)(i * 3 + 1);
        K.k_domain[i] = (uint8_t)(i * 5 + 7);
    }

    disk_file f;
    disk_dev d;
    st_disk sd;
    st_dev sdev;
    ts_device tdev;
    CHECK(disk_file_open(&f, &d, path, 4096, ANCHOR_UNITS + STORE_UNITS, 1) == 0, "disk create");
    CHECK(st_disk_bind(&sd, &d, ANCHOR_UNITS, STORE_UNITS, &sdev) == 0, "bind");
    ss_ts_device(&d, &tdev);
    CHECK_EQ(ss_format(&sdev, &tdev, 0, UUID, &K, &g_ws), 0, "format");
    CHECK_EQ(ss_open(&g_ss, &sdev, &tdev, 0, &K, &g_ws), 0, "open genesis");

    static uint8_t payload[3][16384];
    for (uint32_t t = 0; t < TXNS; t++) {
        ss_object o[3];
        for (int j = 0; j < 3; j++) {
            for (size_t i = 0; i < sizeof payload[j]; i++) payload[j][i] = (uint8_t)(i * 7 + j * 13 + t * 31);
            o[j].kind = 40;
            o[j].version = 1;
            o[j].bytes = payload[j];
            o[j].len = j == 0 ? 16384 : 700 * (size_t)(j + 1);
        }
        CHECK_EQ(ss_transact(&g_ss, o, 3, NULL, NULL, NULL), 0, "transact");
    }
    uint32_t n;
    const sv1_entry *cat = st_catalog(&g_ss.st, &n);
    uint64_t stored_units = 0;
    for (uint32_t i = 0; i < n; i++) stored_units += cat[i].unit_count;

    counting c;
    memset(&c, 0, sizeof c);
    c.real = sdev;
    st_dev cdev = sdev;
    cdev.ctx = &c;
    cdev.read_unit = c_read;
    cdev.write_unit = c_write;
    cdev.flush = c_flush;

    CHECK_EQ(kernel_mount(&cdev, &tdev), 0, "mount");
    printf("store: %u objects, %llu object units, generation %llu\n", n,
           (unsigned long long)stored_units, (unsigned long long)st_generation(&g_ss.st));
    printf("mount: %llu read calls, %llu units read, %.2f passes over the object units\n",
           (unsigned long long)c.read_calls, (unsigned long long)c.read_units,
           (double)c.read_units / (double)stored_units);
    printf("RECEIPT mount_reads_calls=%llu mount_reads_units=%llu object_units=%llu\n",
           (unsigned long long)c.read_calls, (unsigned long long)c.read_units,
           (unsigned long long)stored_units);

    /* ceiling: two passes (st_open once, verify_records once) plus the
     * roots, catalogs, commit units and anchor, which are a few percent. */
    CHECK(c.read_units * 100 <= stored_units * 260, "mount reads %llu units for %llu object units (> 2.6 passes)",
          (unsigned long long)c.read_units, (unsigned long long)stored_units);

    if (verbose) {
        enum { REP = 20 };
        double t0 = now();
        for (int i = 0; i < REP; i++) CHECK_EQ(kernel_mount(&cdev, &tdev), 0, "mount rep");
        double dt = (now() - t0) / REP;
        printf("RECEIPT mount_seconds=%.6f (mean of %d, file-backed, page cache warm)\n", dt, REP);
    }

    disk_file_close(&f);
    unlink(path);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
