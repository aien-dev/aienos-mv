/* efi_join.c -- read \EFI\AIENOS\JOIN.BIN (the Interplane provisioning bundle,
 * native/net/aienos_ipj.h) from the boot volume before ExitBootServices
 * (CK_INTERPLANE image). Nothing is written. Result in ck_join_boot: ABSENT (no
 * file: no Interplane join this boot), REFUSED (printed why), FOUND (layout
 * checked; the roster is verified later by the devices stage). */
#include "efi.h"
#include "../kernel/core/join_boot.h"
#include "../kernel/arch/arch.h"
#include "../kernel/core/ck_internal.h"

struct ck_join_boot ck_join_boot;

static void wipe_buf(void)
{
    volatile uint8_t *b = ck_join_boot.buf;
    for (unsigned i = 0; i < sizeof ck_join_boot.buf; i++)
        b[i] = 0;
}

void ck_boot_join_find(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID_INIT;
    static const EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID_INIT;
    static CHAR16 path[] = {'\\', 'E', 'F', 'I', '\\', 'A', 'I', 'E', 'N', 'O', 'S', '\\',
                            'J', 'O', 'I', 'N', '.', 'B', 'I', 'N', 0};
    static uint8_t probe;
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL *root = 0, *f = 0;
    ck_join_boot.state = CK_JOIN_ABSENT;
    ck_join_boot.len = 0;
    if (bs->HandleProtocol(image, &li_guid, (void **)&li) != EFI_SUCCESS || !li || !li->DeviceHandle ||
        bs->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs) != EFI_SUCCESS || !fs ||
        fs->OpenVolume(fs, &root) != EFI_SUCCESS || !root) {
        ck_puts("interplane: absent (boot volume not readable); no join this boot\n");
        return;
    }
    if (root->Open(root, &f, path, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS || !f) {
        root->Close(root);
        ck_puts("interplane: absent (no " CK_JOIN_PATH "); no join this boot\n");
        return;
    }
    wipe_buf();
    uint64_t n = sizeof ck_join_boot.buf;
    EFI_STATUS s = f->Read(f, &n, ck_join_boot.buf);
    uint64_t more = 1;
    EFI_STATUS s2 = s == EFI_SUCCESS ? f->Read(f, &more, &probe) : s; /* a longer file is refused */
    f->Close(f);
    root->Close(root);
    ck_join_boot.state = CK_JOIN_REFUSED;
    if (s != EFI_SUCCESS || s2 != EFI_SUCCESS || more != 0 || n < 1) {
        ck_printf("interplane: refused (read status=0x%llx bytes=%llu, at most %u)\n", (unsigned long long)s,
                  (unsigned long long)n, (unsigned)IPJ_BUNDLE_MAX);
        wipe_buf();
        return;
    }
    ipj_bundle b;
    ipj_status ps = ipj_bundle_parse(ck_join_boot.buf, (size_t)n, &b);
    if (ps != IPJ_OK) {
        ck_printf("interplane: refused (bundle %s)\n", ipj_status_name(ps));
        wipe_buf();
        return;
    }
    volatile uint8_t *w = b.sk;
    for (unsigned i = 0; i < sizeof b.sk; i++) w[i] = 0;
    ck_join_boot.len = (size_t)n;
    ck_join_boot.state = CK_JOIN_FOUND;
    ck_printf("interplane: found a well-formed join bundle (QUALIFICATION ONLY, %u bytes)\n", (unsigned)n);
}
