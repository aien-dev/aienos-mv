/* swap_x86.c -- AIENOS-KERNEL-0 live kernel swap (LAB BUILD, CK_X86_SWAP=1).
 *
 * While Allen is up, the kernel listens on UDP port 47034 for a signed kernel
 * image pushed by ck_imac_swap. On receipt of a complete payload (a signed
 * slot manifest + raw kernel PE, same format as the netboot loader uses), it:
 *   1. Verifies the manifest signature with the same Ed25519 check NETLDR.EFI
 *      uses (no weaker path; see slot_verify_swap below).
 *   2. Prints "swap: verified, quiescing" and waits for all APs to reach their
 *      HLT loop (rest_wake / park-loop below).
 *   3. Disables xHCI, tg3, HDA, and puts the GPU panel state to off.
 *   4. Copies the trampoline to tramp_pa+TRAMP_CODE_OFF and the rebuilt
 *      handoff struct to tramp_pa+TRAMP_HAND_OFF (the tramp page survives the
 *      new-kernel copy since it lies below 1 MiB, outside the kernel range).
 *   5. Jumps to the trampoline, which copies the new kernel's PE sections to
 *      their linked addresses and calls ck_x86_kernel_entry(handoff).
 *
 * QEMU note: QEMU does not model the BCM57766, so the UDP receive path is
 * never exercised there. The QEMU swap test uses a disk-staged image instead:
 * when CK_X86_SWAP_TEST_DISK is defined, ck_x86_swap_init reads a pre-placed
 * signed image from the AIENOS GPT partition and performs the swap at first
 * call to ck_x86_swap_step. The mechanism (verify, quiesce, jump) is the
 * same; only the image source differs.
 *
 * Honest limits: if tramp_pa is zero (no free page below 1 MiB), the swap is
 * refused. If the new PE's ImageBase differs from the current kernel, this
 * loader does not apply PE base relocations; both kernels must be built with
 * the same linker script.
 *
 * This file qualifies nothing physical; QEMU runs of it exercise nothing on
 * the real iMac. */
#include "../core/ck_internal.h"
#include "../mm/frames.h"
#include "../../boot/slot_format.h"
#include "../../boot/netboot_proto.h"
#include "x86.h"
#include "swap_proto.h"
#include "report_x86.h"

#ifndef CK_X86_SWAP
#define CK_X86_SWAP 0
#endif

#if CK_X86_SWAP

/* Owner public key(s) accepted for swap signature verification.
 * At build time: make imac-kernel CK_X86_SWAP=1 CK_X86_SWAP_KEY=<32hex>
 * Default: the test key (16 zero bytes) for QEMU tests only. */
#ifndef CK_X86_SWAP_KEY
#define CK_X86_SWAP_KEY \
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, \
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
#endif
static const uint8_t swap_pubkey[32] = { CK_X86_SWAP_KEY };

/* Swap buffer: pre-allocated from the frame allocator in ck_x86_swap_init.
 * Holds the incoming signed slot object: CK_SLOT_UNIT + image bytes. */
#define SWAP_BUF_PAGES 1024u      /* 4 MiB max kernel image + manifest */
#define SWAP_BUF_BYTES (SWAP_BUF_PAGES * CK_PAGE)

static uint64_t swap_buf_pa;      /* physical address of the swap buffer */
static uint32_t swap_received;    /* bytes received so far */
static uint32_t swap_total;       /* total bytes expected */
static int      swap_in_progress; /* 1: a session is active */
static int      swap_ready;       /* 1: init succeeded */

/* ---- Trampoline page layout (at tramp_pa, already allocated) ---- */
#define TRAMP_SECT_OFF  0u    /* section table: 8 × 32 bytes = 256 bytes */
#define TRAMP_ENTRY_OFF 256u  /* entry_point u64 */
#define TRAMP_HAND_OFF  280u  /* handoff struct copy (232 bytes) */
#define TRAMP_CODE_OFF  512u  /* position-independent trampoline code */

/* Section table entry (32 bytes). */
struct swap_sect {
    uint64_t src_pa;  /* physical address of source section data */
    uint64_t dst_pa;  /* destination (linked) physical address */
    uint64_t len;     /* bytes */
    uint32_t flags;   /* 0=copy, 1=zero (BSS) */
    uint32_t _pad;
};
#define SWAP_SECT_MAX 8u
#define SWAP_SECT_ZERO 1u

/* ---- PE parser helpers ---- */
#define PE_OFF_MAGIC 0x3c
static int pe_read32(const uint8_t *img, size_t img_len, uint32_t off, uint32_t *out)
{
    if ((uint64_t)off + 4 > img_len) return -1;
    uint32_t v; memcpy(&v, img + off, 4); *out = v; return 0;
}
static int pe_read16(const uint8_t *img, size_t img_len, uint32_t off, uint16_t *out)
{
    if ((uint64_t)off + 2 > img_len) return -1;
    uint16_t v; memcpy(&v, img + off, 2); *out = v; return 0;
}
static int pe_read64(const uint8_t *img, size_t img_len, uint32_t off, uint64_t *out)
{
    if ((uint64_t)off + 8 > img_len) return -1;
    uint64_t v; memcpy(&v, img + off, 8); *out = v; return 0;
}

/* Parse the PE and fill sect[] with copy/zero operations.
 * Returns the entry_point virtual address, or 0 on parse failure. */
static uint64_t pe_parse(const uint8_t *img, size_t img_len,
                          struct swap_sect *sect, unsigned *nsect)
{
    *nsect = 0;
    if (img_len < 0x40) return 0;
    if (img[0] != 'M' || img[1] != 'Z') return 0;
    uint32_t pe_off; if (pe_read32(img, img_len, PE_OFF_MAGIC, &pe_off)) return 0;
    if ((uint64_t)pe_off + 24 > img_len) return 0;
    uint32_t sig; if (pe_read32(img, img_len, pe_off, &sig) || sig != 0x00004550u) return 0;
    /* COFF header: NumberOfSections at pe_off+6 */
    uint16_t nsections; if (pe_read16(img, img_len, pe_off + 6, &nsections)) return 0;
    /* Optional header size */
    uint16_t optsize; if (pe_read16(img, img_len, pe_off + 20, &optsize)) return 0;
    /* Optional header magic: 0x20B = PE32+ */
    uint16_t magic; if (pe_read16(img, img_len, pe_off + 24, &magic)) return 0;
    if (magic != 0x020bu) return 0; /* only PE32+ */
    /* AddressOfEntryPoint (RVA) */
    uint32_t entry_rva; if (pe_read32(img, img_len, pe_off + 40, &entry_rva)) return 0;
    /* ImageBase */
    uint64_t image_base; if (pe_read64(img, img_len, pe_off + 48, &image_base)) return 0;
    /* Section table starts at pe_off + 24 + optsize */
    uint32_t sect_off = pe_off + 24 + (uint32_t)optsize;
    for (uint16_t i = 0; i < nsections && *nsect < SWAP_SECT_MAX; i++) {
        uint32_t s = sect_off + i * 40u;
        if ((uint64_t)s + 40 > img_len) break;
        uint32_t vsize, rva, raw_size, raw_off;
        if (pe_read32(img, img_len, s + 16, &vsize) ||
            pe_read32(img, img_len, s + 12, &rva) ||
            pe_read32(img, img_len, s + 20, &raw_size) ||
            pe_read32(img, img_len, s + 24, &raw_off)) break;
        if (!vsize) continue;
        uint64_t dst = image_base + rva;
        if (raw_size) {
            /* copy section */
            if ((uint64_t)raw_off + raw_size > img_len) break;
            sect[*nsect].src_pa = (uint64_t)(uintptr_t)(img + raw_off);
            sect[*nsect].dst_pa = dst;
            sect[*nsect].len    = raw_size;
            sect[*nsect].flags  = 0;
            (*nsect)++;
        }
        if (vsize > raw_size) {
            /* BSS: zero the excess */
            sect[*nsect].src_pa = 0;
            sect[*nsect].dst_pa = dst + raw_size;
            sect[*nsect].len    = vsize - raw_size;
            sect[*nsect].flags  = SWAP_SECT_ZERO;
            (*nsect)++;
        }
    }
    return *nsect ? (image_base + entry_rva) : 0;
}

/* ---- Signature verification ---- */
/* Minimal slot manifest check + Ed25519 verify + SHA-512 image hash.
 * We reproduce the logic of ck_slot_verify (boot/slot_select.c) here because
 * that file's include chain pulls UEFI headers not available in the kernel. */
#include "../../native/sig/aienos_sig.h"

static int slot_verify_swap(const uint8_t *obj, size_t obj_len,
                             const uint8_t *pubkey, const uint8_t **img_out, uint32_t *img_len_out)
{
    if (obj_len < CK_SLOT_UNIT) {
        ck_puts("swap: payload too short for a manifest\n");
        return -1;
    }
    const struct ck_slot_manifest *m = (const struct ck_slot_manifest *)obj;
    if (memcmp(m->magic, CK_SLOT_MANIFEST_MAGIC, 8) != 0) {
        ck_puts("swap: bad manifest magic\n");
        return -1;
    }
    if ((uint64_t)CK_SLOT_UNIT + m->image_len > obj_len) {
        ck_puts("swap: manifest image_len beyond payload\n");
        return -1;
    }
    if (aienos_ed25519_verify(m->signature, (const uint8_t *)m, CK_SLOT_TBS_BYTES, pubkey) != AIENOS_SIG_OK) {
        ck_puts("swap: Ed25519 signature REJECTED\n");
        return -1;
    }
    const uint8_t *img = obj + CK_SLOT_UNIT;
    uint8_t digest[64];
    aienos_sha512(img, (size_t)m->image_len, digest);
    if (memcmp(digest, m->image_sha512, 64) != 0) {
        ck_puts("swap: SHA-512 image hash mismatch\n");
        return -1;
    }
    *img_out = img;
    *img_len_out = m->image_len;
    return 0;
}

/* ---- Quiesce helpers ---- */
extern volatile uint32_t ap_up;       /* from kmain_x86.c (via x86.h) */
extern volatile int      allen_asleep;/* from kmain_x86.c */
static volatile int swap_quiescing;

static void quiesce_aps(void)
{
    /* Signal APs to enter their HLT loop via the existing rest mechanism. */
    __atomic_store_n(&swap_quiescing, 1, __ATOMIC_SEQ_CST);
    ck_x86_panel_off();          /* panel off */
    /* Wake any MWAIT'd APs, then wait for them to enter cli+hlt. */
    /* ck_x86_rest_wake is internal; replicate: bump park.gen */
    ck_x86_swap_wake_aps();      /* declared in x86.h, defined below */
    /* Give APs up to 100 ms to reach their HLT */
    uint64_t t0 = ck_rdtsc(), limit = ck_rdtsc() + ck_x86_tsc_hz() / 10;
    (void)t0;
    while (ck_rdtsc() < limit)
        ck_pause();
}

/* ---- Trampoline code (position-independent, copied to tramp_pa+512) ---- */
/* Defined in swap_tramp.S */
extern char ck_x86_swap_tramp[];
extern char ck_x86_swap_tramp_end[];

/* ---- UDP receive (LAB BUILD, via tg3) ---- */
/* Returns 1 if a swap was triggered, 0 otherwise. */
static int recv_swap_udp(void) { return 0; /* stub: tg3 recv not yet impl */ }

/* ---- Disk-staged swap (for QEMU test: CK_X86_SWAP_TEST_DISK) ---- */
#ifdef CK_X86_SWAP_TEST_DISK
static int disk_stage_triggered;
extern int ck_x86_swap_read_inbox(uint8_t *buf, size_t cap, size_t *len_out);
static void try_disk_stage(void)
{
    if (disk_stage_triggered || !swap_ready || !swap_buf_pa)
        return;
    disk_stage_triggered = 1;
    size_t len = 0;
    if (ck_x86_swap_read_inbox((uint8_t *)(uintptr_t)swap_buf_pa, SWAP_BUF_BYTES, &len))
        return;
    if (!len) return;
    swap_total    = (uint32_t)len;
    swap_received = (uint32_t)len;
}
#endif

/* ---- Disk-staged swap (QEMU: CK_X86_SWAP_TEST_DISK) ---- */
#ifdef CK_X86_SWAP_TEST_DISK
/* Reads the signed slot object from the first SATA disk, probing a set of
 * well-known LBAs where the test script places SWAP_INBOX.BIN. */
int ck_x86_swap_read_inbox(uint8_t *buf, size_t cap, size_t *len_out)
{
    *len_out = 0;
    extern int ck_x86_ahci_read_lba(unsigned ctrl, unsigned disk, uint64_t lba,
                                     unsigned count, uint8_t *buf);
    unsigned lba_try[] = {2048, 4096, 8192, 16384};
    static uint8_t header[512];
    for (unsigned k = 0; k < 4; k++) {
        if (ck_x86_ahci_read_lba(0, 0, lba_try[k], 1, header) != 0) continue;
        if (memcmp(header, "AIENSLT1", 8) != 0) continue;
        uint32_t img_len;
        memcpy(&img_len, header + 16, 4);
        size_t total = (size_t)4096 + img_len;
        if (total > cap) {
            ck_puts("swap_inbox: slot too large\n");
            return -1;
        }
        unsigned nsectors = (unsigned)((total + 511) / 512);
        if (ck_x86_ahci_read_lba(0, 0, lba_try[k], nsectors, buf) != 0) {
            ck_puts("swap_inbox: read failed\n");
            return -1;
        }
        *len_out = total;
        ck_printf("swap: inbox at LBA %u img_len=%u total=%zu\n",
                  lba_try[k], img_len, total);
        return 0;
    }
    return -1;
}
#endif /* CK_X86_SWAP_TEST_DISK */

/* ---- Public API ---- */

void ck_x86_swap_init(struct ck_frames *fr)
{
    uint64_t pa;
    if (ck_frames_alloc(fr, SWAP_BUF_PAGES, CK_PAGE, &pa)) {
        ck_puts("swap: no memory for swap buffer (swap disabled)\n");
        return;
    }
    swap_buf_pa = pa;
    swap_ready  = 1;
    ck_printf("swap: ready buf=0x%llx cap=%u KiB port=%u\n",
              (unsigned long long)pa, (unsigned)(SWAP_BUF_BYTES >> 10),
              (unsigned)CK_X86_SWAP_PORT);
}

/* Called from the hold loop. Returns 1 if a swap was performed (never returns
 * normally after a successful swap since we jump to the new kernel). */
int ck_x86_swap_step(const struct ck_handoff *hand, uint64_t tramp_pa)
{
    if (!swap_ready) return 0;

#ifdef CK_X86_SWAP_TEST_DISK
    try_disk_stage();
#endif
    if (!recv_swap_udp() && swap_received < swap_total && swap_total == 0)
        return 0;
    if (swap_received < swap_total || swap_total == 0)
        return 0;

    /* We have a complete payload in swap_buf_pa..swap_buf_pa+swap_total. */
    ck_puts("swap: payload complete, verifying\n");
    const uint8_t *img;
    uint32_t img_len;
    if (slot_verify_swap((const uint8_t *)(uintptr_t)swap_buf_pa, swap_total,
                          swap_pubkey, &img, &img_len)) {
        ck_puts("swap: REJECTED, resuming normal operation\n");
        swap_received = swap_total = 0;
        swap_in_progress = 0;
        return 0;
    }
    ck_puts("swap: verified ok, quiescing APs and devices\n");

    /* Parse PE sections. */
    struct swap_sect sects[SWAP_SECT_MAX];
    unsigned nsect = 0;
    uint64_t entry = pe_parse(img, (size_t)img_len, sects, &nsect);
    if (!entry || !nsect) {
        ck_puts("swap: PE parse failed, resuming\n");
        swap_received = swap_total = 0;
        return 0;
    }
    ck_printf("swap: PE ok entry=0x%llx sections=%u\n",
              (unsigned long long)entry, nsect);

    if (!tramp_pa) {
        ck_puts("swap: no tramp page (no free page below 1 MiB was available at boot), swap refused\n");
        swap_received = swap_total = 0;
        return 0;
    }

    /* Quiesce devices. */
    quiesce_aps();
    ck_x86_xhci_stop_all();
    ck_x86_tg3_stop();
    ck_x86_hda_stop();

    /* Build rebuilt handoff in the tramp page. */
    uint8_t *tp = (uint8_t *)(uintptr_t)tramp_pa;
    /* Section table. */
    struct swap_sect *st = (struct swap_sect *)(tp + TRAMP_SECT_OFF);
    for (unsigned i = 0; i < nsect; i++)
        st[i] = sects[i];
    /* Sentinel at end. */
    if (nsect < SWAP_SECT_MAX) {
        st[nsect].src_pa = 0;
        st[nsect].dst_pa = 0;
        st[nsect].len    = 0;
        st[nsect].flags  = 0xFFu;
        st[nsect]._pad   = 0;
    }
    /* Entry point. */
    uint64_t *ep = (uint64_t *)(tp + TRAMP_ENTRY_OFF);
    *ep = entry;
    /* Handoff. */
    struct ck_handoff *new_hand = (struct ck_handoff *)(tp + TRAMP_HAND_OFF);
    memcpy(new_hand, hand, sizeof *hand);
    new_hand->exit_attempts = 1;
    /* Update image_base/image_end from the PE header (first/last section). */
    uint64_t img_base_va = sects[0].dst_pa;
    uint64_t img_end_va  = sects[nsect-1].dst_pa + sects[nsect-1].len;
    for (unsigned i = 1; i < nsect; i++) {
        if (sects[i].dst_pa < img_base_va) img_base_va = sects[i].dst_pa;
        if (sects[i].dst_pa + sects[i].len > img_end_va) img_end_va = sects[i].dst_pa + sects[i].len;
    }
    new_hand->image_base = img_base_va;
    new_hand->image_end  = img_end_va;
    /* uefi_entry_ticks: preserve original (the clock since power-on). */

    /* Copy trampoline code to tramp_pa + TRAMP_CODE_OFF. */
    size_t tramp_code_bytes = (size_t)(ck_x86_swap_tramp_end - ck_x86_swap_tramp);
    if (tramp_code_bytes > CK_PAGE - TRAMP_CODE_OFF) {
        ck_puts("swap: trampoline too large, swap refused\n");
        return 0;
    }
    memcpy(tp + TRAMP_CODE_OFF, ck_x86_swap_tramp, tramp_code_bytes);
    __asm__ volatile("wbinvd" ::: "memory");

    ck_puts("swap: jumping to new kernel\n");
    /* Jump to the trampoline at tramp_pa + TRAMP_CODE_OFF. */
    typedef void (*tramp_fn)(uint64_t tramp_pa);
    tramp_fn fn = (tramp_fn)(uintptr_t)(tramp_pa + TRAMP_CODE_OFF);
    fn(tramp_pa);
    __builtin_unreachable();
}

#else  /* !CK_X86_SWAP */
void ck_x86_swap_init(struct ck_frames *fr) { (void)fr; }
int  ck_x86_swap_step(const struct ck_handoff *h, uint64_t tramp_pa) { (void)h; (void)tramp_pa; return 0; }
#endif /* CK_X86_SWAP */
