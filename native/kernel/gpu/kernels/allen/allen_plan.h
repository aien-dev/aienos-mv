/* native/kernel/gpu/kernels/allen/allen_plan.h -- GPU memory layout of the small Allen and the
 * kernel launches for one token. Kernel-side code: no floats.
 *
 * The model arrives as an AGP1 file (aien-train-allen `export-gpu`): a 16-word header, one kind
 * word per layer, then tensors (u32 dtype, u32 count, data). ap_parse() places every tensor and
 * every work buffer at a 256-byte aligned offset of one image; ap_token() lists the launches
 * that turn (token, position) into the next token id at m->tok. The caller writes the RoPE
 * tables (cos, sin: [max_pos][rotary / 2] f32) once; the image holds the DeltaNet states, conv
 * tails and KV caches between tokens (zero them for a new conversation).
 */
#ifndef ALLEN_PLAN_H
#define ALLEN_PLAN_H

#include <stddef.h>
#include <stdint.h>

#define AP_MAX_LAYERS 64
#define AP_MAX_DISP   512

typedef struct { int id; uint32_t c, groups; uint32_t p[12]; } ap_disp;

typedef struct {
    uint32_t in_norm, post_norm, gate, up, down;
    uint32_t qkv, z, a, b, conv, a_log, dt, norm, out, tail, state; /* DeltaNet */
    uint32_t q, k, v, o, qn, kn, kc, vc;                         /* attention */
} ap_layer;

typedef struct {
    uint32_t hidden, layers, vocab, inter, heads, kvh, hd, rot, kh, vh, kd, vd, kern, eps;
    uint32_t attn[AP_MAX_LAYERS];
    uint32_t max_pos;
    uint32_t embed, final_norm;
    ap_layer l[AP_MAX_LAYERS];
    uint32_t x, xn, qkv, z, a, b, mixed, core, y, qg, kr, vt, q, ao, g, u, act, logits, tok, cos, sin;
    uint32_t state_lo, state_hi; /* the per-conversation region (zero it to start over) */
    uint64_t size;
    /* where each tensor sits in the AGP1 file, for the copy into the image */
    uint32_t ntensor;
    uint64_t src[AP_MAX_LAYERS * 16 + 2];
    uint32_t dst[AP_MAX_LAYERS * 16 + 2], bytes[AP_MAX_LAYERS * 16 + 2];
} ap_model;

/* Reads the AGP1 header and tensor table from file[0..n); 0 ok, -1 bad file (err set). */
int ap_parse(const uint8_t *file, size_t n, uint32_t max_pos, ap_model *m, const char **err);
/* Copies the tensors from the file into the image (m->size bytes, zeroed by the caller). */
void ap_fill(const ap_model *m, const uint8_t *file, uint8_t *image);
/* Launches for one token at `pos`; returns the count (<= AP_MAX_DISP). */
int ap_token(const ap_model *m, uint32_t token, uint32_t pos, ap_disp *d);

#endif
