/* virtio_x86.c -- AienOS PC edition (CK_X86_PLATFORM_PC): the virtio 1.x PCI
 * devices a virtual machine gives the kernel, polled, no interrupts.
 *
 *   virtio-blk   (1af4:1042, or transitional 1af4:1001 subsystem 2): READ-ONLY.
 *                Capacity, sector 0 and the GPT through the shared report
 *                (pci_x86.c ck_x86_disk_report); the disk_dev's write and flush
 *                always refuse and no OUT request is ever built.
 *   virtio-net   (1af4:1041, or 1af4:1000 subsystem 1): brought up through the
 *                hosted-tested driver native/net/aienos_virtio_net.c; one
 *                broadcast hello frame (EtherType 0x88b5, local experimental)
 *                is sent so a host capture can see the path works; received
 *                frames are counted and dropped.
 *   virtio-input (1af4:1052) with EV_KEY and KEY_A: the keyboard. Its key
 *                events are turned into bytes for the console.
 * Other virtio devices (virtio-gpu/vga: the screen is the firmware's GOP
 * framebuffer, drawn by core/fbcon.c) are listed and left alone.
 *
 * Written from the virtio 1.2 specification (OASIS, 2022): 2.7 split
 * virtqueues, 3.1.1 driver initialization, 4.1.4 PCI capabilities, 5.1 net,
 * 5.2 block, 5.8 input. Only VIRTIO_F_VERSION_1 is negotiated (and
 * VIRTIO_F_ACCESS_PLATFORM when offered; this kernel runs without an IOMMU,
 * so bus addresses are physical). Every device-written index and length is
 * checked before use. QEMU runs of this path qualify nothing physical. */
#include "../core/ck_internal.h"
#include "../mm/frames.h"
#include "aienos_virtio_net.h"
#include "disk.h"
#include "x86.h"

void ck_x86_disk_report(const char *tag, unsigned ctrl, unsigned index, const disk_dev *dev); /* pci_x86.c */

#define VIRTIO_VENDOR 0x1af4u
#define ST_ACK 1u
#define ST_DRIVER 2u
#define ST_DRIVER_OK 4u
#define ST_FEATURES_OK 8u
#define ST_FAILED 128u
#define DESC_NEXT 1u
#define DESC_WRITE 2u

static struct ck_x86_virtio_result vres;
static uint64_t g_tsc_hz;
static uint8_t cfg_img[4096];

static uint32_t cr32(volatile uint8_t *cfg, unsigned off) { return *(volatile uint32_t *)(cfg + off); }
static uint16_t cr16(volatile uint8_t *cfg, unsigned off) { return *(volatile uint16_t *)(cfg + off); }
static void cw16(volatile uint8_t *cfg, unsigned off, uint16_t v) { *(volatile uint16_t *)(cfg + off) = v; }
static void mb(void) { __asm__ volatile("mfence" ::: "memory"); }

static void wait_us(uint64_t us)
{
    uint64_t t0 = ck_rdtsc(), n = g_tsc_hz / 1000000 * us;
    while (ck_rdtsc() - t0 < n)
        ck_pause();
}

/* ---- transport: the four capability windows ---- */
enum { W_COMMON = 0, W_NOTIFY = 1, W_ISR = 2, W_DEVICE = 3 };
struct vio {
    volatile uint8_t *win[4];
    uint32_t len[4];
    uint32_t notify_mult;
    virtio_pci_caps caps;
};

static uint8_t c8(struct vio *v, unsigned o) { return *(volatile uint8_t *)(v->win[W_COMMON] + o); }
static uint16_t c16(struct vio *v, unsigned o) { return *(volatile uint16_t *)(v->win[W_COMMON] + o); }
static uint32_t c32(struct vio *v, unsigned o) { return *(volatile uint32_t *)(v->win[W_COMMON] + o); }
static void w8(struct vio *v, unsigned o, uint8_t x) { *(volatile uint8_t *)(v->win[W_COMMON] + o) = x; }
static void w16(struct vio *v, unsigned o, uint16_t x) { *(volatile uint16_t *)(v->win[W_COMMON] + o) = x; }
static void w32(struct vio *v, unsigned o, uint32_t x) { *(volatile uint32_t *)(v->win[W_COMMON] + o) = x; }
static uint8_t d8(struct vio *v, unsigned o) { return o < v->len[W_DEVICE] ? *(volatile uint8_t *)(v->win[W_DEVICE] + o) : 0; }
static uint32_t d32(struct vio *v, unsigned o)
{
    return o + 4 <= v->len[W_DEVICE] ? *(volatile uint32_t *)(v->win[W_DEVICE] + o) : 0;
}
static void dw8(struct vio *v, unsigned o, uint8_t x)
{
    if (o < v->len[W_DEVICE])
        *(volatile uint8_t *)(v->win[W_DEVICE] + o) = x;
}

static uint64_t bar_addr(volatile uint8_t *cfg, unsigned bar)
{
    if (bar > 5)
        return 0;
    uint32_t lo = cr32(cfg, 0x10 + 4 * bar);
    if (lo & 1)
        return 0; /* an I/O BAR: the modern windows are memory */
    uint64_t a = lo & ~0xfull;
    if ((lo & 6) == 4 && bar < 5)
        a |= (uint64_t)cr32(cfg, 0x14 + 4 * bar) << 32;
    return a;
}

/* 0 ok; the reason otherwise. */
static const char *vio_map(volatile uint8_t *cfg, struct vio *v)
{
    for (unsigned off = 0; off < sizeof cfg_img; off += 4) {
        uint32_t x = cr32(cfg, off);
        memcpy(cfg_img + off, &x, 4);
    }
    virtio_pci_err e = virtio_pci_parse_caps(cfg_img, sizeof cfg_img, &v->caps);
    if (e != VIRTIO_PCI_OK)
        return virtio_pci_err_name(e);
    const virtio_pci_region *r[4] = {&v->caps.common_cfg, &v->caps.notify_cfg, &v->caps.isr_cfg,
                                     &v->caps.device_cfg};
    for (unsigned i = 0; i < 4; i++) {
        if (!r[i]->present)
            return "capability missing";
        uint64_t a = bar_addr(cfg, r[i]->bar);
        if (!a)
            return "capability BAR unusable";
        a += r[i]->offset;
        if (ck_x86_map_mmio(a, r[i]->length))
            return "capability window not mappable";
        v->win[i] = (volatile uint8_t *)(uintptr_t)a;
        v->len[i] = r[i]->length;
    }
    if (v->len[W_COMMON] < 0x38)
        return "common window too short";
    v->notify_mult = v->caps.notify_off_multiplier;
    cw16(cfg, 4, (uint16_t)(cr16(cfg, 4) | 0x6)); /* memory decode + bus master */
    return 0;
}

/* Reset, ACK, DRIVER, features (VERSION_1 + ACCESS_PLATFORM when offered),
 * FEATURES_OK checked. 0 ok; the reason otherwise. */
static const char *vio_begin(struct vio *v)
{
    w8(v, 0x14, 0);
    for (unsigned i = 0; i < 1000 && c8(v, 0x14); i++)
        wait_us(100);
    if (c8(v, 0x14))
        return "reset did not complete";
    w8(v, 0x14, ST_ACK);
    w8(v, 0x14, ST_ACK | ST_DRIVER);
    w32(v, 0x00, 1);
    uint32_t hi = c32(v, 0x04);
    if (!(hi & 1u)) {
        w8(v, 0x14, ST_FAILED);
        return "no VIRTIO_F_VERSION_1";
    }
    w32(v, 0x08, 0);
    w32(v, 0x0c, 0);
    w32(v, 0x08, 1);
    w32(v, 0x0c, 1u | (hi & 2u));
    w8(v, 0x14, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(c8(v, 0x14) & ST_FEATURES_OK)) {
        w8(v, 0x14, ST_FAILED);
        return "FEATURES_OK refused";
    }
    return 0;
}

/* ---- one split virtqueue in one 4 KiB frame (size <= 64) ---- */
struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};
_Static_assert(sizeof(struct vq_desc) == 16, "virtq_desc");
struct vq {
    uint16_t size, avail_idx, last_used, index;
    volatile struct vq_desc *desc;
    volatile uint16_t *avail; /* flags, idx, ring[size] */
    volatile uint8_t *used;   /* flags, idx, {id, len}[size] */
    volatile uint16_t *notify;
};

static const char *vq_setup(struct vio *v, uint16_t index, uint16_t want, struct vq *q, struct ck_frames *fr)
{
    w16(v, 0x16, index);
    uint16_t max = c16(v, 0x18);
    if (!max)
        return "queue missing";
    if (c16(v, 0x1c))
        return "queue already enabled";
    uint16_t size = 1;
    while ((uint16_t)(size << 1) <= max && (uint16_t)(size << 1) <= want)
        size = (uint16_t)(size << 1);
    uint32_t noff = (uint32_t)c16(v, 0x1e) * v->notify_mult;
    if (noff + 2 > v->len[W_NOTIFY])
        return "notify offset outside the window";
    uint64_t pa;
    if (ck_frames_alloc(fr, 1, CK_PAGE, &pa))
        return "no frame for the queue";
    memset((void *)(uintptr_t)pa, 0, CK_PAGE);
    q->size = size;
    q->index = index;
    q->avail_idx = q->last_used = 0;
    q->desc = (volatile struct vq_desc *)(uintptr_t)pa;
    q->avail = (volatile uint16_t *)(uintptr_t)(pa + 1024);
    q->used = (volatile uint8_t *)(uintptr_t)(pa + 2048);
    q->notify = (volatile uint16_t *)(v->win[W_NOTIFY] + noff);
    q->avail[0] = 1; /* VIRTQ_AVAIL_F_NO_INTERRUPT: the kernel polls */
    w16(v, 0x18, size);
    w32(v, 0x20, (uint32_t)pa);
    w32(v, 0x24, (uint32_t)(pa >> 32));
    w32(v, 0x28, (uint32_t)(pa + 1024));
    w32(v, 0x2c, (uint32_t)((pa + 1024) >> 32));
    w32(v, 0x30, (uint32_t)(pa + 2048));
    w32(v, 0x34, (uint32_t)((pa + 2048) >> 32));
    w16(v, 0x1c, 1);
    return 0;
}

static void vq_push(struct vq *q, uint16_t head)
{
    q->avail[2 + q->avail_idx % q->size] = head;
    mb();
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
    mb();
    *q->notify = q->index;
}

/* 1 and the used element, 0 when none, -1 when the device wrote nonsense. */
static int vq_pop(struct vq *q, uint32_t *id, uint32_t *len)
{
    mb();
    uint16_t used_idx = *(volatile uint16_t *)(q->used + 2);
    if (used_idx == q->last_used)
        return 0;
    if ((uint16_t)(used_idx - q->last_used) > q->size)
        return -1;
    volatile uint32_t *e = (volatile uint32_t *)(q->used + 4 + 8u * (q->last_used % q->size));
    *id = e[0];
    *len = e[1];
    q->last_used++;
    return *id < q->size ? 1 : -1;
}

/* ---- virtio-blk, read-only ---- */
#define BLK_MAX 2u
#define BLK_IO_SECTORS 128u /* 64 KiB bounce */
struct blk {
    struct vio v;
    struct vq q;
    uint8_t *req;  /* 16-byte request header, then the status byte at +16 */
    uint64_t req_pa;
    uint8_t *data; /* BLK_IO_SECTORS * 512 */
    uint64_t data_pa;
    uint64_t capacity;
    int broken;
    disk_dev dev;
};
static struct blk blks[BLK_MAX];

static int blk_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf)
{
    struct blk *b = ctx;
    if (b->broken)
        return DISK_ESTATE;
    if (!count || lba >= b->capacity || count > b->capacity - lba)
        return DISK_ERANGE;
    while (count) {
        uint32_t n = count < BLK_IO_SECTORS ? count : BLK_IO_SECTORS;
        memset(b->req, 0, 17);
        memcpy(b->req + 8, &lba, 8); /* type 0 = VIRTIO_BLK_T_IN, reserved 0, sector */
        b->req[16] = 0xff;
        b->q.desc[0] = (struct vq_desc){b->req_pa, 16, DESC_NEXT, 1};
        b->q.desc[1] = (struct vq_desc){b->data_pa, n * 512u, DESC_NEXT | DESC_WRITE, 2};
        b->q.desc[2] = (struct vq_desc){b->req_pa + 16, 1, DESC_WRITE, 0};
        vq_push(&b->q, 0);
        uint32_t id = 0, len = 0;
        int got = 0;
        uint64_t t0 = ck_rdtsc();
        while (!(got = vq_pop(&b->q, &id, &len)) && ck_rdtsc() - t0 < 2 * g_tsc_hz)
            ck_pause();
        if (got != 1 || id != 0 || b->req[16] != 0) {
            b->broken = 1; /* a lost, foreign or failed completion: no further reads */
            return DISK_EIO;
        }
        memcpy(buf, b->data, n * 512u);
        buf += n * 512u;
        lba += n;
        count -= n;
    }
    return DISK_OK;
}
static int blk_ro_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf)
{
    (void)ctx, (void)lba, (void)count, (void)buf;
    return DISK_ESTATE; /* read-only: no OUT request exists in this file */
}
static int blk_ro_flush(void *ctx)
{
    (void)ctx;
    return DISK_ESTATE;
}

static void blk_start(volatile uint8_t *cfg, const char *bdf, struct ck_frames *fr)
{
    unsigned idx = vres.blk_disks;
    if (idx >= BLK_MAX) {
        ck_printf("virtio_blk: %s skipped (more than %u disks)\n", bdf, BLK_MAX);
        return;
    }
    struct blk *b = &blks[idx];
    const char *why = vio_map(cfg, &b->v);
    if (!why)
        why = vio_begin(&b->v);
    if (!why)
        why = vq_setup(&b->v, 0, 8, &b->q, fr);
    uint64_t pa = 0;
    if (!why && ck_frames_alloc(fr, 1 + BLK_IO_SECTORS * 512u / CK_PAGE, CK_PAGE, &pa))
        why = "no DMA frames";
    if (why) {
        ck_printf("virtio_blk: %s not bound (%s)\n", bdf, why);
        return;
    }
    b->req = (uint8_t *)(uintptr_t)pa;
    b->req_pa = pa;
    b->data = (uint8_t *)(uintptr_t)(pa + CK_PAGE);
    b->data_pa = pa + CK_PAGE;
    b->capacity = (uint64_t)d32(&b->v, 0) | (uint64_t)d32(&b->v, 4) << 32;
    w8(&b->v, 0x14, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    b->dev = (disk_dev){b, 512, b->capacity, blk_read, blk_ro_write, blk_ro_flush, BLK_IO_SECTORS};
    if (!b->capacity) {
        ck_printf("virtio_blk: %s no medium (capacity 0)\n", bdf);
        return;
    }
    vres.blk_disks++;
    ck_printf("virtio_blk_disk: ctrl=%u disk=0 %s blocks=%llu block=512 mib=%llu queue=%u access=read-only\n", idx,
              bdf, (unsigned long long)b->capacity, (unsigned long long)(b->capacity >> 11), b->q.size);
    ck_x86_disk_report("virtio_blk", idx, 0, &b->dev);
}

/* ---- virtio-net, through native/net/aienos_virtio_net.c ---- */
static struct vio nv;
static vnet_dev vn;
static unsigned net_rx_frames;

static uint32_t vn_read(void *ctx, vnet_window win, uint32_t off, unsigned width)
{
    volatile uint8_t *p = ((struct vio *)ctx)->win[win] + off;
    return width == 1 ? *p : width == 2 ? *(volatile uint16_t *)p : *(volatile uint32_t *)p;
}
static void vn_write(void *ctx, vnet_window win, uint32_t off, unsigned width, uint32_t val)
{
    volatile uint8_t *p = ((struct vio *)ctx)->win[win] + off;
    if (width == 1)
        *p = (uint8_t)val;
    else if (width == 2)
        *(volatile uint16_t *)p = (uint16_t)val;
    else
        *(volatile uint32_t *)p = val;
}
static void vn_notify(void *ctx, uint32_t off, uint16_t queue)
{
    mb();
    *(volatile uint16_t *)(((struct vio *)ctx)->win[W_NOTIFY] + off) = queue;
}

static void net_start(volatile uint8_t *cfg, const char *bdf, struct ck_frames *fr)
{
    if (vres.net_up) {
        ck_printf("virtio_net: %s skipped (one network device is driven)\n", bdf);
        return;
    }
    const char *why = vio_map(cfg, &nv);
    size_t need = vnet_mem_size(16);
    uint64_t pa = 0;
    if (!why && ck_frames_alloc(fr, (need + CK_PAGE - 1) / CK_PAGE, CK_PAGE, &pa))
        why = "no DMA frames";
    if (why) {
        ck_printf("virtio_net: %s not bound (%s)\n", bdf, why);
        return;
    }
    memset((void *)(uintptr_t)pa, 0, need);
    vnet_ops ops = {&nv, vn_read, vn_write, vn_notify};
    vnet_err e = vnet_init(&vn, &ops, &nv.caps, (void *)(uintptr_t)pa, pa, need, 16);
    if (e != VNET_OK) {
        ck_printf("virtio_net: %s not bound (%s)\n", bdf, vnet_err_name(e));
        return;
    }
    static const char hello[] = CK_X86_TITLE " hello from virtio-net";
    uint8_t frame[64] = {0};
    memset(frame, 0xff, 6);
    if (vn.has_mac)
        memcpy(frame + 6, vn.mac, 6);
    else
        memcpy(frame + 6, "\x02\x00\x00\x00\x00\x01", 6);
    frame[12] = 0x88;
    frame[13] = 0xb5;
    memcpy(frame + 14, hello, sizeof hello - 1);
    e = vnet_tx(&vn, frame, 60);
    unsigned done = 0;
    uint64_t t0 = ck_rdtsc();
    while (e == VNET_OK && !done && ck_rdtsc() - t0 < g_tsc_hz / 5) {
        unsigned n = 0;
        e = vnet_tx_reclaim(&vn, &n);
        done += n;
    }
    if (e != VNET_OK || !done) {
        ck_printf("virtio_net: %s up, hello frame not completed (%s)\n", bdf, e != VNET_OK ? vnet_err_name(e) : "timeout");
        vres.net_up = 1;
        return;
    }
    vres.net_up = 1;
    vres.net_tx_frames = 1;
    ck_printf("virtio_net: %s up queue=%u mac=%s tx_hello=sent ethertype=0x88b5\n", bdf, vn.q[VNET_TX_QUEUE].size,
              vn.has_mac ? "present" : "none");
}

/* ---- virtio-input keyboard ---- */
#define KQ_WANT 64u
static struct vio kv;
static struct vq kq;
static volatile uint8_t *kev; /* KQ_WANT events of 8 bytes */
static unsigned kshift, kctrl;
static char kbuf[64];
static unsigned khead, ktail;

/* Linux input key codes 0..57 (the same numbers as PC scan code set 1 make
 * codes), US layout. Shared with the PS/2 path in pc_console_x86.c. */
const char ck_x86_keymap_lo[58] = "\0\033" "1234567890-=" "\b\t" "qwertyuiop[]" "\n\0" "asdfghjkl;'`" "\0\\"
                                  "zxcvbnm,./" "\0*\0 ";
const char ck_x86_keymap_hi[58] = "\0\033" "!@#$%^&*()_+" "\b\t" "QWERTYUIOP{}" "\n\0" "ASDFGHJKL:\"~" "\0|"
                                  "ZXCVBNM<>?" "\0*\0 ";

static void kput(char c)
{
    if ((unsigned)(ktail - khead) < sizeof kbuf)
        kbuf[ktail++ % sizeof kbuf] = c;
}

static void key_event(uint16_t code, uint32_t value)
{
    if (code == 42 || code == 54) { /* shifts */
        kshift = value != 0;
        return;
    }
    if (code == 29 || code == 97) { /* controls */
        kctrl = value != 0;
        return;
    }
    if (value == 0)
        return; /* release */
    if (code == 96) { /* keypad Enter */
        kput('\n');
        return;
    }
    if (code >= sizeof ck_x86_keymap_lo)
        return;
    char c = (kshift ? ck_x86_keymap_hi : ck_x86_keymap_lo)[code];
    if (kctrl && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);
    if (c)
        kput(c);
}

static void kbd_start(volatile uint8_t *cfg, const char *bdf, struct ck_frames *fr)
{
    if (vres.kbd_up) {
        ck_printf("virtio_input: %s skipped (one keyboard is driven)\n", bdf);
        return;
    }
    struct vio v;
    const char *why = vio_map(cfg, &v);
    if (!why)
        why = vio_begin(&v);
    if (why) {
        ck_printf("virtio_input: %s not bound (%s)\n", bdf, why);
        return;
    }
    char name[64];
    dw8(&v, 0, 1); /* VIRTIO_INPUT_CFG_ID_NAME */
    dw8(&v, 1, 0);
    unsigned n = d8(&v, 2);
    if (n > sizeof name - 1)
        n = sizeof name - 1;
    for (unsigned i = 0; i < n; i++) {
        char c = (char)d8(&v, 8 + i);
        if (!c) {
            n = i;
            break;
        }
        name[i] = c >= 0x20 && c < 0x7f && c != '"' ? c : '?';
    }
    name[n] = 0;
    dw8(&v, 0, 0x11); /* VIRTIO_INPUT_CFG_EV_BITS, subsel EV_KEY */
    dw8(&v, 1, 1);
    int has_a = d8(&v, 2) > 3 && (d8(&v, 8 + 30 / 8) & 1u << (30 % 8)); /* KEY_A */
    if (!has_a) {
        w8(&v, 0x14, 0);
        ck_printf("virtio_input: %s name=\"%s\" not a keyboard, left alone\n", bdf, name);
        return;
    }
    uint64_t pa;
    why = vq_setup(&v, 0, KQ_WANT, &kq, fr);
    if (!why && ck_frames_alloc(fr, 1, CK_PAGE, &pa))
        why = "no event frame";
    if (why) {
        w8(&v, 0x14, ST_FAILED);
        ck_printf("virtio_input: %s not bound (%s)\n", bdf, why);
        return;
    }
    kv = v;
    kev = (volatile uint8_t *)(uintptr_t)pa;
    memset((void *)(uintptr_t)pa, 0, CK_PAGE);
    w8(&kv, 0x14, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    for (uint16_t i = 0; i < kq.size; i++) {
        kq.desc[i] = (struct vq_desc){pa + 8u * i, 8, DESC_WRITE, 0};
        vq_push(&kq, i);
    }
    vres.kbd_up = 1;
    ck_printf("virtio_input: %s keyboard name=\"%s\" queue=%u\n", bdf, name, kq.size);
}

int ck_x86_virtio_poll_key(void)
{
    if (vres.kbd_up) {
        uint32_t id, len;
        int r;
        while ((r = vq_pop(&kq, &id, &len)) == 1) {
            uint16_t type, code;
            uint32_t value;
            memcpy(&type, (const void *)(kev + 8u * id), 2);
            memcpy(&code, (const void *)(kev + 8u * id + 2), 2);
            memcpy(&value, (const void *)(kev + 8u * id + 4), 4);
            if (type == 1 && len >= 8) /* EV_KEY */
                key_event(code, value);
            vq_push(&kq, (uint16_t)id);
        }
        if (r < 0) {
            vres.kbd_up = 0; /* the device wrote nonsense: stop using it */
            ck_puts("virtio_input: keyboard stopped (bad completion)\n");
        }
    }
    if (vres.net_up && !vn.broken) {
        static uint8_t f[VNET_FRAME_MAX];
        for (unsigned k = 0; k < 16; k++) {
            size_t len = 0;
            int got = 0;
            if (vnet_rx(&vn, f, sizeof f, &len, &got) != VNET_OK || !got)
                break;
            net_rx_frames++;
        }
        vnet_tx_reclaim(&vn, 0);
    }
    if (khead == ktail)
        return -1;
    return (unsigned char)kbuf[khead++ % sizeof kbuf];
}

unsigned ck_x86_virtio_net_rx_frames(void) { return net_rx_frames; }

const struct ck_x86_virtio_result *ck_x86_virtio_result(void) { return &vres; }

void ck_x86_virtio_probe(volatile uint8_t *cfg, uint8_t bus, uint8_t dev, uint8_t fn, struct ck_frames *fr,
                         uint64_t tsc_hz)
{
    g_tsc_hz = tsc_hz;
    uint32_t id = cr32(cfg, 0);
    uint16_t device = (uint16_t)(id >> 16), subsys = cr16(cfg, 0x2e);
    if ((id & 0xffff) != VIRTIO_VENDOR)
        return;
    char bdf[16];
    ck_snprintf(bdf, sizeof bdf, "%02x:%02x.%u", bus, dev, fn);
    /* modern ids are 0x1040 + type; transitional 0x1000.. carry the type in the subsystem id */
    unsigned type = device >= 0x1040 && device <= 0x107f ? device - 0x1040u
                    : device >= 0x1000 && device <= 0x103f ? subsys
                                                            : 0;
    ck_printf("virtio: %s id=%04x:%04x type=%u\n", bdf, VIRTIO_VENDOR, device, type);
    if (type == 2)
        blk_start(cfg, bdf, fr);
    else if (type == 1)
        net_start(cfg, bdf, fr);
    else if (type == 18)
        kbd_start(cfg, bdf, fr);
    else
        ck_printf("virtio: %s left alone (type %u has no driver here; the screen is the firmware framebuffer)\n", bdf,
                  type);
}
