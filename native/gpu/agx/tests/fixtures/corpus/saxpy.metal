#include <metal_stdlib>
using namespace metal;
kernel void saxpy(device const float *x [[buffer(0)]], device float *y [[buffer(1)]],
                  constant float &alpha [[buffer(2)]], uint i [[thread_position_in_grid]]) {
  y[i] = alpha * x[i] + y[i];
}
