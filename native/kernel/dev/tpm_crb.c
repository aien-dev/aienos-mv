/* tpm_crb.c -- TPM 2.0 CRB driver, see tpm_crb.h.
 *
 * Register map (PTP chapter 5, as transcribed in edk2 MdePkg/Include/
 * IndustryStandard/TpmPtp.h, PTP_CRB_REGISTERS; the PTP PDF itself was not
 * reachable when this was written, so the offsets below are the edk2 copy,
 * and the QEMU tpm-crb model plus swtpm are the executable check):
 *   LocalityState 00h, LocalityControl 08h, LocalityStatus 0Ch, InterfaceId
 *   30h, Vid 34h, Did 36h, ControlRequest 40h, ControlStatus 44h,
 *   ControlCancel 48h, ControlStart 4Ch, CommandSize 58h, CommandAddress
 *   5Ch/60h, ResponseSize 64h, ResponseAddress 68h, DataBuffer 80h.
 * Handshake, quoted from the same header: ControlRequest.cmdReady "Set to 1
 * by Software to indicate the TPM should be ready to receive a command.
 * Cleared to 0 by TPM to acknowledge the request. TPM SHALL complete this
 * transition within TIMEOUT_C" (200 ms); goIdle likewise; ControlStart
 * "Writes (0000 0001h): TPM transitions to Command Execution. Writes
 * (0000 0000h): TPM clears this field and transitions to Command Completion". */
#include "tpm_crb.h"

#define LOC_STATE 0x00u
#define LOC_CTRL 0x08u
#define INTF_ID 0x30u
#define VID_DID 0x34u
#define CTRL_REQ 0x40u
#define CTRL_STS 0x44u
#define CTRL_START 0x4cu
#define CMD_SIZE 0x58u
#define CMD_LADDR 0x5cu
#define CMD_HADDR 0x60u
#define RSP_SIZE 0x64u
#define RSP_ADDR 0x68u
#define WINDOW 0x1000u
#define BUF_MIN 0x80u

#define LS_ESTABLISHED_VALID 0x80u /* tpmRegValidSts, bit 7 */
#define LS_ASSIGNED 0x02u          /* locAssigned, bit 1 */
#define LC_REQUEST 0x01u           /* requestAccess */
#define LC_RELINQUISH 0x02u
#define REQ_CMD_READY 0x01u
#define REQ_GO_IDLE 0x02u
#define STS_FATAL 0x01u
#define STS_IDLE 0x02u

#define TIMEOUT_A_US 750000u
#define TIMEOUT_C_US 200000u
#define EXEC_US 5000000u
#define STEP_US 50u

#define TPM_ST_NO_SESSIONS 0x8001u
#define TPM_ST_SESSIONS 0x8002u
#define TPM_CC_STARTUP 0x144u
#define TPM_CC_NV_READ 0x14eu
#define TPM_CC_GET_RANDOM 0x17bu
#define TPM_CC_PCR_READ 0x17eu
#define TPM_RS_PW 0x40000009u
#define TPM_ALG_SHA256 0x000bu
#define HDR 10u

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint8_t *put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return p + 2;
}
static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return p + 4;
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Poll until (reg & mask) == want, giving up after limit_us. */
static int wait_bits(struct ck_tpm_crb *t, uint32_t off, uint32_t mask, uint32_t want, uint32_t limit_us)
{
    uint32_t waited = 0;
    for (;;) {
        if ((t->ops.rd32(t->ops.ctx, off) & mask) == want)
            return 0;
        if (waited >= limit_us)
            return CK_TPM_E_TIMEOUT;
        t->ops.delay_us(t->ops.ctx, STEP_US);
        waited += STEP_US;
    }
}

int ck_tpm2_acpi_parse(const void *tbl, size_t len, uint64_t *crb_base, uint32_t *method)
{
    const uint8_t *p = tbl;
    if (!tbl || !crb_base || !method || len < 52)
        return CK_TPM_E_ACPI;
    if (p[0] != 'T' || p[1] != 'P' || p[2] != 'M' || p[3] != '2')
        return CK_TPM_E_ACPI;
    if (le32(p + 4) > len)
        return CK_TPM_E_ACPI;
    uint64_t ctrl = (uint64_t)le32(p + 40) | (uint64_t)le32(p + 44) << 32;
    *method = le32(p + 48);
    if (*method != 7u)
        return CK_TPM_E_START;
    if (ctrl < 0x40u || (ctrl & 0xfu))
        return CK_TPM_E_ACPI;
    *crb_base = ctrl - 0x40u; /* the control area starts at window offset 40h */
    return 0;
}

int ck_tpm_crb_open(struct ck_tpm_crb *t, const struct ck_tpm_crb_ops *ops, uint64_t base)
{
    if (!t || !ops || !ops->rd32 || !ops->wr32 || !ops->rd_buf || !ops->wr_buf || !ops->delay_us)
        return CK_TPM_E_ARG;
    t->ops = *ops;
    t->retries = 0;
    uint32_t ls = ops->rd32(ops->ctx, LOC_STATE);
    if (ls == 0xffffffffu)
        return CK_TPM_E_NO_TPM;
    if (wait_bits(t, LOC_STATE, LS_ESTABLISHED_VALID, LS_ESTABLISHED_VALID, TIMEOUT_A_US))
        return CK_TPM_E_NO_TPM;
    if (!(ops->rd32(ops->ctx, LOC_STATE) & LS_ASSIGNED)) {
        ops->wr32(ops->ctx, LOC_CTRL, LC_REQUEST);
        if (wait_bits(t, LOC_STATE, LS_ASSIGNED, LS_ASSIGNED, TIMEOUT_A_US))
            return CK_TPM_E_LOCALITY;
    }
    t->interface_id = ops->rd32(ops->ctx, INTF_ID);
    uint32_t vd = ops->rd32(ops->ctx, VID_DID);
    t->vid = (uint16_t)vd;
    t->did = (uint16_t)(vd >> 16);
    t->cmd_size = ops->rd32(ops->ctx, CMD_SIZE);
    t->rsp_size = ops->rd32(ops->ctx, RSP_SIZE);
    uint64_t cmd_a = (uint64_t)ops->rd32(ops->ctx, CMD_LADDR) | (uint64_t)ops->rd32(ops->ctx, CMD_HADDR) << 32;
    uint64_t rsp_a = (uint64_t)ops->rd32(ops->ctx, RSP_ADDR) | (uint64_t)ops->rd32(ops->ctx, RSP_ADDR + 4) << 32;
    if (cmd_a < base || rsp_a < base || cmd_a - base >= WINDOW || rsp_a - base >= WINDOW)
        return CK_TPM_E_LAYOUT;
    t->cmd_off = (uint32_t)(cmd_a - base);
    t->rsp_off = (uint32_t)(rsp_a - base);
    if (t->cmd_off < BUF_MIN || t->rsp_off < BUF_MIN || t->cmd_size < HDR || t->rsp_size < HDR ||
        t->cmd_off + t->cmd_size > WINDOW || t->rsp_off + t->rsp_size > WINDOW)
        return CK_TPM_E_LAYOUT;
    return 0;
}

void ck_tpm_crb_close(struct ck_tpm_crb *t)
{
    if (t && t->ops.wr32)
        t->ops.wr32(t->ops.ctx, LOC_CTRL, LC_RELINQUISH);
}

static int go_idle(struct ck_tpm_crb *t)
{
    t->ops.wr32(t->ops.ctx, CTRL_REQ, REQ_GO_IDLE);
    return wait_bits(t, CTRL_REQ, REQ_GO_IDLE, 0, TIMEOUT_C_US);
}

int ck_tpm_crb_transact(struct ck_tpm_crb *t, const uint8_t *cmd, size_t cmd_len,
                        uint8_t *rsp, size_t rsp_cap, size_t *rsp_len)
{
    uint8_t hdr[HDR];
    int rc;
    if (!t || !cmd || !rsp || !rsp_len || cmd_len < HDR || cmd_len > t->cmd_size || rsp_cap < HDR)
        return CK_TPM_E_ARG;
    if (t->ops.rd32(t->ops.ctx, CTRL_STS) & STS_FATAL)
        return CK_TPM_E_FATAL;
    t->ops.wr32(t->ops.ctx, CTRL_REQ, REQ_CMD_READY);
    rc = wait_bits(t, CTRL_REQ, REQ_CMD_READY, 0, TIMEOUT_C_US);
    if (!rc)
        rc = wait_bits(t, CTRL_STS, STS_IDLE, 0, TIMEOUT_C_US);
    if (rc) {
        (void)go_idle(t);
        return rc;
    }
    t->ops.wr_buf(t->ops.ctx, t->cmd_off, cmd, cmd_len);
    t->ops.wr32(t->ops.ctx, CTRL_START, 1);
    rc = wait_bits(t, CTRL_START, 1, 0, EXEC_US);
    if (!rc && (t->ops.rd32(t->ops.ctx, CTRL_STS) & STS_FATAL))
        rc = CK_TPM_E_FATAL;
    if (!rc) {
        t->ops.rd_buf(t->ops.ctx, t->rsp_off, hdr, HDR);
        uint32_t size = be32(hdr + 2);
        if (size < HDR || size > t->rsp_size || size > rsp_cap) {
            rc = CK_TPM_E_PROTO;
        } else {
            for (unsigned i = 0; i < HDR; i++)
                rsp[i] = hdr[i];
            if (size > HDR)
                t->ops.rd_buf(t->ops.ctx, t->rsp_off + HDR, rsp + HDR, size - HDR);
            *rsp_len = size;
        }
    }
    int idle = go_idle(t);
    return rc ? rc : idle;
}

/* Run a built command and return the TPM response code (0 ok, > 0 TPM_RC)
 * or a driver error; the response is left in rsp. */
static int run(struct ck_tpm_crb *t, uint8_t *cmd, size_t cmd_len, uint16_t tag, uint32_t cc,
               uint8_t *rsp, size_t cap, size_t *rlen)
{
    put16(cmd, tag);
    put32(cmd + 2, (uint32_t)cmd_len);
    put32(cmd + 6, cc);
    for (unsigned attempt = 0;; attempt++) {
        int rc = ck_tpm_crb_transact(t, cmd, cmd_len, rsp, cap, rlen);
        if (rc)
            return rc;
        uint16_t rtag = be16(rsp);
        if (rtag != TPM_ST_NO_SESSIONS && rtag != TPM_ST_SESSIONS)
            return CK_TPM_E_PROTO;
        uint32_t trc = be32(rsp + 6);
        if ((trc == CK_TPM_RC_RETRY || trc == CK_TPM_RC_TESTING) && attempt < CK_TPM_RETRY_MAX) {
            t->retries++;
            t->ops.delay_us(t->ops.ctx, 50000u);
            continue;
        }
        return trc ? (int)trc : 0;
    }
}

int ck_tpm2_startup(struct ck_tpm_crb *t, uint16_t startup_type)
{
    uint8_t cmd[HDR + 2], rsp[64];
    size_t rl;
    put16(cmd + HDR, startup_type);
    return run(t, cmd, sizeof cmd, TPM_ST_NO_SESSIONS, TPM_CC_STARTUP, rsp, sizeof rsp, &rl);
}

int ck_tpm2_get_random(struct ck_tpm_crb *t, uint8_t *out, size_t n)
{
    uint8_t cmd[HDR + 2], rsp[HDR + 2 + 64];
    size_t rl, got = 0;
    if (!out)
        return CK_TPM_E_ARG;
    while (got < n) {
        size_t want = n - got > 32 ? 32 : n - got;
        put16(cmd + HDR, (uint16_t)want);
        int rc = run(t, cmd, sizeof cmd, TPM_ST_NO_SESSIONS, TPM_CC_GET_RANDOM, rsp, sizeof rsp, &rl);
        if (rc)
            return rc;
        if (rl < HDR + 2)
            return CK_TPM_E_PROTO;
        size_t have = be16(rsp + HDR);
        if (have == 0 || HDR + 2 + have > rl)
            return CK_TPM_E_PROTO;
        if (have > n - got)
            have = n - got;
        for (size_t i = 0; i < have; i++)
            out[got + i] = rsp[HDR + 2 + i];
        got += have;
    }
    return 0;
}

int ck_tpm2_pcr_read_sha256(struct ck_tpm_crb *t, unsigned pcr, uint8_t digest[32], uint32_t *update_counter)
{
    uint8_t cmd[HDR + 4 + 2 + 1 + 3], rsp[256], *p = cmd + HDR;
    size_t rl;
    if (pcr > 23 || !digest)
        return CK_TPM_E_ARG;
    p = put32(p, 1);
    p = put16(p, TPM_ALG_SHA256);
    *p++ = 3;
    p[0] = p[1] = p[2] = 0;
    p[pcr / 8] = (uint8_t)(1u << (pcr % 8));
    int rc = run(t, cmd, sizeof cmd, TPM_ST_NO_SESSIONS, TPM_CC_PCR_READ, rsp, sizeof rsp, &rl);
    if (rc)
        return rc;
    /* updateCounter u32, TPML_PCR_SELECTION out, TPML_DIGEST */
    size_t o = HDR;
    if (rl < o + 8)
        return CK_TPM_E_PROTO;
    uint32_t counter = be32(rsp + o);
    uint32_t sel_n = be32(rsp + o + 4);
    o += 8;
    for (uint32_t i = 0; i < sel_n; i++) {
        if (rl < o + 3)
            return CK_TPM_E_PROTO;
        o += 3u + rsp[o + 2];
    }
    if (rl < o + 4 + 2 || be32(rsp + o) != 1)
        return CK_TPM_E_PROTO;
    o += 4;
    if (be16(rsp + o) != CK_TPM_PCR_SHA256_BYTES || rl < o + 2 + CK_TPM_PCR_SHA256_BYTES)
        return CK_TPM_E_PROTO;
    for (unsigned i = 0; i < CK_TPM_PCR_SHA256_BYTES; i++)
        digest[i] = rsp[o + 2 + i];
    if (update_counter)
        *update_counter = counter;
    return 0;
}

int ck_tpm2_nv_read(struct ck_tpm_crb *t, uint32_t auth_handle, uint32_t nv_index,
                    uint16_t offset, uint8_t *out, uint16_t size)
{
    uint8_t cmd[HDR + 4 + 4 + 4 + 9 + 4], rsp[HDR + 4 + 2 + CK_TPM_NV_READ_MAX + 64], *p = cmd + HDR;
    size_t rl;
    if (!out || size == 0 || size > CK_TPM_NV_READ_MAX)
        return CK_TPM_E_ARG;
    p = put32(p, auth_handle);
    p = put32(p, nv_index);
    p = put32(p, 9);          /* authorizationSize */
    p = put32(p, TPM_RS_PW);  /* password session, empty nonce, attributes 0, empty hmac */
    p = put16(p, 0);
    *p++ = 0;
    p = put16(p, 0);
    p = put16(p, size);
    p = put16(p, offset);
    int rc = run(t, cmd, (size_t)(p - cmd), TPM_ST_SESSIONS, TPM_CC_NV_READ, rsp, sizeof rsp, &rl);
    if (rc)
        return rc;
    /* parameterSize u32, TPM2B_MAX_NV_BUFFER, then the response auth area */
    if (rl < HDR + 4 + 2 || be16(rsp + HDR + 4) != size || rl < HDR + 4 + 2 + (size_t)size)
        return CK_TPM_E_PROTO;
    for (unsigned i = 0; i < size; i++)
        out[i] = rsp[HDR + 4 + 2 + i];
    return 0;
}
