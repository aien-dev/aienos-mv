#include <metal_stdlib>
using namespace metal;
kernel void select_k(device const float *a [[buffer(0)]], device const int *n [[buffer(1)]],
                     device float *b [[buffer(2)]], device int *c [[buffer(3)]],
                     uint i [[thread_position_in_grid]]) {
  float x = a[i];
  int k = n[i];
  b[i] = select(x, -x, x < 0.5f) + clamp(x, 0.0f, 1.0f) + min(x, 2.0f) + max(x, -3.0f) + fmin(x, 1.25f);
  c[i] = (k > 3 ? k : -k) + min(k, 17) + max(k, -9) + abs(k) + (x != x ? 1 : 0);
}
