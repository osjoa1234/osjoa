#ifndef IP_H
#define IP_H

#include "console.h"

enum {
    IP_ALEN        = 4,
    IP_HDR_LEN     = 20,
    IP_MAX_PAYLOAD = 1480,
    IP_PROTO_ICMP  = 1,
    IP_PROTO_TCP   = 6,
    IP_PROTO_UDP   = 17
};

struct ip_hdr {
    u8  version_ihl;
    u8  dscp_ecn;
    u16 total_len;
    u16 id;
    u16 flags_frag;
    u8  ttl;
    u8  protocol;
    u16 checksum;
    u8  src[IP_ALEN];
    u8  dst[IP_ALEN];
} __attribute__((packed));

extern const u8 ip_local[IP_ALEN];
extern const u8 ip_gateway[IP_ALEN];

int  ip_equal(const u8 *a, const u8 *b);
void ip_print(const u8 *ip);
u16  ip_checksum(const u8 *data, u32 len);
void ip_rx(const u8 *packet, u32 len);
int  ip_send(const u8 *dst_ip, u8 protocol, const u8 *payload, u32 len);
void ip_arp_resolved(const u8 *ip, const u8 *mac);

#endif
