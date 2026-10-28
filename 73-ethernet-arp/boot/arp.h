#ifndef ARP_H
#define ARP_H

#include "console.h"

void arp_rx(const u8 *packet, u32 len);
int  arp_send_request(const u8 *target_ip);
int  arp_cache_lookup(const u8 *ip, u8 *mac_out);
void arp_test(void);

#endif
