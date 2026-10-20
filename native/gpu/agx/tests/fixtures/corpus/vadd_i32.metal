#include <metal_stdlib>
using namespace metal;
kernel void vadd_i32(device const int *a [[buffer(0)]], device const int *b [[buffer(1)]],
                     device int *c [[buffer(2)]], uint i [[thread_position_in_grid]]) {
  c[i] = a[i] + b[i];
}
