#include <metal_stdlib>
using namespace metal;
kernel void atomics(device atomic_uint *a [[buffer(0)]], device atomic_int *b [[buffer(1)]],
                    device uint *out [[buffer(2)]], uint i [[thread_position_in_grid]]) {
  uint old = atomic_fetch_add_explicit(&a[0], 1u, memory_order_relaxed);
  atomic_fetch_max_explicit(&b[0], int(i), memory_order_relaxed);
  atomic_fetch_and_explicit(&a[1], ~i, memory_order_relaxed);
  uint expected = i;
  atomic_compare_exchange_weak_explicit(&a[2], &expected, i + 1, memory_order_relaxed, memory_order_relaxed);
  out[i] = old + atomic_exchange_explicit(&a[3], i, memory_order_relaxed) + expected;
}
