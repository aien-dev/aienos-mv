# Spark AIENOS region: map + raw-disk proof (2026-10-18)
Region: /var/lib/aienos-region/region.img, 214748364800 B, immutable, zero filled; host partition nvme0n1p2 starts at LBA 612352.
REGION.MAP (ck_region_map, FIEMAP): pieces=12045 bytes=214748364800 base=612352 window=612352..8001573518 size=289152.
Probe (ck_region_probe, same dev/disk_extent.c as the kernel, raw /dev/nvme0n1 read only): 62 logical blocks (46 seeded random + first/last block of pieces 1, 100, 5000, 12044) written via the filesystem with distinct patterns, read raw through the map: REGION_PROBE: PASS checked=62 mismatches=0.
Negative control: same map against /dev/nvme0n1p2 (wrong parent) -> REGION_PROBE: FAIL parse region piece outside its window.
Map re-made after the probe writes and after blanking: byte identical. Probe spots re-zeroed (0 nonzero).
Incident: a first spot list mixed physical offsets in; 4 writes landed past EOF and grew the file sparse (filesystem writes only, nothing outside the file). Truncated back to 214748364800; map byte identical to the original afterwards.
Host test: make disk-extent-test -> DISK_EXTENT_HOST: PASS (30 checks, ASan+UBSan) and 6/6 guard mutants caught.
