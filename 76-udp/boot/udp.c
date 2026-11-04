#include "udp.h"
#include "ethernet.h"
#include "thread.h"
#include "timer.h"
#include "net_rx.h"

enum {
    UDP_PORT_SLOTS       = 4,
    UDP_PORT_ECHO        = 7,
    UDP_PORT_DNS         = 53,
    UDP_PORT_DNS_CLIENT  = 40053,
    UDP_PORT_PEER        = 5000,
    UDP_PORT_UNBOUND     = 9999,
    UDP_WAIT_MS          = 1000U,
    UDP_TEST_DATA_MAX    = 32,

    UDP_INJECT_VALID     = 0,
    UDP_INJECT_NO_CSUM   = 1,
    UDP_INJECT_BAD_CSUM  = 2,

    RFLAGS_IF = 1U << 9
};

struct udp_hdr {
    u16 src_port;
    u16 dst_port;
    u16 length;
    u16 checksum;
} __attribute__((packed));

struct udp_pseudo_hdr {
    u8  src_ip[IP_ALEN];
    u8  dst_ip[IP_ALEN];
    u8  zero;
    u8  protocol;
    u16 udp_length;
} __attribute__((packed));

struct udp_port {
    volatile int used;
    u16 port;
    udp_handler_t handler;
};

static struct udp_port g_ports[UDP_PORT_SLOTS];

static thread_t *volatile g_dns_waiter;
static volatile int g_dns_done;

static u16 udp_checksum(const u8 *src_ip, const u8 *dst_ip, const u8 *hdr, const u8 *data, u32 data_len)
{
    struct udp_pseudo_hdr pseudo;
    u32 sum;
    u32 i;

    for (i = 0U; i < IP_ALEN; i++) {
        pseudo.src_ip[i] = src_ip[i];
        pseudo.dst_ip[i] = dst_ip[i];
    }
    pseudo.zero       = 0U;
    pseudo.protocol   = IP_PROTO_UDP;
    pseudo.udp_length = htons((u16)(UDP_HDR_LEN + data_len));

    sum = ip_checksum_add(0U, (const u8 *)&pseudo, sizeof(pseudo));
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
        console_printf("udp: rx bad checksum from ");
        ip_print(src_ip);
        console_printf(":%u\n", src_port);
        return;
    }

    for (i = 0U; i < UDP_PORT_SLOTS; i++) {
        if (g_ports[i].used && g_ports[i].port == dst_port) {
            g_ports[i].handler(src_ip, src_port, packet + UDP_HDR_LEN, length - UDP_HDR_LEN);
            return;
        }
    }

    console_printf("udp: rx port %u not bound, dropped (from ", dst_port);
    ip_print(src_ip);
    console_printf(":%u)\n", src_port);
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

    console_printf("udp: tx ");
    ip_print(ip_local);
    console_printf(":%u -> ", src_port);
    ip_print(dst_ip);
    console_printf(":%u (%u bytes)\n", dst_port, len);

    return ip_send(dst_ip, IP_PROTO_UDP, segment, UDP_HDR_LEN + len);
}

static void udp_echo_handler(const u8 *src_ip, u16 src_port, const u8 *data, u32 len)
{
    console_printf("udp: echo rx from ");
    ip_print(src_ip);
    console_printf(":%u (%u bytes)\n", src_port, len);

    if (udp_send(src_ip, src_port, UDP_PORT_ECHO, data, len) != 0) {
        console_printf("udp: echo tx dropped\n");
    }
}

static void udp_dns_handler(const u8 *src_ip, u16 src_port, const u8 *data, u32 len)
{
    (void)data;

    console_printf("udp: dns rx from ");
    ip_print(src_ip);
    console_printf(":%u (%u bytes)\n", src_port, len);

    g_dns_done = 1;
    if (g_dns_waiter) thread_unpark(g_dns_waiter);
}

static int udp_wait_dns(u32 ms)
{
    u64 flags;
    int result = 0;
    u32 deadline = timer_ticks() + ms * timer_hz() / 1000U;

    __asm__ volatile ("pushfq; pop %0" : "=r" (flags) : : "memory");

    for (;;) {
        __asm__ volatile ("cli" : : : "memory");

        if (g_dns_done) break;

        if ((int)(timer_ticks() - deadline) >= 0) {
            result = -1;
            break;
        }

        g_dns_waiter = thread_current();
        thread_sleep((deadline - timer_ticks()) * 1000U / timer_hz());
        g_dns_waiter = 0;
    }

    if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

    return result;
}

static void udp_inject(const u8 *peer_mac, const u8 *peer_ip, u16 src_port, u16 dst_port, const u8 *data, u32 len, int mode)
{
    u8 frame[ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + UDP_TEST_DATA_MAX];
    struct eth_hdr *eth = (struct eth_hdr *)frame;
    struct ip_hdr *ip = (struct ip_hdr *)(frame + ETH_HDR_LEN);
    struct udp_hdr *udp = (struct udp_hdr *)(frame + ETH_HDR_LEN + IP_HDR_LEN);
    u8 *payload = frame + ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN;
    u16 checksum;
    u32 i;

    if (len > UDP_TEST_DATA_MAX) return;

    for (i = 0U; i < ETH_ALEN; i++) {
        eth->dst[i] = ethernet_mac()[i];
        eth->src[i] = peer_mac[i];
    }
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->version_ihl = 0x45U;
    ip->dscp_ecn    = 0U;
    ip->total_len   = htons((u16)(IP_HDR_LEN + UDP_HDR_LEN + len));
    ip->id          = htons(1U);
    ip->flags_frag  = htons(0x4000U);
    ip->ttl         = 64U;
    ip->protocol    = IP_PROTO_UDP;
    ip->checksum    = 0U;
    for (i = 0U; i < IP_ALEN; i++) {
        ip->src[i] = peer_ip[i];
        ip->dst[i] = ip_local[i];
    }
    ip->checksum = htons(ip_checksum((const u8 *)ip, IP_HDR_LEN));

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((u16)(UDP_HDR_LEN + len));
    udp->checksum = 0U;
    for (i = 0U; i < len; i++) payload[i] = data[i];

    if (mode != UDP_INJECT_NO_CSUM) {
        checksum = udp_checksum(peer_ip, ip_local, (const u8 *)udp, payload, len);
        if (checksum == 0U) checksum = 0xFFFFU;
        if (mode == UDP_INJECT_BAD_CSUM) checksum ^= 0x0001U;
        udp->checksum = htons(checksum);
    }

    net_rx_enqueue(frame, ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + len);
}

void udp_test(void)
{
    static const u8 dns_ip[IP_ALEN]    = { 10U, 0U, 2U, 3U };
    static const u8 peer_ip[IP_ALEN]   = { 10U, 0U, 2U, 99U };
    static const u8 peer_mac[ETH_ALEN] = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x99U };
    static const u8 dns_query[] = {
        0x12U, 0x34U, 0x01U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        7U, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3U, 'c', 'o', 'm', 0U,
        0x00U, 0x01U, 0x00U, 0x01U
    };
    static const u8 hello[] = { 'h', 'e', 'l', 'l', 'o', ' ', 'u', 'd', 'p' };

    if (!ethernet_ready()) return;

    console_set_color(0x0BU);

    if (udp_bind(UDP_PORT_ECHO, udp_echo_handler) != 0 || udp_bind(UDP_PORT_DNS_CLIENT, udp_dns_handler) != 0) {
        console_set_color(0x0CU);
        console_printf("udp: bind failed\n");
        return;
    }

    g_dns_done = 0;

    if (udp_send(dns_ip, UDP_PORT_DNS, UDP_PORT_DNS_CLIENT, dns_query, sizeof(dns_query)) != 0) {
        console_set_color(0x0CU);
        console_printf("udp: dns query tx failed\n");
    } else if (udp_wait_dns(UDP_WAIT_MS) != 0) {
        console_set_color(0x0CU);
        console_printf("udp: no dns reply\n");
    }

    console_set_color(0x0BU);

    udp_inject(peer_mac, peer_ip, UDP_PORT_PEER, UDP_PORT_ECHO, hello, sizeof(hello), UDP_INJECT_VALID);
    thread_sleep(100U);

    udp_inject(peer_mac, peer_ip, UDP_PORT_PEER, UDP_PORT_ECHO, hello, sizeof(hello), UDP_INJECT_NO_CSUM);
    thread_sleep(100U);

    udp_inject(peer_mac, peer_ip, UDP_PORT_PEER, UDP_PORT_ECHO, hello, sizeof(hello), UDP_INJECT_BAD_CSUM);
    thread_sleep(100U);

    udp_inject(peer_mac, peer_ip, UDP_PORT_PEER, UDP_PORT_UNBOUND, hello, sizeof(hello), UDP_INJECT_VALID);
    thread_sleep(100U);
}
