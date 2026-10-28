#ifndef ETHERNET_H
#define ETHERNET_H

#include "console.h"

enum {
    ETH_ALEN      = 6,
    ETH_HDR_LEN   = 14,
    ETH_MTU       = 1500,
    ETHERTYPE_ARP = 0x0806
};

struct eth_hdr {
    u8  dst[ETH_ALEN];
    u8  src[ETH_ALEN];
    u16 ethertype;
} __attribute__((packed));

static inline u16 htons(u16 value)
{
    return (u16)((value << 8) | (value >> 8));
}

static inline u16 ntohs(u16 value)
{
    return htons(value);
}

extern const u8 ethernet_broadcast_mac[ETH_ALEN];

void ethernet_rx(const u8 *frame, u32 len);
int  ethernet_send(const u8 *dst_mac, u16 ethertype, const u8 *payload, u32 len);
int  ethernet_mac_equal(const u8 *a, const u8 *b);
void ethernet_print_mac(const u8 *mac);

#endif
