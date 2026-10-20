#include <metal_stdlib>
using namespace metal;
#define T 16
kernel void matmul_tiled(device const float *A [[buffer(0)]], device const float *B [[buffer(1)]],
                         device float *C [[buffer(2)]], constant uint &N [[buffer(3)]],
                         uint2 gid [[thread_position_in_grid]], uint2 lid [[thread_position_in_threadgroup]]) {
  threadgroup float As[T][T];
  threadgroup float Bs[T][T];
  float acc = 0.0f;
  for (uint t = 0; t < N; t += T) {
    As[lid.y][lid.x] = A[gid.y * N + t + lid.x];
    Bs[lid.y][lid.x] = B[(t + lid.y) * N + gid.x];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint k = 0; k < T; k++) acc = fma(As[lid.y][k], Bs[k][lid.x], acc);
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  C[gid.y * N + gid.x] = acc;
}
