Title: Track: AienOS on Ballentine_PC (Ryzen 5 7600X, RTX 5070, Windows 11 kept)

Map: `docs/PC_AIENOS_MAP.md` (L209) and `docs/PROGRAM.md` section 2.5. Nothing has run on this machine. First step for Drake (L212): run the read-only PowerShell inventory from the map and send the file. Before any firmware setting changes, save the BitLocker recovery key. Windows stays; Aienux boots from a USB stick or its own partition, never replacing Windows Boot Manager.

Rungs (rows PD1 to PD14, PG1 to PG8 are in the map):
- [ ] inventory fixtures committed and parsers run over this PC's tables (L212)
- [ ] x86 kernel made vendor-neutral on a clean base (the iMac lanes are unmerged)
- [ ] AMD-Vi driver and IVRS parser against QEMU (PD3)
- [ ] NIC driver for the chip the inventory names, UDP report (PD5)
- [ ] attended boot from a USB stick, read-only disk and network, IOMMU state printed before any DMA
- [ ] GPU: discovery of `10de:2f04` and BAR0; FSP boot encoder (GSP boots through FSP, unlike the GB10); two memory pools; vector add
- [ ] model: 27B does not fit in 12 GB, so a stated policy (stream or smaller resident model)
- [ ] storage home for Allen's Store (USB, second drive or a new partition, Drake's choice)
