#include <metal_stdlib>
using namespace metal;
// N fixed at compile time: what an AienOS-generated kernel would look like.
kernel void matmul_fixed(device const float *A [[buffer(0)]], device const float *B [[buffer(1)]],
                         device float *C [[buffer(2)]], uint2 gid [[thread_position_in_grid]]) {
  const uint N = 64;
  float acc = 0.0f;
  for (uint k = 0; k < N; k++) acc = fma(A[gid.y * N + k], B[k * N + gid.x], acc);
  C[gid.y * N + gid.x] = acc;
}
