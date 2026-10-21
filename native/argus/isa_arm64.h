/* isa_arm64.h -- run-time choice of the ARMv8 crypto instructions (L339).
 *
 * SHA-256 (argus/sha256.c), AES-256 (crypto/aes.c) and POLYVAL
 * (crypto/polyval.c) each carry a software path and an instruction path
 * (SHA256H/SHA256H2/SHA256SU0/SHA256SU1, AESE/AESMC, PMULL) written as
 * file-scope assembly, so the -mgeneral-regs-only kernel builds need no new
 * flags or files. The two paths give identical bytes (tests compare them).
 *
 * Detection: ID_AA64ISAR0_EL1 AES [7:4] (1 = AESE/AESD/AESMC/AESIMC,
 * 2 = also PMULL/PMULL2), SHA2 [15:12] (1 = SHA256H/H2/SU0/SU1), from the Arm
 * ARM DDI0487 register description (field positions as transcribed in Linux
 * arch/arm64/tools/sysreg). EL1 reads the register; Linux traps the EL0 read
 * and returns the sanitised value (HWCAP_CPUID, "cpuid" in /proc/cpuinfo).
 * CPUs without the extension (Pi 5 BCM2712) read 0 and keep the software path.
 *
 * Kernel (freestanding, EL1): the kernel never saves v0-v31 (core/fpu.c), and
 * EL1 threads are switched by the timer IRQ (core/m3.c), so every instruction
 * section runs with IRQ and FIQ masked and is bounded by AIENOS_ISA_MAX_BLOCKS
 * (64 blocks of 64 B or 256 of 16 B: a few microseconds). Every section
 * zeroes the vector registers it used before returning, so no key, round key
 * or hash state stays in a register. Hosted builds (Linux EL0) let the OS
 * save the registers and skip the masking.
 *
 * Constant time: AESE/AESMC/PMULL/SHA256* have data-independent timing (Arm
 * lists them under PSTATE.DIT); no branch or address depends on data.
 *
 * Build with -DAIENOS_NO_ISA to compile the software path only.
 */
#ifndef AIENOS_ISA_ARM64_H
#define AIENOS_ISA_ARM64_H

#include <stdint.h>

#if defined(__aarch64__) && defined(__ELF__) && !defined(__AARCH64EB__) && !defined(AIENOS_NO_ISA)
#define AIENOS_ISA_ARM64 1

#define AIENOS_ISA_AES 1u
#define AIENOS_ISA_PMULL 2u
#define AIENOS_ISA_SHA256 4u
#define AIENOS_ISA_MAX_BLOCKS 64u

static inline unsigned aienos_isa_detect(void)
{
    uint64_t r;
    unsigned caps = 0;
    __asm__ volatile("mrs %0, ID_AA64ISAR0_EL1" : "=r"(r));
    if (((r >> 4) & 0xf) >= 1) caps |= AIENOS_ISA_AES;
    if (((r >> 4) & 0xf) >= 2) caps |= AIENOS_ISA_PMULL;
    if (((r >> 12) & 0xf) >= 1) caps |= AIENOS_ISA_SHA256;
    return caps;
}

/* Mask IRQ+FIQ in the kernel; returns the DAIF value to restore. */
static inline uint64_t aienos_isa_enter(void)
{
#if __STDC_HOSTED__
    return 0;
#else
    uint64_t daif;
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #3" : "=r"(daif) : : "memory");
    return daif;
#endif
}

static inline void aienos_isa_leave(uint64_t daif)
{
#if __STDC_HOSTED__
    (void)daif;
#else
    __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
#endif
}

/* State per translation unit: -1 = not yet read, else the usable caps.
 * Racing first reads store the same value. */
#define AIENOS_ISA_STATE(name) static volatile int name = -1

static inline unsigned aienos_isa_caps(volatile int *st)
{
    if (*st < 0) *st = (int)aienos_isa_detect();
    return (unsigned)*st;
}

/* on = 0: force the software path; on = 1: use the instructions if the CPU
 * has them. Returns 1 when `want` is active afterwards. */
static inline int aienos_isa_select(volatile int *st, int on, unsigned want)
{
    *st = on ? (int)aienos_isa_detect() : 0;
    return (aienos_isa_caps(st) & want) == want;
}
#endif

#endif
