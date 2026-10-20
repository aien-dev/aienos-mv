# AienOS PC edition 0.1.0

This is a first, small version of AienOS for an ordinary Windows PC. You can try it two ways:

- **In a window on Windows** (a virtual machine, a pretend computer inside your real one). Nothing about Windows changes.
- **From a USB stick** (the "live" stick). The PC starts AienOS instead of Windows for as long as the stick is in. Windows and its disk are not touched; take the stick out and restart, and Windows is back.

The kernel (the core program that runs the hardware) is called **Aienux**; the whole system is **AienOS**.

## What it is, and what it is not yet

What it does today:

- Starts on any 64-bit PC or virtual machine that boots the modern way (UEFI), Intel or AMD.
- Finds the processor cores, memory, timers, disks, network card and keyboard, and prints a report of what it found.
- Gives you a small text console. Type `help`, `status`, `poweroff` or `reboot`.
- Reads disks to report what is on them. **It never writes to any disk**, and it never changes a firmware setting.

What it does **not** do yet:

- No graphics card use (your RTX 5070 is left alone), no AI model running on the PC, no programs to run, no saving anything.
- On the real PC (the USB stick), typing may not work yet: the USB keyboard driver is not in this edition. The screen report works; the power button turns it off.
- The "virtual machine" version is the better way to try typing commands.

How it was tested: in QEMU (a virtual-machine program) pretending to be an AMD Zen 4 processor, the same family as your Ryzen 5 7600X. **No real PC has run it yet.** Your PC would be the first. One part could not be tried in QEMU: the newer way AMD processors handle interrupts (called x2APIC). The emulator on our test machine cannot offer it, so that code is written but untested until a real PC or a faster virtual machine runs it.

## Download

From the GitHub release page of `aien-dev/aienos` (tag `aienos-pc-v0.1.0`):

| File | What it is for | Size |
|---|---|---|
| `aienos-pc-live-x86_64.iso` | The USB stick (or a DVD) | about 22 MB |
| `aienos-pc-vm-x86_64.qcow2` | The virtual machine, for the `aienos-run` launcher | under 1 MB |
| `aienos-pc-vm-x86_64.vhdx` | The virtual machine, for Hyper-V (not on Windows 11 Home) | a few MB |
| `aienos-pc-vm-x86_64.img` | The same virtual machine disk, plain (for other VM programs) | 64 MB |
| `aienos-pc-manifest.json` and `.sig` | The list of files with their fingerprints, signed | small |
| `SHA256SUMS` | The fingerprints, one line per file | small |

## Check the download is genuine

Every file has a fingerprint (a SHA-256 number: 64 letters and digits that change completely if one byte of the file changes). Compare yours with the line in `SHA256SUMS`.

1. Open the Start menu, type **PowerShell**, and open **Windows PowerShell** (not as administrator; nothing is changed).
2. Type this and press Enter (change the file name for each file you downloaded):

   ```
   Get-FileHash $HOME\Downloads\aienos-pc-live-x86_64.iso -Algorithm SHA256
   ```

3. You see a line with a long `Hash`. It must match the line for that file in `SHA256SUMS` (Windows prints capitals, the file lower case: that is fine). If it does not match, delete the file and download it again.

About the signature: the manifest is signed, but **with a public test key**, because the project's release key does not exist yet. The signature proves the format works; it does not prove who made the file. Trust comes from the fingerprints matching the release page.

## Run it in a window on Windows (virtual machine)

### With the AienOS launcher (aienos-run), the recommended way

The launcher `aienos-run-windows-x86_64.exe` is being built alongside this edition (a separate piece of work). It downloads `aienos-pc-vm-x86_64.qcow2`, checks its fingerprint against the manifest, and opens AienOS in a window, like WSL does for Linux but without WSL. It uses the **Windows Hypervisor Platform**, a part of Windows that lets programs run virtual machines.

- Microsoft documents that this platform exists since the April 2018 update. Its page does not say which editions have it. Users report it in the Windows Features list on Windows 11 Home. On your PC: open Start, type **Turn Windows features on or off**, and look for **Windows Hypervisor Platform**. Tick it, press OK, restart when asked. This changes no files of yours and can be unticked later.
- What you see in the window: a few dozen lines of report, then `aienos: console ready` and the prompt `aienos> `. Type `help` and press Enter.
- To stop: type `poweroff`, or close the launcher (it presses the virtual power button and AienOS turns itself off).

Until the launcher is published, this way is not available. The launcher's own instructions will be on the same release page.

### With Hyper-V (only on Windows 11 Pro, Enterprise or Education)

Microsoft: "The Hyper-V role can't be installed on Windows 10 Home or Windows 11 Home." Your PC runs Windows 11 Home, so skip this part unless the PC is upgraded. On Pro:

1. Hyper-V Manager, **New, Virtual Machine**. Generation: **Generation 2**. Memory: 1024 MB. Network: Default Switch. Disk: **Use an existing virtual hard disk**, pick `aienos-pc-vm-x86_64.vhdx`.
2. Before starting it: **Settings, Security**, untick **Enable Secure Boot** (AienOS is not signed with Microsoft's key yet).
3. Start and Connect. The report and the `aienos>` prompt appear in the window. Hyper-V's window has no PS/2 keyboard and no virtio keyboard, so typing there may not work yet; the screen report does.
4. To stop: **Action, Shut Down** (presses the virtual power button), or type `poweroff`.

This is NOT TESTED on Hyper-V yet (only on QEMU).

## Try it on the real PC from a USB stick

This starts the PC from the stick. Nothing is installed, no boot setting is saved, and the Windows disk is not written. Windows' own core is not involved at all: the PC's firmware (the setup program built into the motherboard) starts AienOS directly.

### Before you start (do this first, every time)

1. **Save your BitLocker recovery key.** Windows 11 often encrypts the disk (BitLocker). Changing the Secure Boot setting (step 3 below) can make Windows ask for that key once on the next start. Get it from https://account.microsoft.com/devices/recoverykey (sign in with the Microsoft account used on the PC), and write it on paper or keep it on your phone. If there is no key listed and Windows says "Device encryption off", you do not need one.
2. A USB stick of 1 GB or more, **which will be erased**.

### Write the stick

1. Download Rufus from https://rufus.ie (a small free program that writes sticks).
2. Plug in the stick. In Rufus: **Device** = your stick, **Boot selection** = `aienos-pc-live-x86_64.iso`. Press **START**.
3. If Rufus asks how to write the image, choose **Write in DD Image mode** (byte for byte). This erases the stick.
4. When it says READY, close Rufus. (If the stick does not start later, write `aienos-pc-vm-x86_64.img` the same way: it is the same system as a plain disk.)

### Start the PC from the stick

1. Restart the PC and press the **firmware setup key** while it starts (usually **Delete** or **F2** on desktop motherboards).
2. Find **Secure Boot** (often under Boot or Security) and set it to **Disabled**. AienOS is not signed with a key the PC trusts yet, so with Secure Boot on the PC refuses to start it. Save and exit.
3. While it restarts, press the **boot menu key** (one-time choice of what to start from; usually **F8** on ASUS, **F11** on MSI and ASRock, **F12** on Gigabyte; the start-up screen often says which). Pick the USB stick, the entry that starts with **UEFI:**.
4. AienOS prints its report on the screen. The line `secure_boot: SecureBoot=0` records the setting it found. A photo of the screen is the most useful thing you can send back.
5. To leave: press the PC's power button once (AienOS turns itself off), or hold it.

### Go back to Windows

1. Take the stick out.
2. Restart, enter firmware setup again, set **Secure Boot** back to **Enabled**, save and exit.
3. Windows starts as before. If it asks for the BitLocker recovery key, type the key you saved; it asks only once.

Nothing else needs undoing: AienOS wrote nothing to the PC.

## For the curious: how the files are made

One command builds everything from one version of the source code: `bash scripts/pc_edition_build.sh` (or `make -C native/kernel pc-edition`). It is reproducible: building twice gives byte-identical files (`scripts/pc_edition_repro.sh` checks this). The test that boots all four ways in QEMU with an AMD processor model is `scripts/qemu_ck_pc_edition_test.sh`. The image files are written by the project's own tool (`native/kernel/tools/ck_pc_image.c`); outside tools are used only to double-check them.
