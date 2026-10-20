/* net_join.h -- Interplane join on the bound virtio-net device (CK_INTERPLANE
 * image). ck_net_bind_selftest calls ck_net_join_run instead of the slirp UDP
 * round trip when boot/efi_join.c found a well-formed JOIN.BIN; the device is
 * released by the caller afterwards. The run lasts the bundle's run_seconds:
 * beacons every second, sessions with every roster member heard on the LAN,
 * the bundle's one signed message (if any), relays, and a summary line.
 * QEMU only so far: no hardware NIC has run it. */
#ifndef AIENOS_CK_NET_JOIN_H
#define AIENOS_CK_NET_JOIN_H
#include "net_bind.h"
#include "aienos_virtio_net.h"
#include "aienos_ipj.h"

int ck_net_join_run(vnet_dev *dev);
#endif
