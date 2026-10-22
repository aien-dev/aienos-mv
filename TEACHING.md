# From boot to one guarded message

I am building a small deterministic kernel because the agent must not be the machine's root of trust. The C kernel on `main` is experimental. The earlier Rust tree is legacy. My [gate map](native/kernel/GATES.md) describes the emulator checks; it does not turn a QEMU result into a physical Spark result.

At the AArch64 entry, [the handoff and EL transition](native/kernel/core/kmain.c) switch to the kernel's page tables before `ck_el1_main` calls `ck_mm_el1_ready`. That function reports the live MMU state, parses the ACPI MADT, initializes the GIC and measures an EL1 timer window. [M3](native/kernel/core/m3.c) then creates user tasks and uses `ck_sched_enqueue` and `ck_sched_tick` from [sched.c](native/kernel/core/sched.c). Its IPC syscall path calls `ck_ipc_send`, `ck_ipc_recv` and `ck_ipc_delegate` in [ipc.c](native/kernel/core/ipc.c). A handle includes a slot generation; lookup checks its rights and resource before a message or object access. The [host fixture](native/kernel/tests/test_ipc.c) sends a message from principal 0 to principal 1, delegates read rights, and proves that a write, forged generation and revoked handle are refused. A two-entry channel also refuses a third message until space is freed. This is a concrete reason to keep authority in capability tables rather than in agent text.

## Replay

At base commit `18e276590424593647cd7c39c6d16c2f9f8d6027`, use a fresh detached checkout on AArch64 Linux with gcc, make, binutils, `qemu-system-aarch64` and AAVMF firmware. No model or network service is used. Read the [boot script](scripts/qemu_ck_boot_test.sh) before running it; it uses a private QEMU gate lock and honors the machine quiet flag.

```sh
git clone https://github.com/aien-dev/aienux.git aienux-lesson
cd aienux-lesson
git checkout --detach 18e276590424593647cd7c39c6d16c2f9f8d6027
bash scripts/qemu_ck_boot_test.sh
make -s -C native/kernel "$PWD/target/native-kernel/host/test_ipc"
./target/native-kernel/host/test_ipc
```

Expected exit is 0 with `AIENOS_CK_M3: PASS` and `AIENOS_CK_M1: PASS`, including the IPC and MMU checks. The host fixture must print `test_ipc: PASS (75 checks)` with exit 0; it exercises wrong rights, forged handles and revocation. A `NOT_RUN` is not a pass. The negative case can be inspected without hardware in `test_ipc.c`; the gate log also checks for faults and panic rather than accepting an unbounded boot. My observed output and actual replay status are in [TEACHING-REPLAY.txt](TEACHING-REPLAY.txt). A QEMU pass checks the emulated boot only. Pi 5 reports, old Rust tests and native Spark qualification are separate evidence.
