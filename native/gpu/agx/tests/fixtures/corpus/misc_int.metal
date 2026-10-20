#include <metal_stdlib>
using namespace metal;
kernel void misc_int(device uint *a [[buffer(0)]], device atomic_uint *cnt [[buffer(1)]],
                     uint i [[thread_position_in_grid]], uint tg [[threadgroup_position_in_grid]]) {
  uint v = a[i];
  v = (v << 3) ^ (v >> 5) ^ (v * 2654435761u) ^ tg;
  a[i] = v & 0xffff;
  if (v & 1) atomic_fetch_add_explicit(cnt, 1, memory_order_relaxed);
}
