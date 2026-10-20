#include <metal_stdlib>
using namespace metal;
kernel void bitops(device const uint *a [[buffer(0)]], device uint *b [[buffer(1)]],
                   uint i [[thread_position_in_grid]]) {
  uint x = a[i];
  uint r = popcount(x) + clz(x) + ctz(x);
  r ^= reverse_bits(x);
  r += extract_bits(x, 3u, 5u);
  r = insert_bits(r, x, 8u, 4u);
  r += (x << 7) | (x >> 25);
  r += rotate(x, 13u);
  r += mulhi(x, r);
  b[i] = r * 3u - (x / 7u) + (x % 11u);
}
