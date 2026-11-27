#include "udp.h"
#include "ethernet.h"

enum {
    UDP_PORT_SLOTS = 4
};

struct udp_hdr {
    u16 src_port;
    u16 dst_port;
    u16 length;
    u16 checksum;
} __attribute__((packed));

struct udp_port {
    volatile int used;
    u16 port;
    udp_handler_t handler;
};

static struct udp_port g_ports[UDP_PORT_SLOTS];

static u16 udp_checksum(const u8 *src_ip, const u8 *dst_ip, const u8 *hdr, const u8 *data, u32 data_len)
{
    u32 sum = ip_pseudo_sum(src_ip, dst_ip, IP_PROTO_UDP, (u16)(UDP_HDR_LEN + data_len));

    sum = ip_checksum_add(sum, hdr, UDP_HDR_LEN);
    sum = ip_checksum_add(sum, data, data_len);

    return ip_checksum_finish(sum);
}

int udp_bind(u16 port, udp_handler_t handler)
{
    u32 i;
    struct udp_port *slot = 0;

    for (i = 0U; i < UDP_PORT_SLOTS; i++) {
        if (g_ports[i].used && g_ports[i].port == port) return -1;
        if (!g_ports[i].used && slot == 0) slot = &g_ports[i];
    }

    if (slot == 0) return -1;

    slot->port    = port;
    slot->handler = handler;
    __asm__ volatile ("" ::: "memory");
    slot->used    = 1;

    return 0;
}

void udp_rx(const u8 *src_ip, const u8 *packet, u32 len)
{
    const struct udp_hdr *udp = (const struct udp_hdr *)packet;
    u16 length;
    u16 src_port;
    u16 dst_port;
    u32 i;

    if (len < UDP_HDR_LEN) return;

    length   = ntohs(udp->length);
    src_port = ntohs(udp->src_port);
    dst_port = ntohs(udp->dst_port);
    if (length < UDP_HDR_LEN || length > len) return;

    if (udp->checksum != 0U && udp_checksum(src_ip, ip_local, packet, packet + UDP_HDR_LEN, length - UDP_HDR_LEN) != 0U) {
        return;
    }

    for (i = 0U; i < UDP_PORT_SLOTS; i++) {
        if (g_ports[i].used && g_ports[i].port == dst_port) {
            g_ports[i].handler(src_ip, src_port, packet + UDP_HDR_LEN, length - UDP_HDR_LEN);
            return;
        }
    }
}

int udp_send(const u8 *dst_ip, u16 dst_port, u16 src_port, const u8 *data, u32 len)
{
    u8 segment[UDP_HDR_LEN + UDP_MAX_PAYLOAD];
    struct udp_hdr *udp = (struct udp_hdr *)segment;
    u16 checksum;
    u32 i;

    if (len > UDP_MAX_PAYLOAD) return -1;

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((u16)(UDP_HDR_LEN + len));
    udp->checksum = 0U;
    for (i = 0U; i < len; i++) segment[UDP_HDR_LEN + i] = data[i];

    checksum = udp_checksum(ip_local, dst_ip, segment, data, len);
    if (checksum == 0U) checksum = 0xFFFFU;
    udp->checksum = htons(checksum);

    return ip_send(dst_ip, IP_PROTO_UDP, segment, UDP_HDR_LEN + len);
}
