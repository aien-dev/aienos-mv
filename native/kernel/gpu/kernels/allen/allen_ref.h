/* native/kernel/gpu/kernels/allen/allen_ref.h -- host mirror of the Allen kernels (test only). */
#ifndef ALLEN_REF_H
#define ALLEN_REF_H
#include <stdint.h>
/* Runs kernel `id` (variant c) with parameters p[0..11] over `groups` workgroups on image m. */
void ar_run(uint8_t *m, int id, uint32_t c, const uint32_t *p, uint32_t groups);
#endif
