/* Reuse controls supplement the retained RFC8452 vectors, not a security proof. */
static void test_reused_nonce_siv_samples(void)
{
    uint8_t key[32], nonce[12], p[32], q[32], a[48], b[48], repeat[48], out[32];
    memset(key, 7, sizeof key); memset(nonce, 9, sizeof nonce);
    memset(p, 3, sizeof p); memset(q, 10, sizeof q);
    CHECK(aienos_gcmsiv_seal(key, nonce, NULL, 0, p, 32, a, 48) == 0, "SIV repeated nonce seal a");
    CHECK(aienos_gcmsiv_seal(key, nonce, NULL, 0, q, 32, b, 48) == 0, "SIV repeated nonce seal b");
    CHECK(aienos_gcmsiv_seal(key, nonce, NULL, 0, p, 32, repeat, 48) == 0, "SIV repeated exact input");
    CHECK(memcmp(a, repeat, 48) == 0, "SIV deterministic repeated input reveals equality");
    int xor_equal = 1;
    for (size_t i = 0; i < 32; i++) xor_equal &= (a[i] ^ b[i]) == 9;
    CHECK(!xor_equal, "SIV synthetic sample has no simple payload XOR disclosure (not proof)");
    CHECK(aienos_gcmsiv_open(key, nonce, NULL, 0, a, 48, out, 32) == 0 && memcmp(out, p, 32) == 0, "SIV repeated nonce valid open");
    a[47] ^= 1;
    CHECK(aienos_gcmsiv_open(key, nonce, NULL, 0, a, 48, out, 32) == AIENOS_CRYPTO_ERR_AUTH, "SIV authentication independently refuses changed tag");
    CHECK(all_byte(out, sizeof out, 0), "SIV bad tag leaves zero output");
}
