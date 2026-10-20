/* join_boot.h -- the Interplane provisioning bundle (\EFI\AIENOS\JOIN.BIN,
 * native/net/aienos_ipj.h "provisioning bundle") read by the UEFI stub before
 * ExitBootServices (boot/efi_join.c) and used by the devices stage
 * (dev/net_join.c). CK_INTERPLANE images only. QUALIFICATION ONLY: the bundle
 * holds the machine secret key in the clear (QEMU fixtures). */
#ifndef CK_JOIN_BOOT_H
#define CK_JOIN_BOOT_H
#include <stddef.h>
#include <stdint.h>
#include "../../net/aienos_ipj.h"

#define CK_JOIN_ABSENT 0
#define CK_JOIN_REFUSED 1
#define CK_JOIN_FOUND 2
#define CK_JOIN_PATH "\\EFI\\AIENOS\\JOIN.BIN"

struct ck_join_boot {
    int state;                   /* CK_JOIN_* */
    uint8_t buf[IPJ_BUNDLE_MAX]; /* only while FOUND; wiped after use */
    size_t len;
};
extern struct ck_join_boot ck_join_boot;
#endif
