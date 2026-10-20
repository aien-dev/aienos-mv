#include <metal_stdlib>
using namespace metal;
kernel void half_math(device const half *a [[buffer(0)]], device half *b [[buffer(1)]],
                      device float *c [[buffer(2)]], uint i [[thread_position_in_grid]]) {
  half x = a[i];
  b[i] = x * x + (half)1.5;
  c[i] = exp2((float)x) + rsqrt(fabs((float)x) + 1.0f) + floor((float)x * 3.0f);
}
