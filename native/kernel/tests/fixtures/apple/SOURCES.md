# Apple device-tree test fixtures (TEST INPUT ONLY)

These blobs are inputs to `native/kernel/tests/test_apple_fdt.c` and nothing else. They are never compiled
or linked into any Aienux image. Rebuild: `scripts/apple_dt_fixtures.sh <asahi-linux> <asahi dtc>`.

| File | Board (model string in the tree) | SoC |
| --- | --- | --- |
| t8103-j313.dtb | Apple MacBook Air (M1, 2020) | t8103 M1 |
| t8112-j413.dtb | Apple MacBook Air (13-inch, M2, 2022) | t8112 M2 |
| t8122-j613.dtb | Apple MacBook Air (13-inch, M3, 2024) | t8122 M3 |
| t8132-j713.dtb | Apple MacBook Air (13-inch, M4, 2024) | t8132 M4 |
| t6000-j314s.dtb | Apple MacBook Pro (14-inch, M1 Pro, 2021) | t6000 M1 Pro |
| t6020-j414s.dtb | Apple MacBook Pro (14-inch, M2 Pro, 2023) | t6020 M2 Pro |
| t6031-j514c.dtb | Apple MacBook Pro (14-inch, M3 Max, 16 CPU cores, Nov 2023) | t6031 M3 Max |
| qemu-virt.dtb | QEMU 8.2.2 `-M virt,virtualization=on,gic-version=3 -cpu cortex-a76 -smp 4 -m 1024` (dumpdtb, repacked) | none (linux,dummy-virt) |

- Source: https://github.com/AsahiLinux/linux commit 94fb23346d522edf53722357c426a3e58030beea (2026-09-12),
  `arch/arm64/boot/dts/apple/<board>.dts` and the .dtsi files it includes, plus `include/dt-bindings`.
  Licence of every Apple .dts/.dtsi used: `SPDX-License-Identifier: GPL-2.0+ OR MIT` (OBSERVED in each file header).
- Compiled with `cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp` then the dtc built from the same tree's
  `scripts/dtc` (DTC 1.7.2-g53373d13; the Asahi trees use floating-point cells that Ubuntu's dtc 1.7.0 refuses).
- The seven Apple blobs rebuild byte for byte (OBSERVED 2026-10-21). qemu-virt.dtb does not: QEMU writes a random
  /chosen kaslr-seed and rng-seed on every dump, so its hash changes per dump; the tests read only its structure.

sha256 (Apple blobs):

    1737a97cc7c9b4e3da717a3227e0336731f5b6b66f209129811cb5d660acef2f  t6000-j314s.dtb
    6514cc4320d600c59dd1545a9722c72e6512c3ae81a1e3ae7ff5a6478e7a1442  t6020-j414s.dtb
    ac87f3d64699c4a5bc9f9a18a86bca4a62dc84d94edc13cf3c12c69c877feb4f  t6031-j514c.dtb
    596420449c9b8d30a29392a344398026b5977877caebeecce1c54ac869ba7848  t8103-j313.dtb
    8e6c9d4ac982d3ce3d229c1b1f680c9a801f81e980a6dad83008f8cfe6e42804  t8112-j413.dtb
    b0f6b725a8c2b62f073034249b2baabe2fff0480cad0927e552fc92c9e8a7e34  t8122-j613.dtb
    9f6ab3cfa738216e25f81eb258750cddd354e0f6a315c3419df8f1f3792722b5  t8132-j713.dtb
