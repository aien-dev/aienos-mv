# AienOS on Ballentine_PC: gap map, GPU path and inventory plan (L209)

Status: a map, not a claim that anything works on this machine. Written from Drake's photo-level facts, the repository at `18e2765`, the unmerged iMac and GB10 lanes, and NVIDIA's open driver source at tag 580.173.02. No reboot, no boot-order or ESP change, no firmware load and no live device access was done to write it. Every claim is tagged OBSERVED (read from source or a file at a named place), DOCUMENTED (a vendor or standards document says so; named), INFERRED (with confidence) or UNKNOWN (with the experiment that resolves it). Objections are never "undocumented" or "huge": an unknown is a row with an experiment.

Names (Drake, 2026-10-20): Aienux is the kernel, AienOS is the system. Older files still say AIENOS for both.

## 1. Plain-English summary

Drake wants AienOS on his Windows desktop, Windows kept. The desktop is the best-placed second machine the project has: it is an ordinary x86 PC (the same family of computer as the 2017 iMac, which already ran Aienux, Allen and a GPU register console), and its graphics card is a Blackwell chip like the Spark's, only a different model.

What exists to build on: a C kernel that boots on x86 UEFI machines (proven on the iMac and in the QEMU emulator), drivers for SATA disks, NVMe disks, USB keyboards, sound, a Broadcom network chip, a screen console, and a signed "live kernel swap plus GPU register console" over the network. Almost all of it sits on unmerged branches, not on main (section 4).

What is missing, in three layers:

1. **This machine's own chips.** The motherboard, network chip, Wi-Fi, audio, storage controller and USB controllers are not known yet. One read-only PowerShell command (section 3) tells us. Nothing is planned around a guess.
2. **AMD-specific parts the iMac never needed.** The Ryzen has a different memory-protection unit (AMD-Vi, not Intel VT-d), a different power meter, a different CPU vendor string, probably no old-style serial port. No Aienux code exists for any of them.
3. **The GPU.** The RTX 5070 boots its GSP firmware through a security processor called FSP, which the Spark's chip does not have. So the Spark work (L192, L193) carries over for the firmware image family, the reset step and the page-table format, but the opening handshake with the chip is a different, additional piece (section 5). It also has 12 GB of its own memory behind a PCIe window instead of the Spark's 128 GB shared pool, so a 27B model does not fit; it must be streamed (section 5.3).

Shortest path in one line: Drake runs the inventory; we make the x86 kernel vendor-neutral on a clean base; one attended boot from a USB stick (Windows untouched, Secure Boot and BitLocker handled first); read-only disk and network; GPU discovery; GPU firmware boot only after Drake allows it. Section 7 has the ordered list, section 8 what only Drake can do.

## 2. What is known and what is not

| Fact | Source | Tag |
| --- | --- | --- |
| Name `Ballentine_PC`, Windows 11 Home 25H2, installed 2025-03-20 | Drake's photo | OBSERVED (by Drake, not re-read) |
| CPU AMD Ryzen 5 7600X, 6 cores, 4.7 GHz base, socket AM5 | Drake's photo | OBSERVED (by Drake) |
| 12 hardware threads (SMT on) | public Zen 4 spec; photo says only "6 cores" | INFERRED (high). Resolved by `Win32_Processor` in the inventory |
| The 7600X has a small built-in AMD graphics part (iGPU) | public spec | INFERRED (high). It may be hidden by the firmware when the RTX card is present; resolved by the inventory GPU section |
| RAM 32 GB | photo | OBSERVED (by Drake) |
| GPU NVIDIA GeForce RTX 5070, 12 GB, GB205 | photo; PCI id `10de:2f04` is "NVIDIA GeForce RTX 5070" in `g_nv_name_released.h:5482` of the driver at 580.173.02 | OBSERVED (id from source), the card's actual id UNKNOWN until inventory |
| 1.82 TB disk | photo | OBSERVED (by Drake). Number of disks, bus (NVMe or SATA), sector size UNKNOWN |
| Board, chipset, firmware vendor and version, NIC, Wi-Fi, audio, USB, TPM type, Secure Boot state, BitLocker state, IOMMU state, serial port, display outputs in use | nothing | UNKNOWN. Section 3 resolves every one |

Plausible guesses, labelled as guesses so nobody builds on them: a Realtek 2.5 GbE or Intel I225/I226 wired chip, a MediaTek or Intel Wi-Fi card, a Realtek audio codec, an AMD firmware TPM, an NVMe SSD. INFERRED, low to medium confidence, from what boards of this class commonly carry; none of it is read from this machine.

## 3. The inventory Drake runs (read-only)

Purpose: turn every UNKNOWN in section 2 into a file we can build against. It reads, it never writes anything except the output folder on the Desktop. It prints no serial numbers, no MAC addresses and no BitLocker keys (key *types* only). Nothing is sent anywhere; Drake sends the folder.

Where to run it: the Windows desktop itself. Right-click the Start button, choose "Terminal (Admin)" (or "Windows PowerShell (Admin)"), and paste each command, one at a time, then press Enter. Some items (Secure Boot, BitLocker, TPM) only answer to an administrator; without admin they print an `ERROR:` line and the rest still works. Expected result: command 1 ends with a line `OK pc_inventory.txt` and a few hundred lines in the file; command 2 ends with `OK n ACPI tables`. Then zip the folder `Ballentine_PC_inventory` on the Desktop and send it.

**Status of these commands: UNTESTED.** There is no PowerShell on the Spark (checked: no `pwsh` or `powershell`), so neither command has been run by anyone. They use standard Windows cmdlets and every section is wrapped so one failing section cannot stop the others. If a section prints `ERROR:`, that is itself a useful answer and we fix the command from it.

Command 1, the text inventory (one line):

```powershell
$d="$env:USERPROFILE\Desktop\Ballentine_PC_inventory"; New-Item -ItemType Directory -Force $d | Out-Null; function S($n,[scriptblock]$b){"`n===== $n ====="; try{& $b | Out-String -Width 220}catch{"ERROR: "+$_.Exception.Message}}; function T($i){$i -replace '\\[^\\]*$',''}; & { S SYSTEM {Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer,Model,SystemFamily,SystemSKUNumber,TotalPhysicalMemory | Format-List}; S BOARD {Get-CimInstance Win32_BaseBoard | Select-Object Manufacturer,Product,Version | Format-List}; S BIOS {Get-CimInstance Win32_BIOS | Select-Object Manufacturer,SMBIOSBIOSVersion,ReleaseDate,SMBIOSMajorVersion,SMBIOSMinorVersion | Format-List}; S FIRMWARE {Get-ComputerInfo -Property BiosFirmwareType,OsName,OsVersion,OsBuildNumber,WindowsVersion | Format-List}; S SECUREBOOT {"SecureBoot enabled: "+(Confirm-SecureBootUEFI); "SetupMode bytes: "+((Get-SecureBootUEFI -Name SetupMode).Bytes -join ','); "PK present bytes: "+((Get-SecureBootUEFI -Name PK).Bytes.Length)}; S CPU {Get-CimInstance Win32_Processor | Select-Object Name,Manufacturer,Family,ProcessorId,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed,L2CacheSize,L3CacheSize,VirtualizationFirmwareEnabled | Format-List}; S MEMORY {Get-CimInstance Win32_PhysicalMemory | Select-Object Manufacturer,PartNumber,Capacity,Speed,ConfiguredClockSpeed,SMBIOSMemoryType | Format-Table -AutoSize}; S GPU {Get-CimInstance Win32_VideoController | Select-Object Name,PNPDeviceID,AdapterRAM,DriverVersion,VideoProcessor,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate | Format-List}; S GPU_NVSMI {if(Get-Command nvidia-smi -ErrorAction SilentlyContinue){nvidia-smi --query-gpu=name,pci.bus_id,pci.device_id,pci.sub_device_id,vbios_version,memory.total,pcie.link.gen.current,pcie.link.gen.max,pcie.link.width.current,pcie.link.width.max,driver_version --format=csv; nvidia-smi -q -d MEMORY | Select-String -Context 0,3 'BAR1'} else {'nvidia-smi not found'}}; S DISK {Get-PhysicalDisk | Select-Object FriendlyName,MediaType,BusType,Size,LogicalSectorSize,PhysicalSectorSize,HealthStatus | Format-Table -AutoSize; Get-Disk | Select-Object Number,PartitionStyle,Size,IsBoot,IsSystem,BootFromDisk | Format-Table -AutoSize}; S PARTITIONS {Get-Partition | Select-Object DiskNumber,PartitionNumber,Type,GptType,Size,DriveLetter | Format-Table -AutoSize}; S NIC {Get-NetAdapter -Physical | Select-Object Name,InterfaceDescription,Status,LinkSpeed,DriverVersion,PnPDeviceID | Format-List}; S AUDIO {Get-CimInstance Win32_SoundDevice | Select-Object Name,Manufacturer,PNPDeviceID | Format-List}; S USB {Get-PnpDevice -PresentOnly -Class USB | ForEach-Object{ "{0} | {1}" -f $_.FriendlyName,(T $_.InstanceId) }}; S BLUETOOTH {Get-PnpDevice -PresentOnly -Class Bluetooth | ForEach-Object{ "{0} | {1}" -f $_.FriendlyName,(T $_.InstanceId) }}; S MONITORS {Get-PnpDevice -PresentOnly -Class Monitor | ForEach-Object{ "{0} | {1}" -f $_.FriendlyName,(T $_.InstanceId) }}; S TPM {Get-Tpm | Format-List TpmPresent,TpmReady,ManufacturerIdTxt,ManufacturerVersion; Get-CimInstance -Namespace root\cimv2\Security\MicrosoftTpm Win32_Tpm | Format-List SpecVersion,ManufacturerIdTxt; Get-PnpDevice -PresentOnly -Class SecurityDevices | ForEach-Object{ "{0} | {1}" -f $_.FriendlyName,(T $_.InstanceId) }}; S BITLOCKER {Get-BitLockerVolume | ForEach-Object{ "{0} status={1} protection={2} method={3} protectors={4}" -f $_.MountPoint,$_.VolumeStatus,$_.ProtectionStatus,$_.EncryptionMethod,(($_.KeyProtector | ForEach-Object{$_.KeyProtectorType}) -join ',') }}; S DMA {systeminfo | Select-String 'Kernel DMA|Virtualization|Hyper-V|Secure Boot|BIOS Version'}; S PCI {Get-PnpDevice -PresentOnly | Where-Object{$_.InstanceId -like 'PCI\*'} | ForEach-Object{ "{0} | {1} | {2} | {3}" -f $_.Class,$_.FriendlyName,$_.InstanceId,((Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_LocationInfo -ErrorAction SilentlyContinue).Data) }}; S PCI_BARS {Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID like 'PCI\\%'" | ForEach-Object{ $e=$_; Get-CimAssociatedInstance -InputObject $e -Association Win32_PnPAllocatedResource -ErrorAction SilentlyContinue | Where-Object{$_.CimClass.CimClassName -eq 'Win32_DeviceMemoryAddress'} | ForEach-Object{ "{0} | {1} | 0x{2:X}-0x{3:X} | {4} bytes" -f $e.Name,$e.PNPDeviceID,$_.StartingAddress,$_.EndingAddress,($_.EndingAddress-$_.StartingAddress+1) } }} } | Out-File "$d\pc_inventory.txt" -Encoding utf8; "OK pc_inventory.txt "+(Get-Content "$d\pc_inventory.txt").Count+" lines"
```

Command 2, the firmware tables (the machine's own description of itself; the iMac lane saved the same kind of files in `evidence/imac_boot0_2026-10-16/ACPI/`):

```powershell
$d="$env:USERPROFILE\Desktop\Ballentine_PC_inventory\ACPI"; New-Item -ItemType Directory -Force $d | Out-Null; Add-Type 'using System;using System.Runtime.InteropServices;public class FW{[DllImport("kernel32.dll")]public static extern uint EnumSystemFirmwareTables(uint s,byte[] b,uint n);[DllImport("kernel32.dll")]public static extern uint GetSystemFirmwareTable(uint s,uint id,byte[] b,uint n);}'; $A=0x41435049; $n=[FW]::EnumSystemFirmwareTables($A,$null,0); $b=New-Object byte[] $n; [FW]::EnumSystemFirmwareTables($A,$b,$n) | Out-Null; $c=0; for($i=0;$i -lt $n;$i+=4){ $id=[BitConverter]::ToUInt32($b,$i); $nm=[Text.Encoding]::ASCII.GetString($b,$i,4); $z=[FW]::GetSystemFirmwareTable($A,$id,$null,0); $t=New-Object byte[] $z; [FW]::GetSystemFirmwareTable($A,$id,$t,$z) | Out-Null; [IO.File]::WriteAllBytes("$d\$nm.BIN",$t); $c++ }; "OK $c ACPI tables"
```

Deliberately not collected: the SMBIOS table (it holds the machine serial number and UUID), disk and monitor serial numbers, MAC addresses, any BitLocker key.

### Fixtures the map expects

Delivered folder `Ballentine_PC_inventory`; once received it is committed under `evidence/pc_inventory_<date>/` (same convention as `evidence/imac_boot0_2026-10-16/`). `pc_inventory.txt` is split on its `===== NAME =====` markers; each section becomes one fixture and the parsers are written against the section, not against the whole file.

| Fixture | What it settles | Rows it feeds |
| --- | --- | --- |
| `pc_inventory.txt` section `BOARD`, `BIOS`, `SYSTEM` | board and chipset family, firmware vendor and version | PD1, PD12 |
| section `FIRMWARE`, `SECUREBOOT` | UEFI or legacy, Secure Boot on/off, setup mode, a Platform Key present | PD1, section 7 step A1 |
| section `CPU`, `MEMORY` | family/model/stepping (`ProcessorId`), thread count, RAM speed | PD2 |
| section `GPU`, `GPU_NVSMI` | the 5070's exact id and subsystem, BAR1 total (the VRAM window), PCIe link gen and width, whether the AMD iGPU is also present, which card drives the monitor | PG1, PG3, PG7 |
| section `DISK`, `PARTITIONS` | NVMe or SATA, number of disks, **sector size 512 or 4096**, where Windows and its boot partition sit | PD4, PD14 |
| section `NIC`, `BLUETOOTH` | wired and Wi-Fi chip ids, link speed | PD5, PD13 |
| section `AUDIO` | audio controller and codec ids | PD8 |
| section `USB` | which USB controllers exist (AMD in the CPU, chipset) | PD6 |
| section `TPM`, `BITLOCKER` | TPM maker and spec, whether C: is encrypted and by which protectors | PD10, section 7 step A0 |
| section `DMA` | Kernel DMA Protection state (hint about the AMD IOMMU being on) | PD3 |
| section `PCI`, `PCI_BARS` | every PCI function with its bus/device/function and every memory window with its size: the expected-device list for the first boot report (`tests/fixtures/pc_pci_expected.txt`, to be written from it, like `tests/fixtures/spark_xhci_acpi_expected.txt`) | PD12, PG1 |
| `ACPI/FACP.BIN`, `APIC.BIN`, `MCFG.BIN`, `HPET.BIN` | reset register, core list and APIC mode, PCI config window, timer: inputs the existing parsers already take | PD2, PD11, PD12 |
| `ACPI/IVRS.BIN` | the AMD IOMMU description (the AMD name for what Intel calls DMAR); needs a new parser | PD3 |
| `ACPI/TPM2.BIN` | how the TPM is reached (the start method names CRB or another interface) | PD10 |
| `ACPI/SPCR.BIN` (may be absent) | whether any serial console exists | PD9 |
| `ACPI/DSDT.BIN`, `SSDT*.BIN`, `BGRT.BIN`, others | reference for device paths; `SSDT` copies may be incomplete because Windows returns one table per name: UNKNOWN until we see the folder | PD9, PD12 |

## 4. Reuse: what exists, what is x86-generic, what is iMac-specific

Searched (2026-10-20): `git fetch`; `git branch -r` (all `imac`, `x86` branches listed below); `git diff origin/main...<branch>` per branch; sibling worktrees `~/workspace/aienos-aienos-{imac-pi,macbook,program}-map` (each holds only `docs/GB10_NATIVE_DEPENDENCY_MAP.md` so far: no sibling map exists yet to reuse); `~/workspace/aienos-gb10-gsp-host` (L192) and `aienos-gb10-gpu-mmu-sim` (L193); `~/handoffs` (`2026-10-17-imac-allen-milestone.md`, `2026-10-18-imac-bootdisk-handoff.md`, `2026-10-19-imac-gpu-bench.md`); `~/handoffs/LEDGER.tsv`. Grep on main for `IVRS`, `DMAR`, `Windows`: no IVRS parser, no DMAR parser, no Windows handling anywhere in `native/` or `scripts/`.

Where the x86 work sits (OBSERVED, `git rev-list --count`):

| Branch (origin/) | Ahead of main | Adds |
| --- | --- | --- |
| `main` | 0 | `native/kernel/x86/`: `efi_x86.c`, `kmain_x86.c`, `pci_x86.c`, `ahci_x86.c`, `smp_x86.S`, `tg3_x86.c`, `report_x86.[ch]`, `console_x86.c`, `trap.S`, `x86.h` (kernel-0 set); `native/boot/diag_x86.c` |
| `feat/imac-keyboard`, `feat/imac-bootdisk`, `feat/imac-screensaver`, `fix/imac-kbd-replug` | 37 to 53 | `xhci_x86.c` (USB keyboard), `hda_x86.c` (sound), `memory_x86.c` (Allen's partition), netboot loader and `netboot_proto` |
| `feat/imac-netboot` | 12 | UDP netboot and BSDP lab services |
| `feat/imac-gpu-bench` | 71 | `swap_x86.c` + `swap_tramp.S` (live kernel swap), `bench_x86.c` (signed GPU register console), `courier_x86.c`, `panel_x86.c` |
| `fix/imac-gpu-regs-verified`, `feat/imac-gpu-core`, `feat/imac-gpu-shader` | 60 to 63 | `native/kernel/gpu/amd_*.c` (AMD Polaris display, memory controller, rings, DMA, compute) |

No branch contains the others (checked with `git rev-list` both ways), and none is merged. That is the first obstacle for the PC: there is no single x86 base. Consolidating them onto main is the PR desk's job, not this document's; it is recorded as step 2 of the path.

What each piece is, for a Ryzen PC:

| Piece | Where | Verdict for Ballentine_PC |
| --- | --- | --- |
| UEFI x86_64 stub: memory map, GOP framebuffer, RSDP, ExitBootServices | `x86/efi_x86.c`, `boot/efi_gop.c`, `boot/efi_entry_x86_64.S` | **x86-generic.** Works on any UEFI x86_64 machine |
| CPU setup: GDT/IDT/TSS, 4-level page tables, NX and write-protect (W^X, #409), guard page, TSC measured against HPET, xAPIC timer, MADT core list, INIT-SIPI start of other cores | `x86/kmain_x86.c`, `trap.S`, `smp_x86.S` | **x86-generic in design.** Open items on AMD: it assumes xAPIC MMIO mode (comment at `kmain_x86.c` "the mode the iMac's firmware leaves it in"); x2APIC state on this board UNKNOWN; needs HPET (`ACPI/HPET.BIN` decides); 12 threads instead of 4 cores |
| Power meter (RAPL) | `kmain_x86.c` `meter_start` | **Intel-only, and it already declines cleanly** ("not an Intel processor"). AMD uses different registers: new work, low priority |
| Console: COM1 if a 16550 answers, GOP framebuffer, RAM copy | `x86/console_x86.c`, `core/fbcon.c` | **x86-generic.** COM1 is probably absent on AM5 boards (INFERRED, medium); the GOP screen is the console |
| PCI walk through the ACPI MCFG window, bus-master and memory enable | `x86/pci_x86.c` | **x86-generic walk**, with iMac-shaped glue: it matches Intel sound controllers by `vendor == 0x8086` (`pci_x86.c:296, 321`), the Ethernet chip by Broadcom id `14e4:1686`, and a display chip by "BAR0 holds the firmware framebuffer", which is how AMD cards lay out BARs. NVIDIA puts registers in BAR0 (INFERRED from the Spark: BAR0 is 64 MiB of registers, OBSERVED in `SPARK_REPLACE_LINUX.md` section 3), so the display match must be rewritten for the 5070 |
| SATA (AHCI) | `x86/ahci_x86.c` | **x86-generic** (class-code driven). Only needed if the PC has SATA disks. Write ranges are gated by an "approved disk" list naming an Apple drive (`memory_x86.c`): iMac-specific policy |
| NVMe | `native/disk/disk_nvme.c` (shared with the Spark lane, L199) | **Generic.** QEMU only; one command in flight, bounce buffers (Spark map D4). Sector size of this SSD UNKNOWN until `DISK` |
| xHCI USB keyboard, hubs | `x86/xhci_x86.c` | **x86-generic** (xHCI 1.2, class code `0c0330`, polled). AMD controllers' quirks UNKNOWN. No IOMMU confinement yet (issue #411) |
| HDA sound | `x86/hda_x86.c` | **Controller code generic, codec code iMac-specific**: Cirrus CS420x and CS8409 set-up, Apple GPIO tricks. A Realtek codec would need its own vendor words, written from Realtek/Intel HDA documents |
| Ethernet | `x86/tg3_x86.c` | **iMac-specific** (Broadcom BCM57766). Unusable here. The pattern (bring-up, transmit, UDP report) is reusable; the chip driver is new (L198 does the Spark's Realtek 10GbE; this PC's chip is UNKNOWN) |
| UDP report and signed transport | `x86/report_x86.c`, `native/net/aienos_net.c`, `aienos_sec.c` | **Generic** over any NIC driver that can send a frame |
| Netboot, live kernel swap, signed slots | `boot/net_loader.c` (on `feat/imac-netboot`, not main), `x86/swap_x86.c`, `tools/ck_imac_*` | **Generic** once a NIC driver exists. The swap file is marked "LAB BUILD ONLY"; its UDP receiver was removed and is NOT BUILT (`docs/IMAC_GPU_BENCH.md`, Status) |
| Allen's memory partition | `x86/memory_x86.c` | **Generic mechanism, iMac-specific approval list.** Writing to a PC disk needs Drake's approval entry for that disk model and a free partition. Existing partitions are never touched |
| GPU: AMD Polaris register headers, display, SDMA, compute | `native/kernel/gpu/amd_*`, `x86/panel_x86.c` | **Not usable for the 5070** (different vendor). Reusable as a pattern: register tables checked against vendor headers (`scripts/gpu_bench/check_regs.sh`), bench steps 1 to 6, the signed-register-console idea |
| GPU register console | `x86/bench_x86.c`, `bench_proto.h` | **Pattern reusable.** The allowlist is AMD display registers; an NVIDIA allowlist would be new and read-only first |
| QEMU gates | `scripts/qemu_ck_imac_*_test.sh` | Runs the x86 kernel on QEMU q35; reusable as the PC's emulator gates with a Ryzen-shaped CPU model and an AMD IOMMU device (QEMU provides an `amd-iommu` device: INFERRED, medium, from QEMU's device list; check `qemu-system-x86_64 -device help` before relying on it) |

OBSERVED about the iMac as a physical precedent (from the handoffs, not re-run here): the kernel booted from the iMac's own disk through Apple's firmware without a loader (`2026-10-18-imac-bootdisk-handoff.md`); the keyboard, sound and Ethernet transmit were exercised on the real machine over several runs; the later lab features (live swap over UDP, GPU bench over UDP) are written and NOT_RUN on hardware (`2026-10-19-imac-gpu-bench.md`). So "reuse" means reuse of code that has partly met one real x86 machine, and a PC is a second, different one.

## 5. The GPU: what the RTX 5070 shares with the GB10, and what it does not

Source: NVIDIA open-gpu-kernel-modules at tag 580.173.02 (`~/workspace/_ext/open-gpu-kernel-modules-580.173.02`; the same tag as the Spark's driver). Licence: MIT/GPL dual for the kernel module sources, readable and quotable; we read it and write our own code (nothing copied into the kernel). Manual-guard record: `docs-read` for `src/common/inc/swref/published/nv_arch.h:111,121`. All file:line figures below are from the generated chip tables in `src/nvidia/generated/`.

### 5.1 Which chips share the GB10's firmware boot path

The brief's premise ("boots GSP the same way") is half right. Chip identity from `nv_arch.h`: GB10 is **GB10B** (architecture family GB100, implementation `0x0B`); the RTX 5070 is **GB205** (architecture family GB200, implementation `0x05`). `g_hal_archimpl.h` lists both in the same shipping set.

| Step | GB10B (Spark) | GB205 (RTX 5070) | Same? | Source |
| --- | --- | --- | --- | --- |
| Firmware file | `gsp_ga10x.bin`, section `.fwsignature_gb10y` | `gsp_ga10x.bin`, section `.fwsignature_gb20x` | same file, different signature | `nv-firmware-chip-family-select.h:57-70`; GB10B maps to GB10Y, implementation below `0x0B` in GB2XX maps to GB20X |
| Hardware reset | `kgspResetHw_GB100` | `kgspResetHw_GB100` | **yes** | `g_kernel_gsp_nvoc.c:1070-1092` (the final `else`) |
| Boot ucode ("partitioned FMC", no separate Booter) | `bPartitionedFmc = TRUE` | `bPartitionedFmc = TRUE` | **yes** | `g_kernel_gsp_nvoc.c:445-456` (list includes GB10B and GB202 to GB207) |
| VBIOS extraction | stub, returns NOT_SUPPORTED, driver continues | same stub (GB205 is in neither the Tegra nor the Turing-to-Ada list) | **yes** | `g_kernel_gsp_nvoc.c:1283-1310` |
| GSP boot image archive | `GspRmBoot_GB10B` | `GspRmBoot_GB202` | **no** (different image bytes) | `g_kernel_gsp_nvoc.c:773-822` |
| Signed first-stage image ("GspRmFmcGfwProdSigned") | `_GB10B` | `_GB202` | **no** | `g_kernel_gsp_nvoc.c:916-950` |
| Concatenated FMC | `_GB100` | `_GB202` | **no** | `g_kernel_gsp_nvoc.c:851-875` |
| Who starts the first stage | **SEC2** (`PDB_PROP_KSEC2_BOOT_GSPFMC` true only for GB10B, GB20B, GB20C); **no FSP** (`PDB_PROP_KFSP_IS_MISSING` true) | **FSP** (`KFSP_IS_MISSING` false for GB205); `kfspPrepareBootCommands`/`kfspSendBootCommands` are the real `_GH100` implementations; SEC2 does not boot it | **no. This is the big difference** | `g_kern_fsp_nvoc.c:227-236, 382-415`; `g_kern_fsp_nvoc.h:602-614` (GB10B gets `return NV_OK` stubs); L191 firmware inventory section 4 |
| Wait for secure boot | none (default stub) | `kfspWaitForSecureBoot_GB202` | no | `g_kern_fsp_nvoc.c:418-436` |
| Memory reservations in the boot metadata (WPR meta) | `kgspPopulateWprMeta_GB10B`: VGA workspace 0, PMU reserved 0, non-WPR heap 0 ("No ... on iGPU") | falls to `kgspPopulateWprMeta_GH100`: the card needs those regions | **no** (dedicated-memory card) | `g_kernel_gsp_nvoc.c:975-1000`; `kernel_gsp_gb10b.c` comments |

Other chip ids on the same path as GB205 (the five GB202-family chips in the same table rows): GB202, GB203, GB206, GB207. Chips on the Spark's path: GB10B alone for SEC2 boot; GB20B and GB20C (Tegra-class Blackwell) share its SEC2 and WPR rows but use their own images. Which retail model numbers map to which chip was not read; only `10de:2f04` = RTX 5070 was confirmed from the source table here.

Plain reading: **same firmware container, same reset, same boot-ucode layout, different front door.** The Spark's L192 work encodes the SEC2 route and the GB10B image set. The 5070 needs the FSP route: messages carried over the FSP's mailbox (an MCTP/NVDM packet format, implemented in the driver as the `_GH100` functions above) carrying the "chain of trust" payload that points at the signed first-stage image in system memory. Whether the payload structure is byte-identical to what the SEC2 route sends is UNKNOWN; experiment: on the host, build both payloads from the driver's structure definitions and compare field by field (no chip needed). The firmware images for the GB202 family come out of the same driver tree the L192 extractor already reads (`ck_gsp_bindata_extract` is written for the GB10B set; extending it to the GB202 set is a host-only step).

### 5.2 Differences that change the work, with the physical reason

| Difference | Spark GB10 | Ballentine RTX 5070 | What changes | Tag |
| --- | --- | --- | --- | --- |
| Memory | 128 GB LPDDR5x, one pool shared with the CPU; "NO separate GPU memory" (`spark-hardware-manual` section 1) | 12 GB GDDR7 on the card, reached by the CPU through a PCIe window (BAR1) and by the card's own copy engines | allocator gets two pools (card memory, system memory); page-table entries need a "video memory" aperture, not only system memory; CPU access to card memory goes through BAR1, whose size depends on Resizable BAR (firmware option) | OBSERVED for the Spark; card memory is a vendor spec (12 GB confirmed by search); BAR1 size UNKNOWN: `GPU_NVSMI` shows it (`nvidia-smi -q -d MEMORY`, "BAR1") |
| Boot opener | SEC2, no FSP | FSP | section 5.1 | OBSERVED (source) |
| Memory protection | two SMMUv3 (Arm); GPU path through a confinable window is UNKNOWN (Spark map D3) | AMD IOMMU (AMD-Vi), described by the `IVRS` table, specified in AMD document 48882 (public; the Linux kernel docs name it) | a new driver for AMD-Vi (device table, command buffer, event log); QEMU can emulate it for tests | DOCUMENTED (AMD 48882: `docs.kernel.org/arch/x86/iommu.html` points to it); driver absent: OBSERVED (no `IVRS` on main) |
| Interrupts | GICv3/ITS (polling only so far) | MSI/MSI-X delivered to the local APIC | needs MSI-X table setup and IDT vectors; the IDT and APIC exist (`kmain_x86.c`); polling can carry the first runs | INFERRED (high) |
| Display | the monitor is on the firmware framebuffer; the GB10 display engine is not needed (INFERRED, medium) | the card has a real display engine with DisplayPort/HDMI outputs; the firmware's GOP driver lights one first | first boots use the GOP framebuffer on whichever device owns the monitor. A native mode-setting driver for the NVIDIA display engine is a later, separate cut. Which device drives the monitor (the 5070 or the Ryzen's iGPU) UNKNOWN until `GPU`/`MONITORS` | INFERRED (medium) |
| Bus bandwidth to host | on-package, shared | PCIe 5.0 x16 = 63.0 GB/s per direction (calc:1fae3567c390) if the slot trains at Gen5 x16 | link gen/width UNKNOWN until `GPU_NVSMI`; the card's own memory bandwidth 672 GB/s for 28 Gbps GDDR7 on 192 bits (calc:6fa996f8aa18; the 28 Gbps figure is a public spec, not verified here) | DOCUMENTED (PCIe 5.0 rate) / vendor spec |
| Compute capability | SM 12.1 (`spark-hardware-manual`) | consumer Blackwell, SM 12.0 (INFERRED, high; NVIDIA's public GPU list, not fetched here) | whether Omega's GB10 kernels run unchanged on SM 12.0 is UNKNOWN; experiment: build the 14 fixture kernels (omega#419) for both targets and compare the generated code differences | UNKNOWN |
| Power and thermal | chip managed by platform firmware | the card's fan/power control runs on the card (INFERRED, medium); the CPU side is the motherboard's | still no OS-side thermal code; read-only sensor path UNKNOWN | INFERRED |
| Driver tree files | `kern_bus_gb10b.c`, `kern_mem_sys_gb10b.c` | `kern_bus_gb202.c`, `kern_bus_gb100.c` | the existence of separate per-chip bus and memory-system files means the setup of the bus/memory block differs; the exact differences are not read yet. Experiment: line-by-line compare of those files, the first host task for L193's successor | OBSERVED (files exist); content differences UNKNOWN |

### 5.3 A 27B model does not fit in 12 GB

The Spark's model server holds 38 GB resident (`SPARK_REPLACE_LINUX.md` section 4). At 4 bits per weight a 27B-parameter model is 13.5 GB (calc:5d941ed45544), already more than 12 GB before any KV cache. So the 5070 cannot be a drop-in second Spark. It can run smaller models fully resident, or run the 27B by streaming weights layer by layer over PCIe (63.0 GB/s ceiling, if Gen5 x16). Which of these Drake wants is a product choice for later; the map only records that it is a physical size limit, not a software gap. Speed must be reported against these ceilings, not only against a previous run.

### 5.4 GPU rows

| # | Row | Aienux today | Missing | Evidence the row is done | Host-only now? |
| --- | --- | --- | --- | --- | --- |
| PG1 | Find the card and own its register window | PCI walk finds any class-`03` function; Cut B host fixtures (aienos#287) | NVIDIA match (`10de:2f04`, BAR0 = registers); read `PMC_BOOT_0`/`BOOT_42` after taking BAR0 and expect architecture family GB200, implementation 5 (`nv_arch.h` values) | the read prints the expected pair, Windows boots afterwards | Fixtures yes; read needs an attended boot and a documentation quote before any register code (manual-guard) |
| PG2 | Boot GSP through FSP | none. L192 covers SEC2 + GB10B only | FSP mailbox encoder (MCTP/NVDM), COT payload, GB202-family image set, GB20X signature section, the GB202 secure-boot wait; the mandatory RPC subset after boot (dependency map unknowns 1, 3, 5, shared with GB10) | GSP alive and a channel reply on the chip | Encoder: yes (host tests against driver structures). Chip: no |
| PG3 | Memory: two pools and BAR1 | frame allocator in the x86 kernel | video-memory aperture in page tables; BAR1 mapping; copy-engine use | one buffer written by the CPU through BAR1 and read back by the GPU | Simulated: extend L193's model with a second pool |
| PG4 | GPU page tables, channels | L193 (GB10 simulated) | per-chip bus setup differences (section 5.2 last row) | vector add with a clean teardown and reopen (cut D acceptance) | Simulated; chip no |
| PG5 | Interrupts | polling only | MSI-X programming for the card | a completion seen by interrupt, or a written reason polling suffices | Partly |
| PG6 | DMA confinement | none (no AMD-Vi) | AMD-Vi driver (row PD3); the GPU's DMA windows | the card's DMA works only inside granted ranges | Yes in QEMU with an emulated AMD IOMMU |
| PG7 | Display engine | GOP console only | native modesetting for the 5070's outputs | a picture produced by Aienux, not by the firmware | No work until needed; compute does not depend on it |
| PG8 | Compute, Omega, model | Omega cut A/E in flight on the Spark (L202) | a Blackwell-consumer target check (SM 12.0 vs 12.1), the 12 GB policy of section 5.3 | a token stream, speed against the ceilings above | CPU path yes; GPU no |

## 6. Gap table: the rest of the machine

"Host-only now?" says whether a builder can progress with host builds and QEMU alone. "NOT_RUN" means code and tests exist but no run on this machine has happened.

| # | Area | Aienux today | Missing | Evidence a native boot would need | Host-only now? |
| --- | --- | --- | --- | --- | --- |
| PD1 | Boot and firmware handoff | x86 UEFI stub; `IMACKERN.EFI`/`IMACDIAG.EFI` build targets; booted from an Apple firmware | Boot from a USB stick on this firmware; Secure Boot policy (the image is unsigned, so Secure Boot must be off or our key enrolled); firmware vendor quirks UNKNOWN | `kernel: alive` on this screen, Windows back unchanged | QEMU yes (OVMF); physical no |
| PD2 | CPU, interrupts, timers | see section 4 | `AuthenticAMD` paths, x2APIC check, 12 threads, HPET dependency, RAPL equivalent | `cpu`/`smp` lines for 6 cores / 12 threads | QEMU yes (AMD-flavoured CPU model); physical no |
| PD3 | Memory protection (AMD-Vi) | none (even Intel VT-d is absent: issue #411) | `IVRS` parser, device table, command buffer, event log. **A real risk**: if the firmware leaves AMD-Vi enabled, device DMA from a driver that never configured it could be blocked or redirected. UNKNOWN; experiment: on first boot read the IOMMU control register and report it before any DMA | IOMMU state printed; later a device granted and confined | Parser and driver logic: yes (QEMU `amd-iommu`); physical state: no |
| PD4 | NVMe disk | `disk_nvme.c`, QEMU PASS; queue depth 1 | queue depth (L199); this SSD's sector size; GPT read | GPT and a Store superblock read without a write | Yes against QEMU images at 512 B and 4 KiB; physical no |
| PD5 | Wired network | virtio-net, Pi GEM, iMac tg3 (transmit/receive) | the PC's chip driver (id UNKNOWN until `NIC`); likely candidates Realtek or Intel (INFERRED, low-medium) | one frame out of the PC seen on the LAN | Driver logic against a chip model: yes. Register documents: Intel publishes datasheets (DOCUMENTED for the I225/I226 family, to be quoted at the time); Realtek: UNKNOWN whether public, same question as L198; record DOCS SILENT and name a read-only open-source reference with its licence if so |
| PD6 | USB keyboard | `xhci_x86.c` (unmerged) | merge onto a base; AMD controller quirks | `keyboard: ready` and a typed line | QEMU yes; physical no |
| PD7 | Screen | GOP framebuffer console | which device owns the monitor | photo of the console | QEMU yes |
| PD8 | Audio | `hda_x86.c` controller code (unmerged) | Realtek codec set-up; vendor check generalised from Intel-only | a chirp, if wanted | Parsing yes; sound is Drake-optional (Spark map D13 precedent: ports first, drivers by decision) |
| PD9 | Serial console | COM1 probe | AM5 boards often have none (INFERRED, medium); a UART described by ACPI is not supported; `SPCR.BIN` says | a captured line, if a port exists | QEMU yes |
| PD10 | TPM, measured boot | none on x86. L200 builds a CRB driver for the Spark against swtpm | CRB is a memory-mapped interface (INFERRED, medium; `TPM2.BIN` start method decides) so the Spark's driver may carry over; AMD firmware TPM behaviour UNKNOWN | `TPM2_GetRandom` from the kernel | Yes against swtpm; physical no |
| PD11 | Clock, RTC, entropy | TSC calibration against HPET; `RDRAND` use UNKNOWN on x86 path | CMOS RTC read; HPET absence fallback | wall-clock read | Yes |
| PD12 | Power, reset, device discovery | FADT reset register and PM1 power-off; MCFG walk | check on AMD; sensors UNKNOWN | clean power-off and reset on this machine | QEMU yes; physical no |
| PD13 | Wi-Fi, Bluetooth | none | chip ids from `NIC`/`BLUETOOTH`; same decision as Spark L205/L206/L207 | n/a until decided | n/a |
| PD14 | Windows coexistence | none | everything in section 7 steps A0 to A1; a free disk region or a second drive before any write | Windows boots unchanged after every Aienux boot | Procedures: yes |

Userland rows (ssh, git, agents, compilers, containers) are the same boundary as the Spark (`SPARK_REPLACE_LINUX.md` section 5, class U) and are not repeated: they stay on other machines. On this PC they stay on **Windows**, which is not replaced.

## 7. Shortest ordered path

Notation: **[H]** host or QEMU only, a builder can do it now; **[A]** needs an attended boot (Windows stays the default, nothing on the internal disk changes); **[C]** needs the real chip; **[O]** Drake only; **[X]** another machine.

### Phase 0: facts

A0 **[O]** Run the two commands of section 3 and send the folder. Before anything else, in the same session, look at `BITLOCKER` in the output: if `C:` is encrypted, **save the recovery key** (Microsoft account page or Settings) *before* any firmware setting changes. Changing Secure Boot or the boot chain can make Windows ask for that key once.

### Phase 1: host work (no machine touched)

1. **[H]** Commit the fixtures under `evidence/pc_inventory_<date>/`; write `tests/fixtures/pc_pci_expected.txt` from `PCI`/`PCI_BARS`; run the existing MADT/MCFG/FADT/HPET parsers over the real tables; add an `IVRS` parser with a host test (this PC's table as the fixture).
2. **[H][X]** Get one x86 base: the PR desk merges the iMac lanes (section 4). Not done by this map.
3. **[H]** Make the x86 kernel vendor-neutral: Intel-only checks in `pci_x86.c` and `kmain_x86.c`, an NVIDIA display match, an AMD power meter or none, x2APIC detection. QEMU q35 with an AMD-flavoured CPU.
4. **[H]** NIC driver for the chip `NIC` names, against a chip model, plus the UDP report (the iMac pattern).
5. **[H]** AMD-Vi driver against QEMU's emulated device; NVMe at 4 KiB and 512 B in QEMU; CRB TPM reuse from L200 on x86.
6. **[H]** GPU, host halves: FSP boot-command encoder and GB202-family image extraction (section 5.1), second memory pool in the simulator (L193 extension), per-chip bus/memory-system file comparison.

### Phase 2: first attended boot (nothing written to the internal disk)

A1 **[A][O]** Boot Aienux **from a USB stick** chosen once in the firmware boot menu (the key shown at power-on; no boot entry created, no boot order changed). Secure Boot: if it is on, either turn it off for the test or enrol our own signing key in the firmware's key database *in addition to* Microsoft's (the second keeps Windows booting with Secure Boot on; whether this firmware allows it is UNKNOWN until `SECUREBOOT` and the firmware menu are seen). BitLocker must be suspended or its key at hand. Pass: `kernel: alive` with the device list from section 3's `PCI` fixture, a photo of the console, Windows boots normally afterwards without asking for a key. Stage with the same image and key rules as the iMac; the TEST keys are refused for hardware.
A2 **[A]** Read-only: NVMe identify and GPT read of the internal disk (no write path linked), network report out, USB keyboard, IOMMU state printed before any DMA.
A3 **[A]** GPU discovery (PG1): own BAR0, read `BOOT_0`/`BOOT_42`, expect GB200 / implementation 5. No firmware load.

### Phase 3: a home for Aienux (the Windows-kept dual boot)

A4 **[O]** Choose the storage for Allen's Store, in this order of safety: (a) the USB stick or a second drive (nothing on the Windows disk changes); (b) a new partition made by shrinking Windows' `C:` in Windows Disk Management after a backup (Drake only, reversible only by regrowing). Aienux writes only to a partition of its own type on a disk Drake approved by model (`memory_x86.c` rule), never to existing partitions.
A5 **[A]** Write path on that partition only: Store write, flush, crash recovery.
A6 **[O]** A permanent way in: add a firmware boot entry for Aienux *beside* Windows Boot Manager, never replacing it (from Windows, `bcdedit /enum firmware` lists entries; `bcdedit /set {fwbootmgr} bootsequence {id}` is the one-time-next-boot equivalent of `BootNext`: INFERRED, high, from Microsoft's bcdedit documentation; check at the time). Rollback is deleting the entry or pressing the boot-menu key. Boot order stays Windows-first until Drake changes it in writing (doctrine `NATIVE_BOOT_ONE_TIME.md` invariant 2 applies on this machine as on the Spark).

### Phase 4: GPU

7. **[A]** Native CPU inference on the PC (small model) as a baseline against the memory-bandwidth ceiling (the PC's DDR5 ceiling is not measured; do not assume).
8. **[C][O]** GSP firmware boot through FSP (PG2). Operator step: Drake allows the first firmware load and handoff on the card.
9. **[C]** First compute (vector add, teardown and reopen), then Omega cut E items in the Spark's order (L202 and successors), then model load with the 12 GB policy of section 5.3.
10. **[C]** Native display engine only if Drake wants Aienux to drive the monitor itself.

### Across the network

The PC joins the same lab path as the iMac: the Spark serves signed kernels and receives reports over the LAN (`ck_imac_netboot_push.sh`, `ck_imac_report_listen.c`, signed UDP ports 47034/47035 per `docs/IMAC_GPU_BENCH.md`). The ports are not PC-specific. This machine adds two things the others do not have: a discrete GPU with its own memory, and Windows alongside. Siblings in flight for the other machines (the iMac and Pi map, the MacBook map L208, the program map) had no document in their worktrees when this was written (`docs/GB10_NATIVE_DEPENDENCY_MAP.md` only), so cross-references are by ledger id, not by section: L208 for the MacBook, `aienos-aienos-imac-pi-map` and `aienos-aienos-program-map` for the rest. Whole-system order stays with `aien-spark/aien-architecture/CURRENT_EXECUTION_PLAN.md`; this document competes with none of it.

## 8. Operator steps (Drake only)

None of these is authorized by this document.

- Run the two inventory commands (section 3). They change nothing except creating the Desktop folder.
- Save the BitLocker recovery key if `C:` is encrypted (before any firmware setting changes).
- At A1: plug the stick, press the boot-menu key at power-on, pick the stick; handle Secure Boot as above; photograph the console. A power cycle if recovery is tested.
- Decide the storage home (A4) and approve a disk model for Allen's section.
- Allow the first firmware load and GSP handoff on the card, and each later GPU use.
- Decide whether a permanent Aienux boot entry is wanted (A6), and whether a 27B-on-12-GB streaming design or a smaller resident model is the goal (section 5.3).

## 9. What is tracked where

| Item | Place |
| --- | --- |
| This map | L209, `docs/PC_AIENOS_MAP.md`, branch `feat/aienos-pc-map` |
| Spark map (the model for this one) | L191, `docs/SPARK_REPLACE_LINUX.md` |
| GSP boot bytes (SEC2/GB10B) | L192, `feat/gb10-gsp-host` |
| GPU page tables and channel (simulated) | L193, `feat/gb10-gpu-mmu-sim` |
| RTL8127 NIC, NVMe queues, TPM+RTC, Omega cut A | L198, L199, L200, L202 |
| iMac x86 lanes | `feat/imac-*`, `fix/imac-gpu-regs-verified`, L142 |
| MacBook map | L208 |
| Whole-system order | `aien-spark/aien-architecture/CURRENT_EXECUTION_PLAN.md` |

## 10. Limits of this document

- Nothing here was run on or against Ballentine_PC. The hardware column of section 2 is Drake's photo plus public specs; every other device is UNKNOWN by design.
- The PowerShell commands are UNTESTED (no PowerShell on the Spark). Expect to fix one or two cmdlet details after the first output.
- GPU claims come from reading generated tables in the driver at 580.173.02; no register was read and no firmware loaded. "Same path" statements are about which code the driver selects per chip, not about silicon behaviour.
- The AMD IOMMU document number (48882) comes from the Linux kernel documentation's reference to it, not from reading the PDF; the Ryzen 7000 programming reference was not located (a search for its number returned nothing): UNKNOWN, resolve before writing any AMD chipset code.
- iMac physical results are quoted from handoffs, not re-run. The unmerged branches were inspected as source, not built.
- No time, effort or speed estimate appears here because none was measured. Bandwidth figures are arithmetic on vendor specs, receipted above, not measurements.
