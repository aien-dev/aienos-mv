/* native/kernel/gpu/kernels/allen/fmath_ref.h -- host C mirror of kmath (host tests only).
 * Build with -ffp-contract=off: every operation rounds exactly where the GPU's does. */
#ifndef FMATH_REF_H
#define FMATH_REF_H
#include <stdint.h>
#include <string.h>

static inline float km_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t km_u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

float km_exp_ref(float x);
float km_div_ref(float a, float b);
float km_sqrt_ref(float x);
float km_log1p_ref(float y);
float km_silu_ref(float x);
float km_sigmoid_ref(float x);
float km_softplus_ref(float x);
#endif
