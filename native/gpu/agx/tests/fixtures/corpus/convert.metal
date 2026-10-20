#include <metal_stdlib>
using namespace metal;
kernel void convert(device const float *a [[buffer(0)]], device int *b [[buffer(1)]],
                    device half *c [[buffer(2)]], device uint *d [[buffer(3)]],
                    device float *e [[buffer(4)]], uint i [[thread_position_in_grid]]) {
  float x = a[i];
  b[i] = int(x);
  c[i] = half(x);
  d[i] = uint(fabs(x));
  e[i] = float(b[i] >> 3) + float(d[i] & 7u);
}
