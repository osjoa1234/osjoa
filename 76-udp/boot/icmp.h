#ifndef ICMP_H
#define ICMP_H

#include "console.h"

void icmp_rx(const u8 *src_ip, u8 ttl, const u8 *packet, u32 len);
int  icmp_ping(const u8 *dst_ip, u16 seq, u32 ms);
void icmp_test(void);

#endif
