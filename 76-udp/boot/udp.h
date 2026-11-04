#ifndef UDP_H
#define UDP_H

#include "console.h"
#include "ip.h"

enum {
    UDP_HDR_LEN     = 8,
    UDP_MAX_PAYLOAD = IP_MAX_PAYLOAD - UDP_HDR_LEN
};

typedef void (*udp_handler_t)(const u8 *src_ip, u16 src_port, const u8 *data, u32 len);

int  udp_bind(u16 port, udp_handler_t handler);
void udp_rx(const u8 *src_ip, const u8 *packet, u32 len);
int  udp_send(const u8 *dst_ip, u16 dst_port, u16 src_port, const u8 *data, u32 len);
void udp_test(void);

#endif
