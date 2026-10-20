#include <metal_stdlib>
using namespace metal;
kernel void matmul_half(device const half *A [[buffer(0)]], device const half *B [[buffer(1)]],
                        device half *C [[buffer(2)]], constant uint &N [[buffer(3)]],
                        uint2 gid [[thread_position_in_grid]]) {
  float acc = 0.0f;
  for (uint k = 0; k < N; k++)
    acc = fma(float(A[gid.y * N + k]), float(B[k * N + gid.x]), acc);
  C[gid.y * N + gid.x] = half(acc);
}
