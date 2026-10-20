#include <metal_stdlib>
using namespace metal;
kernel void matmul_naive(device const float *A [[buffer(0)]], device const float *B [[buffer(1)]],
                         device float *C [[buffer(2)]], constant uint &N [[buffer(3)]],
                         uint2 gid [[thread_position_in_grid]]) {
  uint row = gid.y, col = gid.x;
  if (row >= N || col >= N) return;
  float acc = 0.0f;
  for (uint k = 0; k < N; k++) acc += A[row * N + k] * B[k * N + col];
  C[row * N + col] = acc;
}
