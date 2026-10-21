# aienos-devc: the device contract compiler (ADR 0021)

A `.devc` file describes a device (registers, rings, the order of steps); this tool checks it and writes the driver C,
and for class `net-poll` also the chip model, fuzzer, documentation page and Omegese form.

Classes:
- `net-poll` (virtio-pci, virtq-split): native/net/contract/virtio_net.devc, L215.
- `gpu-chan` (pci-bar, gpfifo): a GPU command channel; src/gpu.rs. First contract:
  native/kernel/gpu/contract/gb10_chan.devc (L332, on branch feat/own-gpu-driver), compiled to C with the API of
  native/kernel/gpu/gpu_chan.h. The checker refuses unsafe orders (doorbell before publish, missing fences, no ring-room
  check, GPU-cached memory the CPU writes or polls, a bound wider than its hardware field, teardown before idle).

Usage: `aienos-devc gen|check CONTRACT OUTDIR`, `stats CONTRACT`, `infer TRACE DEVICE`, `covers CONTRACT TRACE`.
Tests: `cargo test -p aienos-devc` (tests/gb10_chan.devc is a copy of the GB10 contract used by the gpu-chan tests).
