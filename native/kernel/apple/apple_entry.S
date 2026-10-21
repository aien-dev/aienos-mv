/* apple_entry.S -- entry of the CK_APPLE=1 raw arm64 Image (header in
 * apple_image.ld). The loader (m1n1 on a Mac, QEMU's built-in loader in the
 * gate) jumps to offset 0 with x0 = device tree, MMU off, at EL2 or EL1
 * (Linux Documentation/arch/arm64/booting.rst, "Call the kernel image").
 *
 *  1. mask DAIF, keep x0, apply our R_AARCH64_RELATIVE relocations against
 *     the load address, zero .bss, take our own stack;
 *  2. at EL2, drop to EL1h with the MMU off (Aienux runs at EL1, ADR 0009):
 *     - HCR_EL2 = RW. Apple CPUs implement HCR_EL2.E2H as RAO/WI (Linux
 *       arch/arm64/include/asm/el2_setup.h: "Fruity CPUs ... have HCR_EL2.E2H
 *       implemented as RAO/WI"), so E2H is read back after the write;
 *     - E2H == 0: CPTR_EL2 0x33ff (no FP trap), CNTHCTL_EL2 bits 0-1
 *       (EL1PCTEN, EL1PCEN), SCTLR_EL1 written directly;
 *     - E2H == 1: CPTR_EL2 and CNTHCTL_EL2 take their E2H layouts (FPEN
 *       bits 20-21; EL1PCTEN, EL1PTEN bits 10-11, as Linux __init_el2_timers
 *       shifts them), and SCTLR_EL1 is only reachable as SCTLR_EL12
 *       (S3_5_C1_C0_0) because the EL1 name now means SCTLR_EL2;
 *     - eret to EL1h, DAIF masked (SPSR 0x3c5), SP_EL1 = our stack;
 *  3. call apple_main(dtb, entry_el, e2h); it never returns.
 *
 * CK_APPLE_TEST_E2H=1 (TEST-ONLY QEMU image): sets HCR_EL2.E2H on a CPU
 * where it is writable, so the QEMU gate runs the E2H == 1 path an Apple CPU
 * forces. */
#define SCTLR_EL1_MMU_OFF_LO 0x0800 /* RES1 bits 11, 20, 22, 23, 28, 29: */
#define SCTLR_EL1_MMU_OFF_HI 0x30d0 /* 0x30d00800, MMU, caches, alignment checks off */

/* EL2 -> EL1h, MMU off; x21 = HCR_EL2.E2H read back. Uses x9, x10. Shared by
 * the boot CPU and every secondary (apple_secondary_entry). */
    .macro apple_el2_to_el1 target
    mov x9, #(1 << 31)                  /* HCR_EL2.RW: EL1 is AArch64 */
#ifdef CK_APPLE_TEST_E2H
    orr x9, x9, #(1 << 34)              /* TEST-ONLY: E2H as an Apple CPU forces it */
#endif
    msr hcr_el2, x9
    isb
    mrs x9, hcr_el2
    ubfx x21, x9, #34, #1
    mrs x9, midr_el1
    msr vpidr_el2, x9
    mrs x9, mpidr_el1
    msr vmpidr_el2, x9
    msr hstr_el2, xzr
    msr vttbr_el2, xzr
    msr cntvoff_el2, xzr
    movz x10, #SCTLR_EL1_MMU_OFF_LO
    movk x10, #SCTLR_EL1_MMU_OFF_HI, lsl #16
    cbnz x21, 6f
    mov x9, #0x33ff                     /* CPTR_EL2 (E2H 0): RES1 bits, TFP 0 */
    msr cptr_el2, x9
    mov x9, #3                          /* CNTHCTL_EL2 (E2H 0): EL1PCTEN | EL1PCEN */
    msr cnthctl_el2, x9
    msr sctlr_el1, x10
    b 7f
6:  mov x9, #(3 << 20)                  /* CPTR_EL2 (E2H 1): FPEN, no FP trap */
    msr cptr_el2, x9
#ifdef CK_APPLE_MUTANT_CNTHCTL_NVHE
    mov x9, #3                          /* TEST-ONLY mutant: E2H 0 layout under E2H 1 */
#else
    mov x9, #(3 << 10)                  /* CNTHCTL_EL2 (E2H 1): EL1PCTEN | EL1PTEN */
#endif
    msr cnthctl_el2, x9
    msr S3_5_C1_C0_0, x10               /* SCTLR_EL12 */
7:  mrs x9, midr_el1
    ubfx x9, x9, #24, #8
    cmp x9, #0x61                       /* implementer 0x61: Apple */
    b.ne 71f
    /* VM_TMR_FIQ_ENA_EL2 (S3_5_C15_C1_3) V and P: let the EL1 (guest, TGE 0)
     * timers raise their FIQ. Bits from Linux irq-apple-aic.c; that EL1 needs
     * them set is INFERRED (docs/APPLE_SILICON_PLATFORM.md UNKNOWN U3). */
    mrs x9, S3_5_C15_C1_3
    orr x9, x9, #3
    msr S3_5_C15_C1_3, x9
71: isb
    mov x9, sp
    msr sp_el1, x9
    mov x9, #0x3c5                      /* EL1h, DAIF masked */
    msr spsr_el2, x9
    adr x9, \target
    msr elr_el2, x9
    isb
    eret
    .endm

    .section .text.apple_entry, "ax"
    .global _apple_start
    .type _apple_start, %function
_apple_start:
    msr daifset, #0xf
    mov x19, x0                         /* device tree */
    mrs x20, CurrentEL
    ubfx x20, x20, #2, #2               /* entry EL */
    mov x21, #0                         /* E2H seen */
    adrp x2, __image_base
    add x2, x2, :lo12:__image_base      /* load address == link offset 0 */
    adrp x3, __rela_start
    add x3, x3, :lo12:__rela_start
    adrp x4, __rela_end
    add x4, x4, :lo12:__rela_end
1:  cmp x3, x4
    b.hs 3f
    ldp x5, x6, [x3]                    /* r_offset, r_info */
    ldr x7, [x3, #16]                   /* r_addend */
    add x3, x3, #24
    and x6, x6, #0xffffffff
    cmp x6, #1027                       /* R_AARCH64_RELATIVE */
    b.ne 2f
    add x7, x7, x2
    str x7, [x2, x5]
    b 1b
2:  cbz x6, 1b                          /* R_AARCH64_NONE */
    b apple_stop                        /* unknown relocation: no console yet */
3:  adrp x3, __bss_start
    add x3, x3, :lo12:__bss_start
    adrp x4, __bss_end
    add x4, x4, :lo12:__bss_end
4:  cmp x3, x4
    b.hs 5f
    str xzr, [x3], #8
    b 4b
5:  adrp x9, apple_stack_top
    add x9, x9, :lo12:apple_stack_top
    mov sp, x9
    cmp x20, #2
    b.ne 8f

    apple_el2_to_el1 8f

8:  mov x0, x19
    mov x1, x20
    mov x2, x21
    mov x29, xzr
    mov x30, xzr
    bl apple_main
    .size _apple_start, . - _apple_start


/* Final state: interrupts masked, wait for an event forever. Used when there
 * is nothing left to do or no way to report (no console). */
    .global apple_stop
    .type apple_stop, %function
apple_stop:
    msr daifset, #0xf
9:  wfe
    b 9b
    .size apple_stop, . - apple_stop

/* Secondary CPU entry (spin-table release on Apple, PSCI CPU_ON in the QEMU
 * gate): x0..x3 are not used. MMU off, EL2 or EL1, DAIF state unknown. Finds
 * its index by MPIDR affinity in apple_smp_mpidr[0..apple_smp_n) (filled by
 * the boot CPU before any release), takes stack idx + 1 of apple_smp_stacks,
 * drops to EL1 like the boot CPU and calls apple_secondary_main(idx,
 * entry_el, e2h), which checks in and parks. Unknown MPIDR: apple_stop. */
    .balign 8
    .global apple_secondary_entry
    .type apple_secondary_entry, %function
apple_secondary_entry:
    msr daifset, #0xf
    mrs x0, mpidr_el1
    movz x1, #0xffff
    movk x1, #0x00ff, lsl #16
    movk x1, #0x00ff, lsl #32           /* Aff3 Aff2 Aff1 Aff0: 0xff00ffffff */
    and x0, x0, x1
    adrp x2, apple_smp_mpidr
    add x2, x2, :lo12:apple_smp_mpidr
    adrp x3, apple_smp_n
    ldr w3, [x3, :lo12:apple_smp_n]
    mov x4, #0
1:  cmp x4, x3
    b.hs apple_stop
    ldr x5, [x2, x4, lsl #3]
    cmp x5, x0
    b.eq 2f
    add x4, x4, #1
    b 1b
2:  mov x22, x4                         /* cpu index */
    adrp x9, apple_smp_stacks
    add x9, x9, :lo12:apple_smp_stacks
    add x10, x4, #1
    add x9, x9, x10, lsl #12            /* 4 KiB stack per secondary */
    mov sp, x9
    mrs x20, CurrentEL
    ubfx x20, x20, #2, #2
    mov x21, #0
    cmp x20, #2
    b.ne 8f
    apple_el2_to_el1 8f
8:  adrp x9, apple_vectors
    add x9, x9, :lo12:apple_vectors
    msr vbar_el1, x9
    isb
    mov x0, x22
    mov x1, x20
    mov x2, x21
    mov x29, xzr
    mov x30, xzr
    bl apple_secondary_main
    b apple_stop
    .size apple_secondary_entry, . - apple_secondary_entry

/* EL1 vectors (Arm ARM D1.10.2: 16 entries of 0x80, table 2 KiB aligned).
 * Each saves x0-x30, ELR_EL1 and SPSR_EL1 in a 272-byte frame (struct
 * apple_frame), calls apple_exception(index, frame) and restores. */
    .macro apple_vec idx
    .balign 0x80
    sub sp, sp, #272
    stp x0, x1, [sp, #0]
    mov x0, #\idx
    b apple_vec_common
    .endm

    .text
    .balign 2048
    .global apple_vectors
apple_vectors:
    apple_vec 0
    apple_vec 1
    apple_vec 2
    apple_vec 3
    apple_vec 4
    apple_vec 5
    apple_vec 6
    apple_vec 7
    apple_vec 8
    apple_vec 9
    apple_vec 10
    apple_vec 11
    apple_vec 12
    apple_vec 13
    apple_vec 14
    apple_vec 15

apple_vec_common:
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    mrs x9, elr_el1
    mrs x10, spsr_el1
    stp x30, x9, [sp, #240]
    str x10, [sp, #256]
    mov x1, sp
    bl apple_exception
    ldp x30, x9, [sp, #240]
    ldr x10, [sp, #256]
    msr elr_el1, x9
    msr spsr_el1, x10
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldp x0, x1, [sp, #0]
    add sp, sp, #272
    eret

    .bss
    .balign 16
apple_stack:
    .space 65536
apple_stack_top:
    .balign 16
    .global apple_smp_stacks
apple_smp_stacks:                       /* 64 x 4 KiB: CK_APPLE_MAX_CPUS */
    .space 64 * 4096
