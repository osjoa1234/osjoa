#include "ip.h"
#include "ethernet.h"
#include "arp.h"
#include "icmp.h"
#include "timer.h"

enum {
    IP_VERSION_IHL      = 0x45,
    IP_TTL              = 64,
    IP_FLAG_DF          = 0x4000,
    IP_FLAG_MF          = 0x2000,
    IP_OFFSET_MASK      = 0x1FFF,
    IP_ARP_WAIT_SLOTS   = 4,
    IP_ARP_WAIT_MAX_MS  = 1000U,

    RFLAGS_IF = 1U << 9
};

const u8 ip_local[IP_ALEN]   = { 10U, 0U, 2U, 15U };
const u8 ip_gateway[IP_ALEN] = { 10U, 0U, 2U, 2U };

struct ip_arp_wait {
    int  valid;
    u8   dst_ip[IP_ALEN];
    u8   protocol;
    u32  len;
    u32  deadline;
    u8   payload[IP_MAX_PAYLOAD];
};

static u16 g_ip_id;
static struct ip_arp_wait g_arp_wait[IP_ARP_WAIT_SLOTS];

int ip_equal(const u8 *a, const u8 *b)
{
    u32 i;

    for (i = 0U; i < IP_ALEN; i++) {
        if (a[i] != b[i]) return 0;
    }

    return 1;
}

void ip_print(const u8 *ip)
{
    console_printf("%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

u16 ip_checksum(const u8 *data, u32 len)
{
    u32 sum = 0U;
    u32 i;

    for (i = 0U; i + 1U < len; i += 2U) {
        sum += ((u32)data[i] << 8) | data[i + 1U];
    }
    if (len & 1U) sum += (u32)data[len - 1U] << 8;

    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);

    return (u16)~sum;
}

void ip_rx(const u8 *packet, u32 len)
{
    const struct ip_hdr *ip = (const struct ip_hdr *)packet;
    u32 hdr_len;
    u32 total_len;

    if (len < IP_HDR_LEN) return;
    if ((ip->version_ihl >> 4) != 4U) return;

    hdr_len = (u32)(ip->version_ihl & 0x0FU) * 4U;
    if (hdr_len < IP_HDR_LEN || hdr_len > len) return;

    total_len = ntohs(ip->total_len);
    if (total_len < hdr_len || total_len > len) return;

    if (ip_checksum(packet, hdr_len) != 0U) {
        console_printf("ip: rx bad header checksum\n");
        return;
    }

    if (ntohs(ip->flags_frag) & (IP_FLAG_MF | IP_OFFSET_MASK)) return;
    if (!ip_equal(ip->dst, ip_local)) return;

    if (ip->protocol == IP_PROTO_ICMP) {
        icmp_rx(ip->src, ip->ttl, packet + hdr_len, total_len - hdr_len);
        return;
    }

    console_printf("ip: rx protocol=%u from ", ip->protocol);
    ip_print(ip->src);
    console_printf(" (%u bytes)\n", total_len);
}

static int ip_output(const u8 *dst_mac, const u8 *dst_ip, u8 protocol, const u8 *payload, u32 len)
{
    u8 packet[IP_HDR_LEN + IP_MAX_PAYLOAD];
    struct ip_hdr *ip = (struct ip_hdr *)packet;
    u32 i;

    if (len > IP_MAX_PAYLOAD) return -1;

    ip->version_ihl = IP_VERSION_IHL;
    ip->dscp_ecn    = 0U;
    ip->total_len   = htons((u16)(IP_HDR_LEN + len));
    ip->id          = htons(g_ip_id++);
    ip->flags_frag  = htons(IP_FLAG_DF);
    ip->ttl         = IP_TTL;
    ip->protocol    = protocol;
    ip->checksum    = 0U;
    for (i = 0U; i < IP_ALEN; i++) {
        ip->src[i] = ip_local[i];
        ip->dst[i] = dst_ip[i];
    }
    ip->checksum = htons(ip_checksum(packet, IP_HDR_LEN));

    for (i = 0U; i < len; i++) packet[IP_HDR_LEN + i] = payload[i];

    return ethernet_send(dst_mac, ETHERTYPE_IPV4, packet, IP_HDR_LEN + len);
}

int ip_send(const u8 *dst_ip, u8 protocol, const u8 *payload, u32 len)
{
    u8 mac[ETH_ALEN];
    struct ip_arp_wait *slot = 0;
    int requested = 0;
    int hit = 0;
    int result = 0;
    u64 flags;
    u32 now;
    u32 i;

    if (len > IP_MAX_PAYLOAD) return -1;

    if (arp_cache_lookup(dst_ip, mac) == 0) return ip_output(mac, dst_ip, protocol, payload, len);

    __asm__ volatile ("pushfq; pop %0; cli" : "=r" (flags) : : "memory");

    now = timer_ticks();

    for (i = 0U; i < IP_ARP_WAIT_SLOTS; i++) {
        if (g_arp_wait[i].valid && (int)(now - g_arp_wait[i].deadline) >= 0) {
            console_printf("ip: arp wait packet to ");
            ip_print(g_arp_wait[i].dst_ip);
            console_printf(" expired\n");
            g_arp_wait[i].valid = 0;
        }
    }

    if (arp_cache_lookup(dst_ip, mac) == 0) hit = 1;

    for (i = 0U; i < IP_ARP_WAIT_SLOTS; i++) {
        if (g_arp_wait[i].valid && ip_equal(g_arp_wait[i].dst_ip, dst_ip)) requested = 1;
        if (!g_arp_wait[i].valid && slot == 0) slot = &g_arp_wait[i];
    }

    if (!hit && slot != 0) {
        for (i = 0U; i < IP_ALEN; i++) slot->dst_ip[i] = dst_ip[i];
        for (i = 0U; i < len; i++) slot->payload[i] = payload[i];
        slot->protocol = protocol;
        slot->len      = len;
        slot->deadline = now + IP_ARP_WAIT_MAX_MS * timer_hz() / 1000U;
        slot->valid    = 1;

        if (!requested && arp_send_request(dst_ip) != 0) {
            slot->valid = 0;
            result = -1;
        }
    }

    if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

    if (hit) return ip_output(mac, dst_ip, protocol, payload, len);

    if (slot == 0) {
        console_printf("ip: arp wait queue full, dropped\n");
        return -1;
    }

    return result;
}

void ip_arp_resolved(const u8 *ip, const u8 *mac)
{
    u8 payload[IP_MAX_PAYLOAD];
    u8 protocol;
    u32 len;
    u64 flags;
    u32 i;
    u32 j;

    for (;;) {
        len = 0U;

        __asm__ volatile ("pushfq; pop %0; cli" : "=r" (flags) : : "memory");

        for (i = 0U; i < IP_ARP_WAIT_SLOTS; i++) {
            if (g_arp_wait[i].valid && ip_equal(g_arp_wait[i].dst_ip, ip)) {
                protocol = g_arp_wait[i].protocol;
                len      = g_arp_wait[i].len;
                for (j = 0U; j < len; j++) payload[j] = g_arp_wait[i].payload[j];
                g_arp_wait[i].valid = 0;
                break;
            }
        }

        if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

        if (i == IP_ARP_WAIT_SLOTS) return;

        console_printf("ip: arp wait packet to ");
        ip_print(ip);
        console_printf(" released\n");

        ip_output(mac, ip, protocol, payload, len);
    }
}
