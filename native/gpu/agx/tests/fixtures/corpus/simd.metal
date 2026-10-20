#include <metal_stdlib>
using namespace metal;
kernel void simd_k(device const float *a [[buffer(0)]], device float *b [[buffer(1)]],
                 device uint *c [[buffer(2)]],
                 uint i [[thread_position_in_grid]], uint lane [[thread_index_in_simdgroup]]) {
  float x = a[i];
  float s = simd_sum(x);
  float m = simd_max(x);
  float p = simd_prefix_exclusive_sum(x);
  float sh = simd_shuffle(x, (lane + 1) & 31);
  float sd = simd_shuffle_down(x, 1);
  b[i] = s + m + p + sh + sd + simd_broadcast_first(x);
  c[i] = uint(static_cast<simd_vote::vote_t>(simd_ballot(x > 0.0f)));
}
