/* ipj_tool -- owner-side tool for the Interplane join (hosted C, libc only).
 *
 *   ipj_tool fixture <dir> [run_seconds]
 *       QEMU qualification fixture. Fresh keys from /dev/urandom every run:
 *       one owner key, machine keys for spark, pi, mac (members), old (listed
 *       at generation 4, revoked at generation 5) and tst (a TEST-kind entry).
 *       Writes <dir>/<name>.bin provisioning bundles (aienos_ipj.h):
 *         spark, pi, mac  roster generation 5, production mode; spark sends one
 *                          signed message to mac with the route mac via pi
 *         old              roster generation 4 (from before its revocation)
 *         tst              a generation-1 roster holding a TEST entry: a
 *                          production node must refuse to join with it
 *       and <dir>/roster5.bin (the signed generation-5 roster), <dir>/names.txt
 *       ("name machine_pk_hex" per line). The owner secret key is never
 *       written; it lives only in this process.
 *
 * Exit 0 on success, 1 on any error. QUALIFICATION ONLY: bundles hold machine
 * secret keys in the clear (docs/INTERPLANE_JOIN.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../aienos_ipj.h"
#include "../../crypto/aienos_crypto.h"
#include "../../sig/aienos_sig.h"

static int rnd(uint8_t *b, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t r = fread(b, 1, n, f);
    fclose(f);
    return r == n ? 0 : -1;
}

static int write_file(const char *dir, const char *name, const uint8_t *b, size_t n)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); return -1; }
    int ok = fwrite(b, 1, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

enum { SPARK, PI, MAC, OLD, TST, N };
static const char *names[N] = {"spark", "pi", "mac", "old", "tst"};
static const uint8_t plats[N] = {IPJ_PLAT_SPARK, IPJ_PLAT_PI, IPJ_PLAT_RUN_MAC, IPJ_PLAT_RUN_WIN, IPJ_PLAT_RUN_LNX};

static int fixture(const char *dir, unsigned run_seconds)
{
    static uint8_t osk[32], opk[32], sk[N][32], pk[N][32];
    static ipj_roster r, t;
    static uint8_t r4[IPJ_ROSTER_MAX_BYTES], r5[IPJ_ROSTER_MAX_BYTES], rt[IPJ_ROSTER_MAX_BYTES], out[IPJ_BUNDLE_MAX];
    size_t n4 = 0, n5 = 0, nt = 0, w = 0;
    if (rnd(osk, 32) || aienos_ed25519_public_key(opk, osk) != AIENOS_SIG_OK) return -1;
    for (int i = 0; i < N; i++)
        if (rnd(sk[i], 32) || aienos_ed25519_public_key(pk[i], sk[i]) != AIENOS_SIG_OK) return -1;
    ipj_roster_init(&r, opk);
    for (int i = SPARK; i <= OLD; i++)
        if (ipj_roster_add(&r, pk[i], names[i], IPJ_KIND_PROD, plats[i]) != IPJ_OK) return -1;
    if (ipj_roster_sign(&r, osk, r4, sizeof r4, &n4) != IPJ_OK) return -1;
    if (ipj_roster_revoke(&r, pk[OLD]) != IPJ_OK || ipj_roster_sign(&r, osk, r5, sizeof r5, &n5) != IPJ_OK) return -1;
    ipj_roster_init(&t, opk);
    if (ipj_roster_add(&t, pk[TST], names[TST], IPJ_KIND_TEST, plats[TST]) != IPJ_OK ||
        ipj_roster_sign(&t, osk, rt, sizeof rt, &nt) != IPJ_OK)
        return -1;
    aienos_wipe(osk, sizeof osk);
    for (int i = 0; i < N; i++) {
        static ipj_bundle b;
        memset(&b, 0, sizeof b);
        b.mode = IPJ_MODE_PRODUCTION;
        memcpy(b.sk, sk[i], 32);
        memcpy(b.owner_pk, opk, 32);
        b.ip[0] = 10; b.ip[1] = 47; b.ip[2] = 40; b.ip[3] = (uint8_t)(11 + i);
        b.bcast[0] = 10; b.bcast[1] = 47; b.bcast[2] = 40; b.bcast[3] = 255;
        b.run_seconds = (uint16_t)run_seconds;
        if (i == SPARK) {
            static const char msg[] = "hello mac, this is the spark, relayed by the pi";
            memcpy(b.send_dest, pk[MAC], 32);
            b.send_len = (uint16_t)(sizeof msg - 1);
            memcpy(b.send_text, msg, sizeof msg - 1);
            b.nroutes = 1;
            memcpy(b.route_dest[0], pk[MAC], 32);
            memcpy(b.route_via[0], pk[PI], 32);
        }
        const uint8_t *ro = i == OLD ? r4 : i == TST ? rt : r5;
        size_t rn = i == OLD ? n4 : i == TST ? nt : n5;
        char fn[32];
        snprintf(fn, sizeof fn, "%s.bin", names[i]);
        if (ipj_bundle_build(&b, ro, rn, out, sizeof out, &w) != IPJ_OK || write_file(dir, fn, out, w)) return -1;
        aienos_wipe(&b, sizeof b);
        aienos_wipe(out, sizeof out);
        aienos_wipe(sk[i], 32);
    }
    if (write_file(dir, "roster5.bin", r5, n5)) return -1;
    char p[1024];
    snprintf(p, sizeof p, "%s/names.txt", dir);
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    for (int i = 0; i < N; i++) {
        fprintf(f, "%s ", names[i]);
        for (int k = 0; k < 32; k++) fprintf(f, "%02x", pk[i][k]);
        fprintf(f, "\n");
    }
    fprintf(f, "owner ");
    for (int k = 0; k < 32; k++) fprintf(f, "%02x", opk[k]);
    fprintf(f, "\n");
    return fclose(f) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "fixture") == 0) {
        unsigned rs = argc >= 4 ? (unsigned)strtoul(argv[3], 0, 10) : 90u;
        if (rs == 0 || rs > 3600) { fprintf(stderr, "run_seconds must be 1..3600\n"); return 1; }
        if (fixture(argv[2], rs) != 0) { fprintf(stderr, "ipj_tool: fixture failed\n"); return 1; }
        printf("ipj_tool: fixture written to %s (owner key not kept)\n", argv[2]);
        return 0;
    }
    fprintf(stderr, "usage: ipj_tool fixture <dir> [run_seconds]\n");
    return 1;
}
