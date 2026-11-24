#ifndef ICMP_H
#define ICMP_H

#include "console.h"

void icmp_rx(const u8 *src_ip, const u8 *packet, u32 len);
int  icmp_ping(const u8 *dst_ip, u16 seq, u32 ms);

#endif
