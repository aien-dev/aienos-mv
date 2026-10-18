/* report_x86.h -- the iMac kernel's network report (report_x86.c): the
 * allowlisted lines of the console's RAM copy cut into UDP broadcast
 * datagrams. Laboratory diagnostic builds only (make imac-kernel
 * IMAC_LAB_REPORT=1); the default image sends nothing and does not contain
 * the frame builder. Reports are unauthenticated: never a receipt, never a
 * command. No machine access in here, so the filter and the frame builder are
 * also compiled and tested on the host. */
#ifndef AIENOS_CK_X86_REPORT_H
#define AIENOS_CK_X86_REPORT_H
#include <stddef.h>
#include <stdint.h>

#define CK_X86_REPORT_PORT 47031u  /* UDP, source and destination */
#define CK_X86_REPORT_CHUNK 1024u  /* report bytes per datagram */
#define CK_X86_REPORT_MAX 16384u   /* the console's RAM copy */
#define CK_X86_REPORT_LINE_MAX 200u /* longer lines are never sent */

/* Copies into `out` only the complete lines of `log` that start with an
 * allowlisted kernel tag (report_x86.c), are at most LINE_MAX bytes and hold
 * printable ASCII only; every other line is dropped and counted in *dropped.
 * Bytes written (never more than cap). */
size_t ck_x86_report_filter(const char *log, size_t len, char *out, size_t cap, unsigned *dropped);

/* Datagrams needed for a report of len bytes; 0 when len is 0 or too big. */
unsigned ck_x86_report_parts(size_t len);
/* The Ethernet frame (no FCS) carrying part `part` of the report: broadcast
 * from `mac`, IPv4 0.0.0.0 -> 255.255.255.255, UDP PORT -> PORT, TTL 1.
 * Bytes written, 0 on a bad argument or a buffer too small. */
size_t ck_x86_report_frame(const uint8_t mac[6], uint64_t run, const char *log, size_t len, unsigned part,
                           uint8_t *out, size_t cap);
#endif
