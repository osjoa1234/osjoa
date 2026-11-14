#ifndef TCP_H
#define TCP_H

#include "console.h"
#include "ip.h"

enum {
    TCP_HDR_LEN = 20,
    TCP_MSS     = IP_MAX_PAYLOAD - TCP_HDR_LEN
};

typedef struct tcp_conn tcp_conn_t;
typedef void (*tcp_established_t)(tcp_conn_t *conn);

int  tcp_listen(u16 port, tcp_established_t handler);
int  tcp_connect(const u8 *dst_ip, u16 dst_port, u16 src_port, tcp_established_t handler);
void tcp_rx(const u8 *src_ip, const u8 *segment, u32 len);
void tcp_test(void);

#endif
