#include <metal_stdlib>
using namespace metal;
kernel void math_k(device const float *a [[buffer(0)]], device float *b [[buffer(1)]],
                   uint i [[thread_position_in_grid]]) {
  float x = a[i];
  b[i] = sin(x) + cos(x) + log2(x) + exp(x) + sqrt(x) + 1.0f / x + precise::divide(x, 3.0f)
       + fract(x) + ceil(x) + trunc(x) + rint(x) + pow(x, 2.5f) + tanh(x);
}
