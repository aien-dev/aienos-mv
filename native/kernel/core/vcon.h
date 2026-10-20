/* vcon.h -- virtio console as the kernel console under a hypervisor with no
 * UART (Apple Virtualization.framework); see core/vcon.c. */
#ifndef AIENOS_CK_VCON_H
#define AIENOS_CK_VCON_H
#include <stddef.h>
#include <stdint.h>
/* 0 attached; <0 no MCFG (-1), no virtio console on the root bus (-2), no
 * capability list (-3), no common/notify capability (-4), no VERSION_1 (-5),
 * FEATURES_OK refused (-6), queue setup failed (-7). Needs the kernel MMU. */
int ck_vcon_attach(void);
int ck_vcon_on(void);
void ck_vcon_where(uint8_t *bus, uint8_t *dev, uint8_t *fn);
unsigned ck_vcon_tx_timeouts(void);
/* '\n' is sent as CR LF. Drops the bytes while re-entered. */
void ck_vcon_write(const char *s, size_t n);
/* Next received byte, or -1 when none is waiting. */
int ck_vcon_rx_poll(void);
#endif
