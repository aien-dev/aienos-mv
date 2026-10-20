Class entropy-poll (devc 1): a polled virtio device that hands the driver random bytes.
- The generated driver API is <prefix>_init_flags (bring-up) and <prefix>_read(dev, out, cap, &got):
  it returns the bytes of one completed buffer (at most cap), re-posts that buffer and notifies.
- Exactly one ring, kind virtq-split, direction device-writes (the device writes, the driver never
  gives it readable buffers). `hdr` is 0 (no header). Buffers must hold at least 64 bytes.
- `queues` and `post`/`kick` name that one ring. There is no device configuration to read.
- Features: VERSION_1 must be `require`d; ACCESS_PLATFORM must be `accept`ed and `required-by` an
  init flag named REQUIRE_ACCESS_PLATFORM (the kernel requires it so the device's DMA stays inside
  the IOMMU window). Every feature not listed is declined.
- One DMA region, no interrupt (`irq none`).
- The sandbox runs a fixed class test against your driver: bring-up with no protocol violation,
  200 byte-exact reads across ring wrap-around, refusal of a device without ACCESS_PLATFORM, of a
  device that refuses FEATURES_OK, of a reset that never completes, and of a hostile used entry.
