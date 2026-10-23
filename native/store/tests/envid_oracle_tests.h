/* #445: independent expected bytes for actual ss_prepare output.
 * Fixed synthetic domain key 0x23*32; no call to envid_derive in the oracle. */
#include "envid_expected.h"
static void test_independent_envid_fixture(void)
{
    uint8_t ids[2][32];
    make_gen2(ids);
    for (unsigned c = 0; c < 9; c++) {
        rig r;
        ss_store s;
        SCHECK_EQ(sopen(&r, &s, &K), 0, "oracle open");
        memset(s.keys.k_domain, 0x23, 32);
        if (c == 1) s.uuid[0] ^= 1;
        if (c == 2) s.st.root.commit.generation = 3;
        if (c == 6) s.keys.key_generation = 2;
        if (c == 7) s.keys.identity_class = M5_ID_TEST;
        const uint8_t *pt = (const uint8_t *)(c == 8 ? "synthetic!" : "synthetic");
        ss_object o[2] = {
            {.kind = c == 4 ? 43 : 42, .version = c == 5 ? 2 : 1, .bytes = pt, .len = c == 8 ? 10 : 9},
            {.kind = 42, .version = 1, .bytes = (const uint8_t *)"synthetic", .len = 9}
        };
        SCHECK_EQ(ss_prepare(&s, o, c == 3 ? 2 : 1), 0, "oracle prepare");
        const uint8_t *env = g_ws.env[c == 3 ? 1 : 0];
        uint8_t pair[24];
        memcpy(pair, env + 28, 16); memcpy(pair + 16, env + 44, 8);
        CHECK(memcmp(pair, expected_pair[c], 24) == 0, "independent ENVID oracle case %u", c);
        rig_close(&r);
    }
}
