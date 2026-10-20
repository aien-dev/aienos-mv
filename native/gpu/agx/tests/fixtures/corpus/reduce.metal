#include <metal_stdlib>
using namespace metal;
kernel void reduce_sum(device const float *x [[buffer(0)]], device float *out [[buffer(1)]],
                       uint i [[thread_position_in_grid]], uint l [[thread_index_in_threadgroup]],
                       uint g [[threadgroup_position_in_grid]]) {
  threadgroup float s[256];
  s[l] = x[i];
  threadgroup_barrier(mem_flags::mem_threadgroup);
  for (uint w = 128; w > 0; w >>= 1) {
    if (l < w) s[l] += s[l + w];
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (l == 0) out[g] = s[0];
}
