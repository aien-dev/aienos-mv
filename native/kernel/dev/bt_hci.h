/* bt_hci.h -- Bluetooth Host Controller Interface for the AIENOS C kernel
 * (L207): the packet codec and a small polled host state machine that sits
 * above any HCI transport (USB today, dev/usb_bt.c). Freestanding, no heap.
 *
 * Reference: Bluetooth Core Specification v5.4 (local copy ~/docs/bluetooth-
 * core/core.txt), Vol 4 Part E (HCI functional specification). Opcode = OGF
 * (upper 6 bits) << 10 | OCF; command packet = opcode(2) length(1) params;
 * event packet = code(1) length(1) params. Linux net/bluetooth was not copied.
 *
 * What the host does: send one command at a time, wait for its Command
 * Complete / Command Status, honour Num_HCI_Command_Packets (a controller that
 * says 0 stops the host), collect Inquiry Result, LE Advertising Report, LE
 * Connection Complete and Disconnection Complete along the way. It never
 * pairs, bonds or sends ACL data: pairing-less LE connect only.
 *
 * QEMU has no usb-bt-dongle on this build, so the state machine is proven
 * against a controller simulator (dev/tests/bt_test.c) behind ck_bt_transport. */
#ifndef AIENOS_CK_BT_HCI_H
#define AIENOS_CK_BT_HCI_H
#include <stddef.h>
#include <stdint.h>

enum {
    CK_BT_OK = 0,
    CK_BT_E_ARG = -901,       /* bad argument or buffer too small */
    CK_BT_E_TRANSPORT = -902, /* the transport failed to send or receive */
    CK_BT_E_TIMEOUT = -903,   /* no matching event within the poll budget */
    CK_BT_E_CREDIT = -904,    /* controller offers no command credit */
    CK_BT_E_PROTO = -905,     /* malformed or impossible event */
    CK_BT_E_STATUS = -906,    /* controller answered with a non-zero HCI status */
    CK_BT_E_BUSY = -907,      /* a command is already outstanding */
    CK_BT_E_STATE = -908,     /* operation not valid in the current state */
};

#define CK_BT_OGF_LINK 0x01u
#define CK_BT_OGF_CTRL 0x03u
#define CK_BT_OGF_INFO 0x04u
#define CK_BT_OGF_LE 0x08u
#define CK_BT_OP(ogf, ocf) ((uint16_t)(((ogf) << 10) | (ocf)))

#define CK_BT_OP_INQUIRY CK_BT_OP(CK_BT_OGF_LINK, 0x0001u)
#define CK_BT_OP_DISCONNECT CK_BT_OP(CK_BT_OGF_LINK, 0x0006u)
#define CK_BT_OP_SET_EVENT_MASK CK_BT_OP(CK_BT_OGF_CTRL, 0x0001u)
#define CK_BT_OP_RESET CK_BT_OP(CK_BT_OGF_CTRL, 0x0003u)
#define CK_BT_OP_READ_LOCAL_VERSION CK_BT_OP(CK_BT_OGF_INFO, 0x0001u)
#define CK_BT_OP_READ_BD_ADDR CK_BT_OP(CK_BT_OGF_INFO, 0x0009u)
#define CK_BT_OP_LE_SET_SCAN_PARAMS CK_BT_OP(CK_BT_OGF_LE, 0x000Bu)
#define CK_BT_OP_LE_SET_SCAN_ENABLE CK_BT_OP(CK_BT_OGF_LE, 0x000Cu)
#define CK_BT_OP_LE_CREATE_CONN CK_BT_OP(CK_BT_OGF_LE, 0x000Du)

#define CK_BT_EVT_INQUIRY_COMPLETE 0x01u
#define CK_BT_EVT_INQUIRY_RESULT 0x02u
#define CK_BT_EVT_DISCONN_COMPLETE 0x05u
#define CK_BT_EVT_CMD_COMPLETE 0x0Eu
#define CK_BT_EVT_CMD_STATUS 0x0Fu
#define CK_BT_EVT_LE_META 0x3Eu
#define CK_BT_LE_CONN_COMPLETE 0x01u
#define CK_BT_LE_ADV_REPORT 0x02u

#define CK_BT_CMD_MAX 258u /* 3-byte header + 255 parameter bytes */
#define CK_BT_EVT_MAX 257u /* 2-byte header + 255 parameter bytes */
#define CK_BT_MAX_RESULTS 8u
#define CK_BT_ADV_DATA_MAX 31u /* legacy advertising PDU payload */

/* Transport seam. send_cmd takes one complete HCI command packet (header
 * included). poll_event returns 0 and fills one complete event packet, 1 when
 * no event is ready yet, a negative value on failure. */
typedef struct {
    int (*send_cmd)(void *ctx, const uint8_t *pkt, uint32_t len);
    int (*poll_event)(void *ctx, uint8_t *buf, uint32_t cap, uint32_t *len);
    void *ctx;
} ck_bt_transport;

typedef struct {
    uint8_t code;
    uint8_t plen;
    const uint8_t *p; /* points into the caller's buffer */
} ck_bt_evt;

/* Codec: pure functions, no state. Both return the packet length or <0. */
int ck_bt_cmd_encode(uint16_t opcode, const uint8_t *params, uint8_t plen, uint8_t *out, uint32_t cap);
int ck_bt_evt_parse(const uint8_t *buf, uint32_t len, ck_bt_evt *e);

typedef struct {
    uint8_t addr[6]; /* wire order, least significant byte first */
    uint8_t addr_type;
    uint8_t rssi;    /* LE only, signed dBm as stored */
    uint8_t data_len;
    uint8_t data[CK_BT_ADV_DATA_MAX];
    uint8_t cod[3];  /* BR/EDR class of device, inquiry only */
} ck_bt_peer;

typedef struct {
    ck_bt_transport tr;
    int credits;        /* Num_HCI_Command_Packets last announced; the first command needs 1 */
    int busy;           /* a command is outstanding */
    uint16_t pending;   /* its opcode */
    /* identity */
    uint8_t hci_version, lmp_version;
    uint16_t hci_subversion, lmp_subversion, company;
    uint8_t bd_addr[6];
    int have_version, have_addr;
    /* discovery */
    ck_bt_peer le[CK_BT_MAX_RESULTS];
    uint32_t n_le;
    ck_bt_peer bredr[CK_BT_MAX_RESULTS];
    uint32_t n_bredr;
    int inquiry_done;
    /* link */
    int connected;
    uint16_t handle;
    uint8_t conn_status, conn_role;
    uint8_t conn_peer[6];
    int disconnected;
    uint8_t disc_status, disc_reason;
    /* counters, for the logs and the tests */
    uint32_t events, dropped;
    uint32_t poll_budget; /* polls to wait for any one event; 0 selects the default */
} ck_bt_host;

#define CK_BT_DEFAULT_POLLS 200000u

void ck_bt_host_init(ck_bt_host *h, const ck_bt_transport *tr);

/* Send one command and wait for its Command Complete / Command Status. Return
 * parameters (after the opcode, so ret[0] is the status for Command Complete)
 * go to ret (up to retcap, *retlen the full length). A Command Status has the
 * status in ret[0] alone. Unrelated events are dispatched to the host state.
 * A non-zero HCI status is returned as CK_BT_E_STATUS with ret filled. */
int ck_bt_command(ck_bt_host *h, uint16_t opcode, const uint8_t *params, uint8_t plen, uint8_t *ret, uint8_t retcap,
                  uint8_t *retlen);

/* Pump events until pred says stop or the poll budget runs out. */
int ck_bt_pump(ck_bt_host *h, uint32_t polls);

/* The bring-up chain: Reset, Read Local Version, Read BD_ADDR, Set Event Mask
 * (default mask plus LE Meta). Fills the identity fields. */
int ck_bt_bringup(ck_bt_host *h);
int ck_bt_reset(ck_bt_host *h);
int ck_bt_read_version(ck_bt_host *h);
int ck_bt_read_bd_addr(ck_bt_host *h);
int ck_bt_set_event_mask(ck_bt_host *h, uint64_t mask);

/* BR/EDR inquiry with the General Inquiry Access Code, length in 1.28 s units;
 * waits for Inquiry Complete. */
int ck_bt_inquiry(ck_bt_host *h, uint8_t length, uint8_t max_responses);
/* Passive LE scan: set parameters, enable, collect for `polls` polls, disable. */
int ck_bt_le_scan(ck_bt_host *h, uint32_t polls);
/* Pairing-less LE connect (public or random peer, no whitelist), waits for LE
 * Connection Complete; then h->connected / h->handle say how it went. */
int ck_bt_le_connect(ck_bt_host *h, uint8_t peer_type, const uint8_t peer[6]);
/* Disconnect the current link (reason 0x13 remote user terminated) and wait for
 * Disconnection Complete. */
int ck_bt_disconnect(ck_bt_host *h);

/* Event-driven pieces, exported so the tests can feed hand-built events. */
void ck_bt_dispatch(ck_bt_host *h, const ck_bt_evt *e);
#endif
