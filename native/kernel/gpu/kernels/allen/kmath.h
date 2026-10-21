/* native/kernel/gpu/kernels/allen/kmath.h -- f32 maths for the Allen kernels.
 *
 * One definition, two forms: GPU code emitters (kmath.c, no floats) and the
 * host C mirror (fmath_ref.c) that performs the same IEEE operations in the
 * same order. The GPU path uses only v_add/v_sub/v_mul/v_fma_f32, v_min/max,
 * v_rndne, conversions and integer ops; never v_exp/v_log/v_rcp/v_rsq/v_sqrt
 * (approximate on hardware) nor v_mac/v_mad (flush f32 denormals).
 *
 * Domains (pre-registered; outside them results are unspecified):
 *   exp(x)     any finite x: x > 88.72283172607421875 -> +inf, x < -104 -> 0
 *   div(a, b)  b in [2^-64, +inf], a finite (and |a| >= 2^-62 when b > 2^64)
 *   sqrt(x)    x positive normal
 *   log1p(y)   y in [0, 2^100]
 *   silu, sigmoid: finite x;  softplus: finite x (x > 20 returns x)
 */
#ifndef KMATH_H
#define KMATH_H
#include <stdint.h>

#define KM_EXP_P0 0x39506967u /* 1.9875691500E-4 */
#define KM_EXP_P1 0x3AB743CEu /* 1.3981999507E-3 */
#define KM_EXP_P2 0x3C088908u /* 8.3334519073E-3 */
#define KM_EXP_P3 0x3D2AA9C1u /* 4.1665795894E-2 */
#define KM_EXP_P4 0x3E2AAAAAu /* 1.6666665459E-1 */
#define KM_EXP_P5 0x3F000000u /* 0.5 */
#define KM_LOG2E  0x3FB8AA3Bu /* 1.44269502 */
#define KM_LN2_HI 0x3F317200u /* 0.693145751953125 */
#define KM_LN2_LO 0x35BFBE8Eu /* 1.428606765e-06 */
#define KM_EXP_HI 0x42B17217u /* 88.72283172607421875: above -> +inf */
#define KM_EXP_LO 0xC2D00000u /* -104: below -> 0 */
#define KM_LOG_P0 0x3D9021BBu /* 7.0376836292E-2 */
#define KM_LOG_P1 0xBDEBD1B8u /* -1.1514610310E-1 */
#define KM_LOG_P2 0x3DEF251Au /* 1.1676998740E-1 */
#define KM_LOG_P3 0xBDFE5D4Fu /* -1.2420140846E-1 */
#define KM_LOG_P4 0x3E11E9BFu /* 1.4249322787E-1 */
#define KM_LOG_P5 0xBE2AAE50u /* -1.6668057665E-1 */
#define KM_LOG_P6 0x3E4CCEACu /* 2.0000714765E-1 */
#define KM_LOG_P7 0xBE7FFFFCu /* -2.4999993993E-1 */
#define KM_LOG_P8 0x3EAAAAAAu /* 3.3333331174E-1 */
#define KM_LOG_Q1 0xB95E8083u /* -2.12194440e-4 */
#define KM_LOG_Q2 0x3F318000u /* 0.693359375 */
#define KM_SQRT2  0x3FB504F3u
#define KM_TWENTY 0x41A00000u /* 20 */
#define KM_TWO64  0x5F800000u /* 2^64 */
#define KM_TWOM64 0x1F800000u /* 2^-64 */
#define KM_RCP_MAGIC 0x7EF311C3u
#define KM_RSQ_MAGIC 0x5F3759DFu
#define KM_INF    0x7F800000u
#define KM_ONE    0x3F800000u

/* temp VGPR counts (t .. t+N-1); all use s[96:99] masks and s100 */
#define KM_EXP_T     5
#define KM_DIV_T     6
#define KM_SQRT_T    3
#define KM_LOG1P_T   12
#define KM_SILU_T    8
#define KM_SIGMOID_T 8
#define KM_SOFTPLUS_T 14

#ifndef KMATH_HOST_ONLY
#include "ka.h"
/* vd = f(x); x is a VGPR operand (KV) unless noted; vd may equal the input */
void km_exp(ka_t *k, uint32_t vd, ko x, uint32_t t);
void km_div(ka_t *k, uint32_t vd, ko a, ko b, uint32_t t);   /* a: VGPR or KK const */
void km_sqrt(ka_t *k, uint32_t vd, ko x, uint32_t t);
void km_log1p(ka_t *k, uint32_t vd, ko y, uint32_t t);
void km_silu(ka_t *k, uint32_t vd, ko x, uint32_t t);
void km_sigmoid(ka_t *k, uint32_t vd, ko x, uint32_t t);
void km_softplus(ka_t *k, uint32_t vd, ko x, uint32_t t);
#endif

#endif
