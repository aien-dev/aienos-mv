# Spark RTL8127 (Aienux driver notes, L198)

Chip: Realtek RTL8127, PCI 10ec:8127 rev 05, Spark function 0007:01:00.0 (Linux interface enP7s7), 10 GbE copper.

## Documentation status

DOCS SILENT. Realtek publishes no register document for the RTL8127 (web search 2026-10-20).
The Linux netdev patch adding RTL8127ATF to r8169 says so in plain words (Fabio Baltieri, 2025-11-17,
https://lkml.iu.edu/2511.2/02128.html): "There's no public datasheet for the chip so this is just
porting over the original vendor code".

Read-only reference used for every register below: Realtek's out-of-tree Linux driver r8127
v11.014.00, the same version the Spark runs (`modinfo r8127`: `version: 11.014.00-NAPI`), mirrored at
github.com/openwrt/rtl8127 commit ec2f285, local copy `~/workspace/_ext/rtl8127-openwrt`.
`r8127_n.c:N` and `r8127.h:N` below are line numbers at ec2f285. Nothing in this file was run on the chip.

## Register facts taken from the reference (DOCUMENTED-BY-VENDOR-CODE, not confirmed on the chip)

| What | Offset / bits | Source |
|---|---|---|
| MAC address | MAC0 0x00 (4 bytes), MAC4 0x04 (2 bytes) | r8127.h:1438 |
| TX ring base | TxDescStartAddrLow 0x20, High 0x24 | r8127.h:1444 |
| ChipCmd | 0x37: StopReq 0x80, CmdReset 0x10, CmdRxEnb 0x08, CmdTxEnb 0x04 | r8127.h:1452, 1639-1642 |
| Interrupt mask/status (ISR version 1, no MSI-X) | IMR0_8125 0x38 (32 bit), ISR0_8125 0x3C (32 bit) | r8127.h IMR0_8125, r8127_n.c:4212, 4276 |
| TX doorbell | TPPOLL_8125 0x90 | r8127.h TPPOLL_8125 |
| TxConfig, RxConfig | 0x40, 0x44; RxConfig accept bits Err 0x20 Runt 0x10 Bcast 0x08 Mcast 0x04 MyPhys 0x02 AllPhys 0x01; DMA burst field shift 8 | r8127.h:1458-1459, 1657-1662, 1672 |
| Cfg9346 (config unlock) | 0x50, unlock 0xC0, lock 0x00 | r8127.h:1461, 1647-1648 |
| PHYstatus | 0x6C (16 bit): LinkStatus 0x02, FullDup 0x01, 10G 0x4000, 5G 0x1000, 2.5G 0x400, 1G 0x10, 100M 0x08, 10M 0x04; 0xffff = no device | r8127.h:1474, 1742-1754; r8127_n.c:4537-4547 |
| MAC OCP window | MACOCP 0xB0: write `((addr/2) << 16) | value | 0x80000000`; read: write `(addr/2) << 16`, then read low 16 bits | r8127_n.c:3287-3314; r8127.h:1814-1817 |
| MCUCmd_reg | 0xD3: Now_is_oob bit 7, Txfifo_empty bit 5, Rxfifo_empty bit 4 | r8127.h:1821-1823 |
| LL share FIFO ready | 0xD2 (16 bit) bit 9 | r8127_n.c:4872-4882 |
| RxMaxSize | 0xDA | r8127.h |
| CPlusCmd | 0xE0 | r8127.h |
| IntrMitigate | 0xE2 (16 bit); FIFO-empty wait wants bits 0,1,8 all set | r8127_n.c:4111-4115 |
| RX ring base | RxDescAddrLow 0xE4, High 0xE8 | r8127.h |
| RX DV gate | 0xF2 bit 3 (set = gate closed) | r8127_n.c:4015-4027 |
| Timer interrupts | 0x58, 0x5C, 0x8C, 0xF4 (all written 0) | r8127_n.c:4360-4368 |
| RX descriptor type 4 enable | 0xD8 bit 1 (EnableRxDescV4_0) | r8127_n.c:15003-15010, r8127.h:1682 |


| More registers | BACKUP_ADDR0_8125 0x19E0 (32 bit), BACKUP_ADDR1_8125 0x19E4 (16 bit): the MAC address Linux uses (it overrides MAC0) | r8127.h BACKUP_ADDR*, r8127_n.c:12418-12441 |
| TX "no close" mode (version 6 on this chip) | software tail pointer SW_TAIL_PTR0_8125BP 0x0D30 (32-bit write of the free-running producer count), hardware close pointer HW_CLO_PTR0_8125BP 0x0D34 (32-bit read, count of closed descriptors, mask 0x7FFFFFFF); enabled by TxConfig bit 6 plus 0x20E4 bit 2 | r8127_n.c:12193, 12056-12072, 16412-16424, 654-664, 14965-15001; r8127.h:852 |
| Classic TX doorbell (not used) | TPPOLL_8125 0x90, 32-bit write of BIT(queue) | r8127_n.c:16420-16422 |
| TxConfig value | (TX_DMA_BURST_unlimited 7 << 8) | (InterFrameGap 3 << 24), then bit 6 for no-close | r8127_n.c:14958-14963 |
| RxConfig value | chip table RCR: Rx_Fetch_Number_8 (bit 30) | Rx_Close_Multiple (bit 21) | RxCfg_pause_slot_en (bit 11) | EnableInnerVlan (bit 22) | EnableOuterVlan (bit 23) | (RX_DMA_BURST_512 5 << 8), plus accept bits | r8127_n.c:112-130, r8127.h:697-698, 1672-1681 |
| RxMaxSize | 0xDA (16 bit) = rms | AcceppVlanPhys 0x8000 | r8127_n.c:14470-14473 |
| Chip id | TxConfig & 0x7c800000 == 0x6C800000; TxConfig & 0x00700000: 0 = CFG_METHOD_1, 0x100000 = CFG_METHOD_2, other = METHOD_2 with "IC version unknown" | r8127_n.c:7210-7235 |
| MMIO BAR | BAR 2, at least 256 bytes (R8127_REGS_SIZE); 64-bit DMA mask | r8127_n.c init_board (13406-13440), r8127.h:700 |

OBSERVED on the Spark (Linux sysfs, read-only, 2026-10-20): BAR 0 = I/O 256 bytes, BAR 2 = 64-bit memory 0x6e900000..0x6e93ffff (256 KiB), revision 0x05.

### Descriptors

- TX descriptor: 32 bytes (`struct TxDesc` r8127.h:2056: opts1, opts2, addr u64, four reserved u32). opts1 = DescOwn bit 31 | RingEnd bit 30 (last slot) | FirstFrag bit 29 | LastFrag bit 28 | length.
- RX descriptor: the Spark's build has RSS off (module version string has no "-RSS"; vendor Makefile default `ENABLE_RSS_SUPPORT = n`), so Linux runs RX descriptor type 1: 16 bytes, opts1 @0, opts2 @4, addr @8 (r8127.h:2066, r8127_n.c:12307-12310), and clears 0xD8 bit 1. Driver hands a slot to the chip by writing opts1 = DescOwn | RingEnd(if last) | buffer size (r8127_n.c:15263-15268). On completion: length = opts1 & 0x3fff including the 4-byte FCS (r8127_n.c:17098, 17119-17126); error = RxRES bit 21 (RWT bit 22, RUNT bit 20, CRC bit 19).
- TX mode: the vendor Makefile default is `ENABLE_TX_NO_CLOSE = y` (Makefile:43), so INFERRED (medium-high) the Spark's Linux runs TX no-close. Aienux uses the same mode.

### Bring-up order in the reference (what Aienux copies, and what it leaves out)

Linux probe: exit_oob, powerup_pll, hw_init, hw_reset, get_mac_address (r8127_n.c:14328-14342). Linux open adds hw_ephy_config, hw_phy_config (PHY micro-code), hw_config, hw_start (r8127_n.c:11920-11925, 4691-4696).

Aienux copies: exit_oob (without DASH, RealWoW and the UPS-resume PHY wait), hw_reset, MAC address read and rar_set, the hw_config steps that set rings, TxConfig, RxConfig, RxMaxSize, descriptor type, no-close mode and the RX DV gate, then hw_start (ChipCmd TE|RE). Interrupts stay masked (polled driver).

Aienux leaves out, on purpose: MAC MCU patch code (hw_mac_mcu_config), PCIe EPHY tuning (hw_ephy_config_8127_1, dozens of magic values), PHY micro-code and PHY tuning (hw_phy_config), ASPM/CLKREQ, EEE, power-save, PTP, TCAM, RSS, and the long list of undocumented MAC OCP writes in hw_config (0xC0B6, 0xE614, 0xE63E, 0xC0B4, 0xEB6A, 0xEB50, 0xE056, 0xE040, 0xEA1C, 0xE0C0, 0xE052, 0xD430, 0xD40C, ...). The hope is that UEFI firmware (which has its own Realtek driver for PXE, Boot0003) already applied whatever the PHY needs. That hope is UNKNOWN until the attended boot.
