/* swap_tramp.S -- AIENOS live kernel swap trampoline (position-independent).
 *
 * This code is copied to tramp_pa + 512 before the swap and executed there.
 * It copies the new kernel's PE sections from the staging area to their
 * linked virtual addresses (= physical, identity-mapped), then calls the new
 * kernel's entry point.
 *
 * At entry (called with a single argument, SysV x64 ABI):
 *   rdi = tramp_pa  (physical address of the tramp page)
 *
 * Tramp page layout (must match swap_x86.c constants):
 *   [0]:   section table, 8 × 32 bytes
 *          each: {src_pa u64, dst_pa u64, len u64, flags u32, _pad u32}
 *          flags: 0=copy, 1=zero (BSS); sentinel: src_pa==0 && flags==0xFF
 *   [256]: entry_point u64  (new kernel's ck_x86_kernel_entry VA)
 *   [280]: struct ck_handoff copy (232 bytes)
 *   [512]: THIS CODE (position-independent)
 *
 * All state is taken from the tramp page; no external symbols are referenced
 * after the copy overwrites the old kernel image. */

#define TRAMP_SECT_OFF   0    /* section table */
#define TRAMP_ENTRY_OFF  256  /* entry_point u64 */
#define TRAMP_HAND_OFF   280  /* handoff copy */
#define SECT_SZ          32   /* bytes per section table entry */

    .text
    .global ck_x86_swap_tramp
    .global ck_x86_swap_tramp_end
    .type ck_x86_swap_tramp, @function

ck_x86_swap_tramp:
    /* rdi = tramp_pa */
    movq  %rdi, %r12          /* r12 = tramp_pa; our data base */
    cld
    xorl  %r13d, %r13d         /* section index */

.sect_loop:
    cmpq  $8, %r13
    jge   .copy_done
    /* Compute address of section table entry: r12 + r13 * SECT_SZ */
    movq  %r13, %rcx
    imulq $SECT_SZ, %rcx
    leaq  TRAMP_SECT_OFF(%r12,%rcx,1), %rbx
    /* Check sentinel: flags byte (offset 24 in entry) == 0xFF */
    movl  24(%rbx), %eax
    cmpb  $0xff, %al
    je    .copy_done
    /* Also stop on src_pa == 0 && flags == 0 (end-of-table zero marker) */
    movq  0(%rbx), %rsi       /* src_pa */
    movl  24(%rbx), %eax      /* flags */
    testq %rsi, %rsi
    jnz   .have_src
    testl %eax, %eax
    jz    .copy_done           /* src_pa=0, flags=0 => end */
.have_src:
    movq  8(%rbx), %rdi       /* dst_pa */
    movq  16(%rbx), %rcx      /* len */
    testl $1, %eax
    jnz   .do_zero
.do_copy:
    rep movsb
    jmp   .sect_next
.do_zero:
    xorl  %eax, %eax
    rep stosb
.sect_next:
    incq  %r13
    jmp   .sect_loop

.copy_done:
    /* Flush caches so the copies are coherent before jumping. */
    wbinvd
    /* Call entry_point(handoff):
     *   rdi = &handoff  (SysV first argument)
     *   call *entry_point */
    leaq  TRAMP_HAND_OFF(%r12), %rdi   /* rdi = &new handoff */
    movq  TRAMP_ENTRY_OFF(%r12), %rax  /* rax = entry_point */
    callq *%rax
    /* Should never return; halt if it does. */
.halt:
    cli
    hlt
    jmp .halt

ck_x86_swap_tramp_end:
    .size ck_x86_swap_tramp, . - ck_x86_swap_tramp
