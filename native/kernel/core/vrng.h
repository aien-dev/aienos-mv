/* vrng.h -- virtio entropy device (virtio-rng) as the kernel entropy source
 * of a VM edition image without FEAT_RNG (core/vguest.h, arch/rndr.c). */
#ifndef AIENOS_CK_VRNG_H
#define AIENOS_CK_VRNG_H
#include "entropy.h"
/* Ops for core/entropy.c: present() attaches the device (once) and is 1 only
 * in a VM edition guest with a virtio entropy device on the root bus. */
const struct ck_rng_ops *ck_vrng_ops(void);
#endif
