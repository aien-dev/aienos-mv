#include <metal_stdlib>
using namespace metal;
kernel void vadd_bounds(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
                        device float *c [[buffer(2)]], constant uint &n [[buffer(3)]],
                        uint i [[thread_position_in_grid]]) {
  if (i < n) c[i] = a[i] + b[i];
}
