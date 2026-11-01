#include "ip.h"
#include "ethernet.h"
#include "arp.h"
#include "icmp.h"

enum {
    IP_VERSION_IHL  = 0x45,
    IP_TTL          = 64,
    IP_FLAG_DF      = 0x4000,
    IP_FLAG_MF      = 0x2000,
    IP_OFFSET_MASK  = 0x1FFF,
    IP_ARP_WAIT_MS  = 1000U
};

const u8 ip_local[IP_ALEN]   = { 10U, 0U, 2U, 15U };
const u8 ip_gateway[IP_ALEN] = { 10U, 0U, 2U, 2U };

static u16 g_ip_id;

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

    if (arp_resolve(dst_ip, mac, IP_ARP_WAIT_MS) != 0) return -1;

    return ip_output(mac, dst_ip, protocol, payload, len);
}

int ip_send_cached(const u8 *dst_ip, u8 protocol, const u8 *payload, u32 len)
{
    u8 mac[ETH_ALEN];

    if (arp_cache_lookup(dst_ip, mac) != 0) return -1;

    return ip_output(mac, dst_ip, protocol, payload, len);
}
