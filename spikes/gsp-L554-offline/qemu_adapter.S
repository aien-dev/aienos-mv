# External RV64 Linux test adapter, NOT shipped Omega code or replacement firmware.
# Only stdin read, stdout write and process exit syscalls. The leaf bytes are
# copied unchanged from the pinned firmware snapshot and verified after linking.
.option norvc
# No CRT initializes gp; keep address materialization PC-relative.
.option norelax
.section .text
.global _start
_start:
    la s0, request
record:
    li s1, 0
read_more:
    li a0, 0
    add a1, s0, s1
    li a2, 32
    sub a2, a2, s1
    li a7, 63
    ecall
    bltz a0, io_error
    beqz a0, eof
    add s1, s1, a0
    li t0, 32
    bltu s1, t0, read_more
    ld a0, 0(s0)
    ld a1, 8(s0)
    ld a2, 16(s0)
    ld a3, 24(s0)
    jal ra, leaf
    la s2, output
    sd a0, 0(s2)
    li s1, 0
write_more:
    li a0, 1
    add a1, s2, s1
    li a2, 8
    sub a2, a2, s1
    li a7, 64
    ecall
    blez a0, io_error
    add s1, s1, a0
    li t0, 8
    bltu s1, t0, write_more
    j record
eof:
    bnez s1, partial
    li a0, 0
    j leave
partial:
    li a0, 2
    j leave
io_error:
    li a0, 3
leave:
    li a7, 93
    ecall
.option rvc
.balign 2
.global leaf
.global leaf_end
leaf:
    .incbin "leaf.bin"
leaf_end:
.section .bss
.balign 8
request: .skip 32
output: .skip 8
.section .note.GNU-stack,"",@progbits
