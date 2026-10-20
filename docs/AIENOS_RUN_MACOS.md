# aienos-run on macOS

`aienos-run` boots AienOS in a virtual machine on your Mac, in one command. It uses
Apple's own Virtualization.framework, so the AienOS image runs natively on the Apple
Silicon processor: nothing is emulated. It needs no administrator password and writes
only inside your home folder. It is the macOS member of one program that also runs on
Linux (`docs/AIENOS_RUN_LINUX.md`) and Windows (ledger L261); all three share the core in
`crates/aienos-run`.

You need: a Mac with Apple Silicon (M1 or later) and macOS 13 Ventura or later. Intel
Macs are not supported.

## Try it

Open Terminal and paste:

    curl -fsSL https://www.aienos.com/try | sh

What happens:

1. The script downloads a short list of files with their sizes and SHA-256 checksums
   (the manifest), then downloads `aienos-run` for Apple Silicon and checks it against
   that list. A mismatch stops everything and nothing is installed.
2. On a Mac it also checks that the program is signed and carries Apple's
   virtualization permission (the entitlement below); a program without it is refused.
3. It installs the program as `~/.local/bin/aienos-run` and starts it.
4. `aienos-run` downloads the AienOS image named in the same list, checks its SHA-256,
   creates your Store disk and boots AienOS in this Terminal window.

What appears: a few `aienos-run:` lines (the backend, the port forward), then the AienOS
boot log scrolling by: memory, devices, the security and Store stages. After about 9
seconds on an M2 comes the `console>` prompt. Type commands there. **Ctrl+]** ends the
session and stops the VM.

Status (2026-10-20): the aienos.com/try page and the published files belong to ledger
L264 and are not live yet, so today the command stops at step 1. Every step after that
was tested on a MacBook Air M2 with local files (`scripts/aienos_run_try_test.sh`, see
Evidence).

## Commands

| Command | What it does |
| --- | --- |
| `aienos-run` | Boots AienOS. The first run downloads and verifies the image. |
| `aienos-run --update` | Downloads a newer image if the manifest names one. |
| `aienos-run --reset` | Erases the AienOS Store disk (asks first; `--yes` skips the question). |
| `aienos-run --where` | Shows where the image, the Store disk and the settings live. |

Options worth knowing: `--backend vz|qemu`, `--cpus N`, `--mem MiB`, `--image <file>`,
`--serial-log <file>`, `--receipt` (prints boot timings), `--headless`. `aienos-run --help`
lists all of them. Exit codes: 0 clean end, 1 VM failed, 2 usage error, 3 no backend can
run here, 4 download or verification failed, 5 timeout.

## Where files live

- Images and the manifest copy: `~/Library/Caches/AienOS`
- The Store disk (1 GiB, sparse, survives `--update`) and the firmware settings:
  `~/Library/Application Support/AienOS`
- The program: `~/.local/bin/aienos-run`
- `AIENOS_RUN_HOME=<dir>` moves everything under `<dir>`.

## Remove it

    rm -f ~/.local/bin/aienos-run
    rm -rf ~/Library/Caches/AienOS "$HOME/Library/Application Support/AienOS"

That is everything: nothing was installed anywhere else, and no system setting was changed.

## What it is not yet

- **A test image.** The image uses the public TEST identity and TEST keys (the boot log
  says so on every line that matters). It is for trying AienOS, not for real data.
- The `console>` prompt comes from the kernel's TEST console session, which is the
  interactive build that exists today.
- The kernel's network self-test expects a lab echo helper on your Mac's UDP port 47029.
  Without one it fails after 5 tries (`net: selftest FAIL (rc=-507)`). That costs about
  5 of the 9 seconds before the prompt and does not stop the boot.
- No graphics window: AienOS uses this Terminal window only. VZ can give the VM a
  framebuffer, but it is not wired up here yet.
- Interplane: UDP port 47040 (the signed transport) is forwarded from your Mac to AienOS,
  but only on 127.0.0.1. Other computers on your network cannot reach it yet (see
  Networking).
- The release program is not Developer ID signed or notarized (see below).
- When AienOS restarts itself, VZ reboots it in the same VM. The QEMU and KVM backends
  end the run instead.

## Gatekeeper, signing and notarization

**Why `curl | sh` gets no Gatekeeper prompt.** Gatekeeper checks files that carry the
`com.apple.quarantine` flag. Browsers and AirDrop set that flag; `curl` does not. So the
program installed by try.sh starts without a prompt. OBSERVED on macOS 26.6.2: a file
fetched with `curl -fsSL https://...` carries only `com.apple.provenance`, no
`com.apple.quarantine`, and the ad-hoc signed aienos-run installed by try.sh starts its VM
without a prompt (Evidence, row T1). Apple Silicon still runs only signed code; an ad-hoc
signature is enough for that.

**If you downloaded `aienos-run` with a browser**, macOS says it "cannot be opened because
Apple cannot check it for malicious software". Either open System Settings > Privacy &
Security and click **Open Anyway** next to the aienos-run message, or remove the flag in
Terminal:

    xattr -d com.apple.quarantine ~/Downloads/aienos-run-macos-aarch64

**The entitlement.** Virtualization.framework starts a VM only for a program signed with
`com.apple.security.virtualization` (file `crates/aienos-run/macos/aienos-run.entitlements`).
Without it the start fails and aienos-run says so. Never strip the signature.
`scripts/aienos_run_macos_sign.sh` signs a build:

    cargo build --release -p aienos-run
    scripts/aienos_run_macos_sign.sh target/release/aienos-run                       # ad-hoc
    scripts/aienos_run_macos_sign.sh target/release/aienos-run "Developer ID Application: <Team>"  # release

**Release steps (owner, not done).** These need the owner's Apple Developer account:

1. Sign with the Developer ID Application certificate, the hardened runtime, a secure
   timestamp and the entitlement (the second command above:
   `codesign --force --sign "<identity>" --options runtime --timestamp --entitlements
   crates/aienos-run/macos/aienos-run.entitlements aienos-run`).
   `com.apple.security.virtualization` needs no provisioning profile (INFERRED from
   Apple's entitlement documentation; confirm at the first Developer ID build).
2. Zip and submit: `ditto -c -k --keepParent aienos-run aienos-run-macos-aarch64.zip`, then
   `xcrun notarytool submit aienos-run-macos-aarch64.zip --keychain-profile <profile> --wait`.
3. A bare program cannot be stapled (only apps, disk images and installer packages
   can), so Gatekeeper looks the ticket up online. Publish the signed program itself,
   and its SHA-256 in the manifest, after notarization passes.

None of this was run for this branch: no notarization was attempted.

## Backends on macOS

| Backend | How | Status |
| --- | --- | --- |
| `vz` (default) | Apple Virtualization.framework, called from Rust through the Objective-C runtime (`backend/objc.rs`, `backend/vz.rs`; no extra crates) | Boots the VM-edition image to `console>`; all stages ok |
| `qemu` | QEMU with `-accel hvf` if it is installed (`brew install qemu`) | Boots the same image to `console>`; all stages ok; slower (below) |

`--backend auto` tries `vz` first, then `qemu`. That order comes from the measurement
below: VZ reached the console about 2 seconds sooner.

The VZ machine has an EFI boot loader with its own variable store, the image on a
small read-only GPT boot disk (EFI system partition with `\EFI\BOOT\BOOTAA64.EFI`), the
Store on an NVMe disk, a virtio console wired to this Terminal, virtio-net on the
runner's own user-mode network, and a virtio entropy device.

**The image has to be the VM edition** (`make full CK_VM_GUEST=1`, output
`target/native-kernel-vm`). VZ has no SMMU and the M1/M2 have no RNDR, and the normal
image refuses DMA without an SMMU and refuses security services without RNDR. The VM
edition gives DMA only to the hypervisor's own emulated devices (virtio, VZ's NVMe,
QEMU's NVMe) and only when the firmware runs at EL1, i.e. inside a VM. It takes entropy
from the virtio entropy device (`core/vguest.h`, `core/vrng.c`).

## Networking and port forwards

`vz` uses the runner's own user-mode network (`crates/aienos-run/src/usernet.rs`). The
guest sees the same addresses as with QEMU's user network: guest 10.0.2.15, gateway
10.0.2.2, which stands for your Mac's 127.0.0.1. Frames go between VZ and the runner over
a socket pair (`VZFileHandleNetworkDeviceAttachment`). The runner answers ARP and moves
UDP: the guest's outbound UDP leaves from a socket on your Mac, and forwarded host ports
deliver to the guest. TCP is not carried yet; `--backend vz` refuses TCP forwards, and
`auto` then falls back to QEMU.

The default forward is the signed transport, `udp 127.0.0.1:47040 -> guest:47040` (port
confirmed by L263, `docs/INTERPLANE_JOIN.md` section 7). A forwarded guest port keeps one
host endpoint: whatever the guest sends from 47040 leaves your Mac from host port 47040,
replies and first contact alike. So a gateway records the same port that peers dial. A
forward bound to 127.0.0.1 reaches only this Mac. LAN peers need `--forward
udp:0.0.0.0:47040:47040 --no-default-forwards`, which exposes the port to your network.

Two VZ device facts the kernel driver had to learn (OBSERVED, macOS 26.6.2):
- VZ's virtio-net refuses FEATURES_OK unless the driver accepts VIRTIO_NET_F_MTU.
- It stalls the receive queue when a buffer is smaller than 1536 bytes.
Both fixes are in `native/net`, shared by every platform.

## Measurements (MacBook Air M2, 8 GB, macOS 26.6.2)

All numbers below are MEASURED unless marked. Image:
`BOOTAA64.EFI` built by `make full CK_VM_GUEST=1 CK_CONSOLE_SESSION=1` at commit 685d9d9,
sha256 `3dc2e1fc623f3f4337b41c24b09eae877cbe8acbd9ea5dbd48d0eb91a85141b0`. Each run is a cold start
of a new VM with the persistent Store disk. "Console ready" is the first `console> ` on the
console, timed by aienos-run from process start (`--receipt`). No echo helper ran, so every
run includes the network self-test's 5-try timeout, as for a stranger. All runs: exit 0,
stages devices/security/store ok.

Command (10 runs per backend, one VM at a time):

    AIENOS_RUN_HOME=<dir> target/release/aienos-run --image BOOTAA64.EFI --backend <vz|qemu> \
        --serial-log run.log --exit-on-ready --receipt --timeout 60 --headless

| Backend | Console ready min | median | max |
| --- | --- | --- | --- |
| vz | 8662.0 ms | 8741.7 ms | 8875.9 ms |
| qemu (HVF, QEMU 11.0.3) | 10613.5 ms | 10677.0 ms | 11007.0 ms |

With the lab echo helper running (`ck_udp_echo 47029 <token>`), the network self-test
passes at once and VZ reached `console>` in 3775.2 ms (one run, MEASURED, earlier build of
the same kernel; image sha256 fe6e8614).

Idle, MEASURED: one boot per backend; 15 s after `console_session: ready`, `top -l 2 -s 10`
(the second sample is a 10 s average). MEM is top's memory footprint. Script:
`idle.sh <image> <backend> <dir>` (L265 helper, logged in the handoff).

| Backend | VM process | CPU | MEM | aienos-run CPU | aienos-run MEM |
| --- | --- | --- | --- | --- | --- |
| vz | com.apple.Virtualization.VirtualMachine | 94.2% | 148M | 0.2% | 3024K |
| qemu | qemu-system-aarch64 | 94.0% | 231M | 0.0% | 1744K |

The idle guest keeps one host core busy because the console session polls without
waiting. That is a kernel issue, not a backend one, and fixer L287 is on it. The runner
itself is idle.

## Evidence

All on the MacBook Air M2, macOS 26.6.2, branch feat/aienos-run-macos.

| Check | Command | Result |
| --- | --- | --- |
| Unit tests | `cargo test --offline -p aienos-run` | exit 0, 35 passed |
| Lint | `cargo clippy --offline -p aienos-run --all-targets -- -D warnings` | exit 0 |
| Format | `cargo fmt --all --check` | exit 0 |
| Try path | `TMPDIR=<scratch> AIENOS_RUN_BIN=target/release/aienos-run AIENOS_KERNEL_EFI=target/native-kernel-vm/full/BOOTAA64.EFI bash scripts/aienos_run_try_test.sh` | exit 0, `AIENOS_RUN_TRY: PASS`, T1 console_ready=8752.0 ms backend vz, T2-T5 PASS |
| VZ network | VM edition `full` image under `--backend vz`, lab helper `ck_udp_echo 47029 <token>` | `net: selftest PASS (rc=0)`, the reply carried the run's token |
| VZ Store | same, two boots in one run | `store: blank disk ... formatted`, then generation 2 -> 3 |
| Console input | `(sleep 12; printf 'status\r'; sleep 2; printf 'exit\r') \| aienos-run --image <console image> --backend vz --timeout 25` | exit 5 (timeout, by design); log `console_line: status source=serial`, `console_session: exit (source=serial)` |
| Interplane port | `lsof -nP -iUDP:47040` while a VZ VM runs | `aienos-run ... UDP 127.0.0.1:47040` |
| Interplane forward | `cargo test -p aienos-run usernet` | host 127.0.0.1:P reaches guest 47030/47040 and back; first contact from guest 47040 leaves from host port P |
| QEMU-HVF | console image under `--backend qemu` | stages devices/security/store ok (after the entropy, NVMe and bus-master fixes) |

Not recorded yet: the Interplane handshake. L263 has no fixture yet, and the kernel releases
its network device after the self-test, so no guest service listens on 47040 at the prompt.
