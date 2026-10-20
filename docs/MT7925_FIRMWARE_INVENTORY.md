# MT7925 Wi-Fi firmware inventory (L206, host work only)

Report plus one hosted checker. No firmware was loaded, no device was touched
beyond read-only inventory (`lspci`, sysfs, `modinfo`, files under
`/lib/firmware`). Any firmware download to the chip stays an OPERATOR STEP
(docs/SPARK_WIFI_MT7925.md, attended-boot rows).

Tags: [OBSERVED] read on the Spark (Linux 7.0.0-1019-nvidia) on 2026-10-20.
[SOURCE file] read in openwrt/mt76 at commit 467ae2e (2026-10-08, BSD-3-Clause-Clear),
fetched read-only into ~/workspace/_ext/mt76, not vendored. [DOCUMENTED]
quoted from a licence file. [INFERRED] reasoned from sources. [UNKNOWN]
nothing read answers it.

## 1. Plain-English summary

- The Spark's Wi-Fi chip is a MediaTek MT7925 on an AzureWave card. Like most
  Wi-Fi chips it has small processors inside that run MediaTek's own code. The
  host must send that code to the chip at every power-up: first a small
  "ROM patch", then the main "RAM code". Without them the chip only answers
  register reads.
- Two files on the Spark carry that code for Wi-Fi; a third one, in the same
  folder, is for the Bluetooth half of the chip (lane L207, not used here).
- MediaTek lets anyone redistribute the files, unchanged, for use with
  MediaTek chips, as long as they are not made part of a GPL work.
  [DOCUMENTED, section 4] AienOS can ship them as separate files next to the
  kernel, never compiled into it.
- The country rules for transmit power (the "CLC" table) are not a separate
  file: they are one region inside the RAM code file. [SOURCE mt7925/mcu.c
  mt7925_load_clc]
- A hosted checker parses both files exactly as our driver will, checks their
  pinned digests, and lists every piece that would be sent to the chip, with
  its chip address. It never sends anything: `make -C native/net mt7925-fw-real`.

## 2. Files the Linux mt7925e driver loads

`modinfo mt7925e` [OBSERVED]:

```
license:  Dual BSD/GPL
firmware: mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin
firmware: mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin
```

The names match `MT7925_ROM_PATCH` and `MT7925_FIRMWARE_WM` in the reference
[SOURCE mt7925/mt7925.h]. On disk they are zstd-compressed (`.zst`), as
Ubuntu ships them; the kernel decompresses on load. Our driver takes the raw
(decompressed) bytes.

| File (in /lib/firmware/mediatek/mt7925/) | Purpose | Compressed bytes | Raw bytes |
| --- | --- | --- | --- |
| WIFI_MT7925_PATCH_MCU_1_1_hdr.bin.zst | ROM patch for the Wi-Fi MCU | 198131 | 198112 |
| WIFI_RAM_CODE_MT7925_1_1.bin.zst | RAM code (WM firmware) + CLC table | 1163970 | 1254904 |
| BT_RAM_CODE_MT7925_1_1_hdr.bin.zst | Bluetooth firmware (L207 lane, not loaded by mt7925e) | 446856 | 446831 |

SHA-256 [OBSERVED, `sha256sum`, and `zstd -d -c | sha256sum` for raw]:

| File | SHA-256 of the .zst | SHA-256 of the raw bytes |
| --- | --- | --- |
| WIFI_MT7925_PATCH_MCU_1_1_hdr.bin | be71bbe9405161897f00e44b7589d2f47d68ec1ad53100d0def27686eb78aaf8 | 089dd0252a7eb95feed55950ad0fd9e6f751a07b4cf22273de722b73fa50d49e |
| WIFI_RAM_CODE_MT7925_1_1.bin | 41c79388cd0d1db0bff27c67938aa14b89680cd492604b0529ce3e5731abe5b9 | 7e4ed27d1e9fe21cdefda35a22da32137b2473fada350617fb603489af19346a |
| BT_RAM_CODE_MT7925_1_1_hdr.bin | 61f80fa8aae6d0839cfeed4d3bb6c6b740a08343ce8746201a642a8e86b16ec4 | d6423564bc4ffbaa5c5252844942f77a6163f3b1ff22cb51d9adf829916f7b53 |

The raw digests of the two Wi-Fi files are pinned in
native/net/tests/mt7925_fw_test.c (PIN_PATCH, PIN_RAM).

Package [OBSERVED, `dpkg -S`, `dpkg-query -W`]: `linux-firmware-mediatek`
version `20240318.git3b128b60-0ubuntu3.1`. File dates in the package are
2026-07-14.

[INFERRED] The package version names the 2024-03-18 upstream snapshot, but
the blobs inside carry 2026-06-05 build dates (section 3) and 2026-07-14
file dates: the Ubuntu "3.1" update replaced the MT7925 files with newer
MediaTek builds without changing the upstream snapshot name. Pin the files by
digest (section 1), never by package version.

## 3. Versions inside the files

Read from the file headers [OBSERVED, `zstd -d -c | head -c 32 | xxd` and
`tail -c 36 | xxd`; layouts from SOURCE mt76_connac2_mcu.h /
mt76_connac_mcu.c, parsed the same way by native/net/aienos_mt7925_fw.c]:

| File | Field | Value |
| --- | --- | --- |
| ROM patch | build date (16 bytes at 0) | `20260605184651a` |
| ROM patch | platform (4 bytes at 16) | `ALPS` |
| ROM patch | hw/sw version (be32 at 20) | 0x8a108a10 |
| ROM patch | patch version (be32 at 24) | 0xffffffff |
| RAM code | trailer chip id / eco | 0x18 / 0x00 |
| RAM code | regions / format version / flag | 5 / 2 / 0x01 |
| RAM code | firmware version (10 bytes) | `____000000` |
| RAM code | build date (15 bytes) | `20260605184805` |
| RAM code | trailer CRC (le32) | 0x8b89bb20 |

The per-region table (chip addresses, lengths, which regions are host-only
such as the CLC) is printed by `make -C native/net mt7925-fw-real`; the
receipt of the last run is in docs/SPARK_WIFI_MT7925.md section 6.

## 4. Licence terms

[DOCUMENTED, /usr/share/doc/linux-firmware-mediatek/copyright]

```
Files: mediatek/*
Copyright: MediaTek Inc.
License: Redistributable-mediatek

License: Redistributable-mediatek
 MediaTek Inc. grants permission to use and redistribute aforementioned firmware
 files for the use with devices containing MediaTek chipsets, but not as part of
 the Linux kernel or in any other form which would require these files themselves
 to be covered by the terms of the GNU General Public License or the GNU Lesser
 General Public License.
 .
 These firmware files are distributed in the hope that they will be useful, but
 are provided WITHOUT ANY WARRANTY, INCLUDING BUT NOT LIMITED TO IMPLIED WARRANTY
 OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
```

What that means for AienOS [INFERRED]: the files may ship in the AienOS boot
package as separate, unmodified files for the Spark's MediaTek chip. They
must not be linked into a GPL or LGPL work; Aienux is neither, and the files
stay outside the kernel image in any case. No source exists for them and no
signing key is involved on the host side (section 5).

The reference driver mt76 is BSD-3-Clause-Clear (`Dual BSD/GPL` module
licence on the Linux build). We read it; no line of it is copied into
native/.

## 5. Protocol facts (what the host sends)

[SOURCE mt76_connac_mcu.c mt76_connac2_load_patch / mt76_connac2_load_ram,
mt792x_core.c, mt7925/mcu.c; implemented in native/net/aienos_mt7925.c
mt7925_fw_download and tested against the chip model]

1. Restart the MCU (`MCU_CMD_RESTART_DL`), wait for `CONN_ON_MISC` bits 2:0
   to read `PWR_ON`.
2. Take the patch semaphore (`PATCH_SEM_CONTROL`, get). Answer 1 means the
   patch is already in place (skip to step 5); 2 means go ahead.
3. For each patch section (64-byte section header, type 2 = INFO): announce
   the target (`PATCH_START_REQ` or `TARGET_ADDRESS_LEN_REQ` by address), then
   send the bytes raw on the firmware-download ring (TX ring 16) in 4096-byte
   chunks. The mode word carries the encryption bits from the section header:
   the chip decrypts; the host never sees a key.
4. `PATCH_FINISH_REQ`, then release the semaphore.
5. RAM code: for each region in the trailer table that is not host-only
   (feature bit 6), announce and send it the same way.
6. `FW_START_REQ` with the override option and start address, then wait for
   `CONN_ON_MISC` bits 1:0 to read 3 (N9 ready).
7. Host-only region type 2 (CLC) is parsed by the host and sent later as a
   country-limits command after the NIC capability query.

[UNKNOWN until the attended boot] whether the chip accepts these bytes from
our rings exactly as it accepts them from Linux (same layout, same order);
how long each step takes on this card; whether the encryption mode bits are
the only per-section difference. The model checks order, addresses, lengths
and digests of what is sent; it cannot check what the chip's boot ROM does.

## 6. What is missing for a shipped image

| Item | State |
| --- | --- |
| Firmware files in the boot package | UNKNOWN: boot package lane L197 has no slot for them yet. Experiment: add the two raw files as package members with their pinned digests, verify on load before any download. |
| Decompression | Not needed if the package carries raw files (the digests above are of the raw bytes). |
| Digest check before download | Done in the host checker; the kernel path must refuse a file whose digest is not pinned (to add with the boot-package slot). |
| Bluetooth firmware | L207 lane. |
