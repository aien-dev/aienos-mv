/* tpm_crb.h -- TPM 2.0 over the CRB (Command Response Buffer) interface, L200.
 *
 * Portable: no kernel header, no clock, no global state. The caller supplies
 * the register and buffer accessors and a microsecond delay, so the same code
 * runs in the kernel (MMIO), in the host tests (a register-level simulator
 * that forwards each command to a real swtpm) and in the x86 QEMU gate.
 *
 * Register map and handshake: TCG PC Client Platform TPM Profile (PTP)
 * chapter 5, quoted in tpm_crb.c. TPM2 command layout: TCG TPM 2.0 Library
 * Part 3 (command codes) and Part 2 (structures). ACPI TPM2 table: TCG ACPI
 * specification. Return convention of every function that talks to the TPM:
 * 0 ok, < 0 a CK_TPM_E_* driver error, > 0 the TPM's own response code
 * (TPM_RC), so a TPM refusal is never mistaken for a driver fault.
 *
 * Limits (stated, not hidden): locality 0 only; the command and response
 * buffers must lie inside the 4 KiB CRB window (the usual shape; anything
 * else is refused); start method 7 (plain CRB) only: the ACPI-start and
 * Arm SMC/FF-A start methods are refused by ck_tpm2_acpi_parse. No interrupt,
 * no cancel, no sessions beyond one password session for NV read. Which start
 * method the Spark's TPM2 table carries is UNKNOWN (the table is root-only
 * and no capture of it is in the repo). */
#ifndef AIENOS_CK_TPM_CRB_H
#define AIENOS_CK_TPM_CRB_H

#include <stddef.h>
#include <stdint.h>

/* Driver errors (negative). */
#define CK_TPM_E_ARG      (-1)  /* bad argument or buffer too small */
#define CK_TPM_E_NO_TPM   (-2)  /* registers read as all ones or never became valid */
#define CK_TPM_E_LOCALITY (-4)  /* locality 0 was not granted */
#define CK_TPM_E_LAYOUT   (-5)  /* command/response buffer outside the CRB window or too small */
#define CK_TPM_E_TIMEOUT  (-6)  /* a handshake step did not finish in time */
#define CK_TPM_E_FATAL    (-7)  /* CRB status reports a fatal TPM error */
#define CK_TPM_E_PROTO    (-8)  /* malformed response (short, bad size, bad tag) */
#define CK_TPM_E_START    (-9)  /* ACPI start method is not plain CRB (7) */
#define CK_TPM_E_ACPI     (-10) /* malformed TPM2 table */

/* Accessors. off is a byte offset from the CRB window base (the control
 * area address in the TPM2 table minus 0x40). */
struct ck_tpm_crb_ops {
    uint32_t (*rd32)(void *ctx, uint32_t off);
    void (*wr32)(void *ctx, uint32_t off, uint32_t v);
    void (*rd_buf)(void *ctx, uint32_t off, uint8_t *dst, size_t n);
    void (*wr_buf)(void *ctx, uint32_t off, const uint8_t *src, size_t n);
    void (*delay_us)(void *ctx, uint32_t us);
    void *ctx;
};

struct ck_tpm_crb {
    struct ck_tpm_crb_ops ops;
    uint16_t vid, did;      /* PTP 34h, 36h */
    uint32_t interface_id;  /* PTP 30h */
    uint32_t cmd_off, cmd_size;
    uint32_t rsp_off, rsp_size;
    uint32_t retries; /* commands resubmitted after TPM_RC_RETRY or TPM_RC_TESTING (counter, never reset) */
};

/* Pure parse of the ACPI TPM2 table (36-byte header, PlatformClass u16 @36,
 * Control Area Address u64 @40, Start Method u32 @48). 0 and the CRB window
 * base, or CK_TPM_E_ACPI / CK_TPM_E_START. *method gets the start method. */
int ck_tpm2_acpi_parse(const void *tbl, size_t len, uint64_t *crb_base, uint32_t *method);

/* Locality 0 and buffer layout. base is the CRB window base (TPM2 table control
 * area address minus 0x40), used only to turn the buffer addresses into window
 * offsets. 0 or CK_TPM_E_*. */
int ck_tpm_crb_open(struct ck_tpm_crb *t, const struct ck_tpm_crb_ops *ops, uint64_t base);
/* Release locality 0 (best effort). */
void ck_tpm_crb_close(struct ck_tpm_crb *t);

/* One command/response exchange: cmdReady, copy in, Start, wait, copy out,
 * goIdle. *rsp_len is the response size the TPM wrote. 0 or CK_TPM_E_*. The
 * TPM response code is in the response header, not interpreted here. */
int ck_tpm_crb_transact(struct ck_tpm_crb *t, const uint8_t *cmd, size_t cmd_len,
                        uint8_t *rsp, size_t rsp_cap, size_t *rsp_len);

#define CK_TPM_SU_CLEAR 0u
#define CK_TPM_SU_STATE 1u
#define CK_TPM_RC_RETRY 0x922u      /* warning: the TPM could not start the command, resubmit */
#define CK_TPM_RC_TESTING 0x90au    /* warning: self-test in progress, resubmit */
#define CK_TPM_RETRY_MAX 3u         /* resubmissions per command (Linux tpm_transmit also resubmits these two) */
#define CK_TPM_RC_INITIALIZE 0x100u /* Startup after the firmware already did it */
#define CK_TPM_RH_OWNER 0x40000001u
#define CK_TPM_PCR_SHA256_BYTES 32u
#define CK_TPM_NV_READ_MAX 1024u

/* TPM2_Startup. A TPM that the firmware already started answers
 * CK_TPM_RC_INITIALIZE (returned as is, a fact, not a fault). */
int ck_tpm2_startup(struct ck_tpm_crb *t, uint16_t startup_type);
/* TPM2_GetRandom, loops until n bytes are collected. */
int ck_tpm2_get_random(struct ck_tpm_crb *t, uint8_t *out, size_t n);
/* TPM2_PCR_Read of one SHA-256 PCR (0..23). *update_counter may be NULL. */
int ck_tpm2_pcr_read_sha256(struct ck_tpm_crb *t, unsigned pcr, uint8_t digest[32],
                            uint32_t *update_counter);
/* TPM2_NV_Read with an empty-password session. auth_handle is the NV index
 * itself (index with AUTHREAD) or CK_TPM_RH_OWNER (index with OWNERREAD). */
int ck_tpm2_nv_read(struct ck_tpm_crb *t, uint32_t auth_handle, uint32_t nv_index,
                    uint16_t offset, uint8_t *out, uint16_t size);

#endif
