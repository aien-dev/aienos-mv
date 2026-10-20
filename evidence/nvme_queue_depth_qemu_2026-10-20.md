# NVMe queue depth and direct DMA: QEMU timing receipt (L199, 2026-10-20)

**Label: QEMU (TCG emulation, no KVM). These are not Spark SSD numbers.** They
measure the native/disk NVMe driver's own cost and queue use against QEMU's
emulated controller. The Spark's Samsung 144d:a810 was not touched.

## What was run

    AIENOS_BENCH_ROUNDS=3 taskset -c 0-4,10-14 ./scripts/qemu_nvme_bench.sh
    -> AIENOS_NVME_BENCH_QEMU: DONE, exit 0

- Old driver: `native/disk/disk*.{c,h}` at base 18e2765 (one command in
  flight, every request through one bounce buffer).
- New driver: tree 5d5d29b (+ uncommitted payload compare fix that does not
  touch the bench path).
- Same payload source, same QEMU command, same 128 MiB image file recreated
  before every run, same host, run ABAB (old, new, old, new, old, new).
- Each pass: write 32 MiB + flush, then read the 32 MiB back three times; a
  64-bit hash of the read data must equal the written data (`verify=ok` on
  every one of the 96 lines).
- Namespace: `logical_block_size=512` (the Spark's LBA size).
- Host: Linux 7.0.0-1019-nvidia aarch64, QEMU 8.2.2, `-cpu cortex-a57 -smp 1`.
- Host load was high and shared with other agents: loadavg 43.6 to 43.8
  before every run (host.txt). The spread inside each configuration is wide
  (below), so only medians are reported.

Raw data: `target/qemu-nvme-bench/bench.tsv` and `host.txt` (not committed;
regenerate with the command above). Timer: guest generic timer,
freq 62 500 000 Hz. MB/s = bytes x freq / ticks / 10^6.

## Results (median of 9 reads, 3 writes)

"In flight" is the most commands the driver keeps queued at once
(queue depth minus one, NVMe 1.4 section 4.1.2).

| driver | path | in flight | read median ticks | read MB/s | receipt |
|---|---|---|---|---|---|
| old (base) | bounce | 1 | 15 530 837 | 135.03 | calc:b567c675b27a43bbd977e450cd42e4711b929bd744098303bdc73d9ba59e3dd7 |
| new | direct | 1 | 2 052 972 | 1021.5 | calc:6a9191703a880994ba3cc0cc53668b4916239fb6c9ca84c708a90185163ad509 |
| new | direct | 3 | 475 043 | 4414.7 | calc:2ded06271c2867daa3a67b7dc5700b1bd0f37921e5749759a7a8c2214deca45d |
| new | direct | 7 | 412 455 | 5084.6 | calc:c26459329de04caef3e9fb479760af923f092255f51c63a5914d60458a879bb0 |
| new | direct | 15 | 420 670 | 4985.3 | calc:91556979b3074010de18044371cc2d85f74a8d7193ff44ec80e59f795498b2c4 |
| new | direct | 31 | 266 223 | 7877.4 | calc:fd073e1030d2a0930250ddd5e92191e351c909674317ad016f35dc3c335930d1 |
| new | direct | 63 | 362 394 | 5786.9 | calc:55d0d280cfa7fa5e8b1b0a69a07820791e4b33668368747414c8f1d750938282 |
| new | bounce | 31 | 8 071 180 | 259.83 | calc:aace81575da807aa55205c9501dd816552bdc8098ab7df6e84e69efd43e5df01 |

Writes (32 MiB + flush): old 23 727 528 ticks = 88.385 MB/s
(calc:9cc1a28da11598d523bd684c7d3d861bccb669d6bbdf15a74a0fe07ef4eeda1f);
new direct, 31 in flight, 881 639 ticks = 2378.7 MB/s
(calc:c9fd7663e465bab12f477f0416ea7f2b8a709abed14a462b1e0b9d5bc7e7ed1a).

Read speedup, old to new at the default depth: 15 530 837 / 266 223 = 58.34x
(calc:cae86852168f513584511e64ceb4243faa2bee01b0624fa018019b6e642eb081).

Spread of the 9 read times per row (ticks, min..max): old 8 172 067..26 348 806;
direct/1 380 241..5 011 482; direct/31 232 768..509 531; direct/63
166 950..706 328; bounce/31 7 891 628..14 242 929.

## What it shows

- OBSERVED: the new driver with direct DMA reads faster than the old one at
  every depth, and every pass verified its data.
- OBSERVED: going from 1 to 3 commands in flight is the big step; from 3 to 63
  the medians stay in the same band (4414.7 to 7877.4 MB/s) with spreads that
  overlap. The best median is at 31 in flight (queue depth 32), so
  NVME_IO_DEPTH_DEFAULT stays 32. INFERRED: QEMU's emulated controller is
  saturated from about 3 in flight on this loaded host; the exact knee is
  inside the noise.
- OBSERVED: the bounce path at 31 in flight (259.83 MB/s) is far below the
  direct path. INFERRED: the per-request copy dominates under TCG, so deeper
  queues help only when the buffer is device-visible.
- UNKNOWN: what the Spark SSD does. The emulator's controller completes
  commands at host-memcpy speed; a real SSD has its own latency and internal
  parallelism. Measuring that needs a native boot on the Spark, which this
  lane is not allowed to do.

## Hardware ceiling

Not measured here. The Spark SSD's rated and measured sequential speed is
not in this receipt; comparing against it needs the native boot run.
