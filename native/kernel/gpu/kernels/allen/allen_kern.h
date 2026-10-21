/* native/kernel/gpu/kernels/allen/allen_kern.h -- the gfx803 kernels that run the small Allen
 * (Qwen3.5-0.8B student, L389) one token at a time. Kernel-side code: no floats.
 *
 * Every kernel is one wavefront (64 lanes) per workgroup. Launch ABI (16 user SGPRs, then
 * TGID_X): s[0:3] a raw buffer descriptor over the whole image (base = image start,
 * num_records = 0xFFFFFFFF), s4..s15 the parameters p0..p11 below, s16 the workgroup id,
 * v0 the lane id. Offsets are bytes from the image start. f32 throughout; matrices bf16.
 * Rounding is IEEE binary32 round-to-nearest-even with denormals kept (FLOAT_MODE 0xF0); no
 * approximate (v_exp/v_rcp/...) and no denormal-flushing (v_mac/v_mad) instruction is used.
 * allen_ref.c performs the same operations in the same order on the host.
 *
 * Kernels (groups = workgroups to launch):
 *   AK_MATVEC(c)     y[r] = sum_k W[r][k] x[k], k = 512 c.   groups = ceil(n / p4)
 *                    p0 W (bf16 [n][k]), p1 x, p2 y, p3 k, p4 rows per group, p5 n
 *   AK_MATVEC_ACC(c) the same with y[r] = y[r] + sum (the residual add)
 *   AK_NORM_ZC       y = x * inv * (1 + w), inv = 1 / sqrt(sum x^2 / n + eps)   groups = rows
 *   AK_NORM_GATED    y = x * inv * w * silu(z)
 *   AK_NORM_L2       y = x * (1 / sqrt(sum x^2 + 1e-6)) * scale, scale = 1 / sqrt(p9) (1 if p9 = 0)
 *                    p0 x, p1 x row stride, p2 y, p3 y row stride, p4 n (power of two, >= 64),
 *                    p5 w, p6 z, p7 z row stride, p8 eps bits, p9 scale_n
 *   AK_EMBED         y[i] = bf16 E[tok][i]                      groups = 1
 *                    p0 E, p1 tok, p2 y, p3 hidden
 *   AK_CONV          causal depthwise conv step + SiLU, kernel 4   groups = cd / 64
 *                    p0 qkv (new input), p1 tail ([3][cd], shifted in place), p2 taps ([cd][4]),
 *                    p3 out, p4 cd
 *   AK_GDN           gated delta rule step for one value head, value dim 128   groups = vh
 *                    p0 mixed (q | k | v, q and k normalised), p1 a, p2 b, p3 a_log, p4 dt_bias,
 *                    p5 state ([vh][kd][128]), p6 out ([vh][128]), p7 kd, p8 k heads, p9 log2(vh/kh)
 *   AK_ROPE          rotate-half on the first 2*half dims of each head   groups = heads
 *                    p0 x, p1 head stride, p2 cos ([pos][half]), p3 sin, p4 pos, p5 half (<= 64)
 *   AK_ATTN          causal attention for one query head, head dim 256, sigmoid output gate
 *                    p0 q ([nh][256]), p1 k cache, p2 v cache ([pos][kvh][256]), p3 span,
 *                    p4 gate, p5 gate stride, p6 out ([nh][256]), p7 log2(nh/kvh), p8 cache row
 *                    stride (bytes), p9 scale bits     groups = nh
 *   AK_SWIGLU        y = silu(g) * u                            groups = n / 64
 *                    p0 g, p1 u, p2 y
 *   AK_COPY          y = x (dwords)                             groups = n / 64
 *                    p0 x, p1 y
 *   AK_ARGMAX        first index of the largest value           groups = 1
 *                    p0 x, p1 n, p2 out (u32)
 */
#ifndef ALLEN_KERN_H
#define ALLEN_KERN_H

#include <stdint.h>
#include "ka.h"

enum {
    AK_MATVEC, AK_MATVEC_ACC, AK_NORM_ZC, AK_NORM_GATED, AK_NORM_L2, AK_EMBED, AK_CONV,
    AK_GDN, AK_ROPE, AK_ATTN, AK_SWIGLU, AK_COPY, AK_ARGMAX, AK_COUNT
};

#define AK_S_DESC 0
#define AK_S_P0   4
#define AK_S_TGID 16
#define AK_LDS_TREE 0xC000u /* reduction scratch; attention scores sit below it */
#define AK_MAX_SPAN 12288u

/* Emits kernel `id` (variant `c` = k / 512 for the matvecs, else 0) into k; 0 ok, -1 error. */
int ak_build(ka_t *k, int id, uint32_t c);
const char *ak_name(int id);

#endif
