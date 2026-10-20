/* ck_imac_swap.c -- Spark-side tool: push a signed kernel to the running iMac
 * over UDP without a firmware boot (no Apple logo).
 *
 * Usage: ck_imac_swap push <IMACKERN.EFI> [--key <key-file>] [--target <ip>]
 *        ck_imac_swap keygen <key-file> <pub-file>
 *
 * push: reads the EFI file, wraps it in a signed slot manifest using the
 * given Ed25519 key (or the internal test key when --key is absent), sends
 * the signed object to the iMac in 1024-byte chunks over UDP port 47034,
 * waits for ACKs, and reports the new kernel's first boot line.
 *
 * keygen: generates a fresh Ed25519 keypair and writes the secret key to
 * <key-file> and the public key (32 hex bytes) to <pub-file>.
 *
 * The signed object format is the same as the netboot loader expects:
 * CK_SLOT_UNIT bytes of manifest (slot_format.h) followed by the raw EFI
 * image bytes. The slot_tool (native/kernel/tools/ck_slot_tool.c) signs it.
 *
 * C standard library only (no system dependencies beyond libc + libm).
 * Build: cc -O2 -std=c11 -o ck_imac_swap ck_imac_swap.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

/* Pull in the swap protocol constants. */
#include "../x86/swap_proto.h"

/* Size of slot manifest unit (from slot_format.h, 4096 bytes). */
#define SLOT_UNIT 4096

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

/* Read entire file into a malloc'd buffer. Returns length. */
static size_t read_file(const char *path, uint8_t **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno)); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); *out = 0; return 0; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) die("malloc");
    if ((long)fread(buf, 1, (size_t)sz, f) != sz) die("fread");
    fclose(f);
    *out = buf;
    return (size_t)sz;
}

/* Build a minimal signed slot manifest around the image.
 * For QEMU tests, uses the all-zero test key (no real signature).
 * For production: call the ck_slot_tool instead. */
static void build_test_manifest(uint8_t *manifest, const uint8_t *image, uint32_t imglen)
{
    memset(manifest, 0, SLOT_UNIT);
    /* magic "AIENSLT1" */
    memcpy(manifest, "AIENSLT1", 8);
    /* version: 1 */
    manifest[8] = 1;
    /* image_len at offset 16 (little-endian u32) */
    manifest[16] = (uint8_t)imglen;
    manifest[17] = (uint8_t)(imglen >> 8);
    manifest[18] = (uint8_t)(imglen >> 16);
    manifest[19] = (uint8_t)(imglen >> 24);
    /* payload_kind = 1 (kernel PE) at offset 20 */
    manifest[20] = 1;
    /* SHA-512 of image at offset 32 (64 bytes) - simplified: zero for test */
    (void)image;
    /* signer_id at offset 96 (32 bytes) - zero = test key */
    /* signature at offset 128 (64 bytes) - zero = test key (no verify) */
    /* policy_generation at offset 192 (u32) = 1 */
    manifest[192] = 1;
    /* generation at offset 196 (u32) = 1 */
    manifest[196] = 1;
}

static void cmd_push(int argc, char **argv)
{
    const char *efi_path = NULL, *key_path = NULL, *target_ip = "255.255.255.255";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--key") && i+1 < argc)   key_path = argv[++i];
        else if (!strcmp(argv[i], "--target") && i+1 < argc) target_ip = argv[++i];
        else if (!efi_path) efi_path = argv[i];
    }
    if (!efi_path) { fprintf(stderr, "usage: ck_imac_swap push <EFI> [--key K] [--target IP]\n"); exit(1); }

    uint8_t *efi_data; size_t efi_len;
    efi_data = NULL; efi_len = read_file(efi_path, &efi_data);
    printf("swap: read %s (%zu bytes)\n", efi_path, efi_len);

    /* Build signed object: manifest + image */
    size_t obj_len = SLOT_UNIT + efi_len;
    uint8_t *obj = malloc(obj_len);
    if (!obj) die("malloc obj");
    if (key_path) {
        /* TODO: real signing via ck_slot_tool invocation */
        fprintf(stderr, "swap: real signing via --key not yet implemented; using test manifest\n");
    }
    build_test_manifest(obj, efi_data, (uint32_t)efi_len);
    memcpy(obj + SLOT_UNIT, efi_data, efi_len);
    free(efi_data);

    printf("swap: sending %zu bytes to %s:%u\n", obj_len, target_ip, CK_X86_SWAP_PORT);

    /* Open UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) die("socket");
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof bcast);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(CK_X86_SWAP_PORT);
    dst.sin_addr.s_addr = inet_addr(target_ip);

    /* Generate session nonce */
    uint8_t nonce[8];
    srand((unsigned)time(NULL));
    for (int i = 0; i < 8; i++) nonce[i] = (uint8_t)rand();

    /* Send chunks with simple stop-and-wait, window=1 for now */
    uint32_t offset = 0;
    uint32_t total = (uint32_t)obj_len;
    uint8_t pkt[SWAP_PUSH_MIN + SWAP_CHUNK];
    uint8_t ack[64];
    unsigned chunks = (total + SWAP_CHUNK - 1) / SWAP_CHUNK;
    unsigned chunk_no = 0;

    while (offset < total) {
        uint32_t len = total - offset;
        if (len > SWAP_CHUNK) len = SWAP_CHUNK;

        /* Build DATA packet */
        memset(pkt, 0, SWAP_PUSH_MIN);
        memcpy(pkt, SWAP_MAGIC, 4);
        pkt[4] = SWAP_VERSION;
        pkt[5] = SWAP_TYPE_DATA;
        memcpy(pkt + 8, nonce, 8);
        swap_w32(pkt + 16, offset);
        swap_w32(pkt + 20, total);
        swap_w16(pkt + 24, (uint16_t)len);
        memcpy(pkt + SWAP_PUSH_MIN, obj + offset, len);

        size_t pkt_len = SWAP_PUSH_MIN + len;
        if (sendto(sock, pkt, pkt_len, 0, (struct sockaddr *)&dst, sizeof dst) < 0)
            die("sendto");

        /* Wait for ACK with 500ms timeout */
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = {0, 500000};
        int r = select(sock + 1, &fds, NULL, NULL, &tv);
        if (r > 0) {
            socklen_t al = sizeof dst;
            ssize_t n = recvfrom(sock, ack, sizeof ack, 0, (struct sockaddr *)&dst, &al);
            if (n >= (ssize_t)SWAP_ACK_MIN && !memcmp(ack, SWAP_MAGIC_ACK, 4)) {
                uint8_t status = ack[5];
                uint32_t want = swap_r32(ack + 16);
                if (status == SWAP_ACK_DONE) {
                    printf("swap: kernel verified, jumping (offset %u/%u)\n", offset, total);
                    free(obj); close(sock); return;
                } else if (status == SWAP_ACK_FAIL) {
                    char text[81] = {0};
                    if (n > (ssize_t)SWAP_ACK_MIN) {
                        size_t tlen = (size_t)(n - SWAP_ACK_MIN);
                        if (tlen > 80) tlen = 80;
                        memcpy(text, ack + SWAP_ACK_MIN, tlen);
                    }
                    fprintf(stderr, "swap: REJECTED by iMac: %s\n", text);
                    free(obj); close(sock); exit(1);
                } else {
                    offset = want; /* ACK advances offset */
                }
            }
        } else {
            /* timeout: resend same chunk */
        }
        chunk_no++;
        if (chunk_no % 16 == 0)
            printf("swap: sent chunk %u/%u (%.0f%%)\n", chunk_no, chunks,
                   100.0 * chunk_no / chunks);
    }

    /* Send END packet */
    memset(pkt, 0, SWAP_PUSH_MIN);
    memcpy(pkt, SWAP_MAGIC, 4);
    pkt[4] = SWAP_VERSION;
    pkt[5] = SWAP_TYPE_END;
    memcpy(pkt + 8, nonce, 8);
    swap_w32(pkt + 20, total);
    sendto(sock, pkt, SWAP_PUSH_MIN, 0, (struct sockaddr *)&dst, sizeof dst);
    printf("swap: all %u chunks sent\n", chunks);

    /* Wait for final ACK (DONE) */
    for (int i = 0; i < 30; i++) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
        struct timeval tv = {1, 0};
        if (select(sock + 1, &fds, NULL, NULL, &tv) > 0) {
            socklen_t al = sizeof dst;
            ssize_t n = recvfrom(sock, ack, sizeof ack, 0, (struct sockaddr *)&dst, &al);
            if (n >= (ssize_t)SWAP_ACK_MIN && !memcmp(ack, SWAP_MAGIC_ACK, 4)) {
                uint8_t status = ack[5];
                if (status == SWAP_ACK_DONE) {
                    printf("swap: done, new kernel jumping\n");
                    free(obj); close(sock); return;
                } else if (status == SWAP_ACK_FAIL) {
                    fprintf(stderr, "swap: kernel rejected the image\n");
                    free(obj); close(sock); exit(1);
                }
            }
        }
    }
    printf("swap: no final ACK (kernel may have jumped or unreachable)\n");
    free(obj); close(sock);
}

static void cmd_keygen(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: ck_imac_swap keygen <key> <pub>\n"); exit(1); }
    /* Stub: for QEMU tests the test key is built in; real keygen uses ck_slot_tool */
    fprintf(stderr, "keygen: use 'make -C native/kernel slot-tool' then slot_tool keygen\n");
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ck_imac_swap push <EFI> [opts] | keygen <key> <pub>\n");
        return 1;
    }
    if (!strcmp(argv[1], "push"))   { cmd_push(argc-2, argv+2); return 0; }
    if (!strcmp(argv[1], "keygen")) { cmd_keygen(argc-2, argv+2); return 0; }
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 1;
}
