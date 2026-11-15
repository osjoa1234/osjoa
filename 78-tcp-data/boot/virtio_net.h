#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "console.h"

void      virtio_net_init(void);
int       virtio_net_ready(void);
const u8 *virtio_net_mac(void);
u8       *virtio_net_tx_alloc(void);
int       virtio_net_tx_submit(u8 *frame, u32 len);
void      virtio_net_print_stats(void);

#endif
