#include <metal_stdlib>
using namespace metal;
kernel void vec4_k(device const float4 *a [[buffer(0)]], device const float4 *b [[buffer(1)]],
                   device float4 *c [[buffer(2)]], constant float &s [[buffer(3)]],
                   uint i [[thread_position_in_grid]]) {
  float4 x = a[i], y = b[i];
  c[i] = x * s + y * dot(x, y) + float4(length(x));
}
