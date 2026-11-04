#ifndef NET_RX_H
#define NET_RX_H

#include "console.h"

void net_rx_init(void);
int  net_rx_enqueue(const u8 *frame, u32 len);
u32  net_rx_dropped(void);

#endif
