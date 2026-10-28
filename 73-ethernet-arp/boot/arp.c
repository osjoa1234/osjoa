#include "arp.h"
#include "ethernet.h"
#include "virtio_net.h"

enum {
    ARP_PACKET_LEN     = 28,
    ARP_HTYPE_ETHERNET = 1,
    ARP_PTYPE_IPV4     = 0x0800,
    ARP_OP_REQUEST     = 1,
    ARP_OP_REPLY       = 2,

    ARP_CACHE_ENTRIES  = 8,
    ARP_WAIT_MS        = 1000U,

    RFLAGS_IF = 1U << 9
};

struct arp_packet {
    u16 htype;
    u16 ptype;
    u8  hlen;
    u8  plen;
    u16 oper;
    u8  sha[ETH_ALEN];
    u8  spa[4];
    u8  tha[ETH_ALEN];
    u8  tpa[4];
} __attribute__((packed));

struct arp_entry {
    u8  ip[4];
    u8  mac[ETH_ALEN];
    int valid;
};

static struct arp_entry g_cache[ARP_CACHE_ENTRIES];
static u32 g_cache_next;

static const u8 g_local_ip[4]        = { 10U, 0U, 2U, 15U };
static const u8 g_gateway_ip[4]      = { 10U, 0U, 2U, 2U };
static const u8 g_zero_mac[ETH_ALEN] = { 0U, 0U, 0U, 0U, 0U, 0U };

static int ip_equal(const u8 *a, const u8 *b)
{
    u32 i;

    for (i = 0U; i < 4U; i++) {
        if (a[i] != b[i]) return 0;
    }

    return 1;
}

static void print_ip(const u8 *ip)
{
    console_printf("%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void cache_store(const u8 *ip, const u8 *mac)
{
    struct arp_entry *slot = 0;
    u32 i;

    for (i = 0U; i < ARP_CACHE_ENTRIES; i++) {
        if (g_cache[i].valid && ip_equal(g_cache[i].ip, ip)) {
            slot = &g_cache[i];
            break;
        }
    }

    if (slot != 0 && ethernet_mac_equal(slot->mac, mac)) return;

    if (slot == 0) {
        for (i = 0U; i < ARP_CACHE_ENTRIES; i++) {
            if (!g_cache[i].valid) {
                slot = &g_cache[i];
                break;
            }
        }
    }

    if (slot == 0) {
        slot = &g_cache[g_cache_next % ARP_CACHE_ENTRIES];
        g_cache_next++;
    }

    for (i = 0U; i < 4U; i++) slot->ip[i] = ip[i];
    for (i = 0U; i < ETH_ALEN; i++) slot->mac[i] = mac[i];
    slot->valid = 1;

    console_printf("arp: cache ");
    print_ip(ip);
    console_printf(" -> ");
    ethernet_print_mac(mac);
    console_printf("\n");
}

static void cache_update(const u8 *ip, const u8 *mac)
{
    u64 flags;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r" (flags) : : "memory");

    cache_store(ip, mac);

    if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");
}

int arp_cache_lookup(const u8 *ip, u8 *mac_out)
{
    u64 flags;
    int found = -1;
    u32 i;
    u32 j;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r" (flags) : : "memory");

    for (i = 0U; i < ARP_CACHE_ENTRIES; i++) {
        if (g_cache[i].valid && ip_equal(g_cache[i].ip, ip)) {
            for (j = 0U; j < ETH_ALEN; j++) mac_out[j] = g_cache[i].mac[j];
            found = 0;
            break;
        }
    }

    if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

    return found;
}

static int arp_send(u16 oper, const u8 *dst_mac, const u8 *target_mac, const u8 *target_ip)
{
    struct arp_packet pkt;
    u32 i;

    pkt.htype = htons(ARP_HTYPE_ETHERNET);
    pkt.ptype = htons(ARP_PTYPE_IPV4);
    pkt.hlen  = ETH_ALEN;
    pkt.plen  = 4U;
    pkt.oper  = htons(oper);

    for (i = 0U; i < ETH_ALEN; i++) {
        pkt.sha[i] = virtio_net_mac()[i];
        pkt.tha[i] = target_mac[i];
    }
    for (i = 0U; i < 4U; i++) {
        pkt.spa[i] = g_local_ip[i];
        pkt.tpa[i] = target_ip[i];
    }

    return ethernet_send(dst_mac, ETHERTYPE_ARP, (const u8 *)&pkt, ARP_PACKET_LEN);
}

int arp_send_request(const u8 *target_ip)
{
    console_printf("arp: tx request who-has ");
    print_ip(target_ip);
    console_printf(" tell ");
    print_ip(g_local_ip);
    console_printf("\n");

    return arp_send(ARP_OP_REQUEST, ethernet_broadcast_mac, g_zero_mac, target_ip);
}

static int arp_send_reply(const u8 *dst_mac, const u8 *dst_ip)
{
    console_printf("arp: tx reply ");
    print_ip(g_local_ip);
    console_printf(" is-at ");
    ethernet_print_mac(virtio_net_mac());
    console_printf(" to ");
    print_ip(dst_ip);
    console_printf("\n");

    return arp_send(ARP_OP_REPLY, dst_mac, dst_mac, dst_ip);
}

void arp_rx(const u8 *packet, u32 len)
{
    const struct arp_packet *pkt = (const struct arp_packet *)packet;
    u16 oper;

    if (len < ARP_PACKET_LEN) return;
    if (ntohs(pkt->htype) != ARP_HTYPE_ETHERNET || ntohs(pkt->ptype) != ARP_PTYPE_IPV4) return;
    if (pkt->hlen != ETH_ALEN || pkt->plen != 4U) return;
    if (!ip_equal(pkt->tpa, g_local_ip)) return;

    oper = ntohs(pkt->oper);

    if (oper == ARP_OP_REPLY) {
        console_printf("arp: rx reply ");
        print_ip(pkt->spa);
        console_printf(" is-at ");
        ethernet_print_mac(pkt->sha);
        console_printf("\n");
        cache_update(pkt->spa, pkt->sha);
        return;
    }

    if (oper == ARP_OP_REQUEST) {
        console_printf("arp: rx request who-has ");
        print_ip(pkt->tpa);
        console_printf(" tell ");
        print_ip(pkt->spa);
        console_printf("\n");
        cache_update(pkt->spa, pkt->sha);
        if (arp_send_reply(pkt->sha, pkt->spa) != 0) {
            console_printf("arp: tx buffer unavailable\n");
        }
    }
}

static int arp_gateway_resolved(void)
{
    u8 mac[ETH_ALEN];

    return arp_cache_lookup(g_gateway_ip, mac) == 0 && virtio_net_idle();
}

void arp_test(void)
{
    static const u8 peer_ip[4]         = { 10U, 0U, 2U, 99U };
    static const u8 peer_mac[ETH_ALEN] = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x99U };
    struct arp_packet request;
    u8 mac[ETH_ALEN];
    u32 i;

    if (!virtio_net_ready()) return;

    console_set_color(0x0BU);

    if (arp_send_request(g_gateway_ip) != 0) {
        console_set_color(0x0CU);
        console_printf("arp: tx buffer unavailable\n");
        return;
    }

    if (virtio_net_wait(arp_gateway_resolved, ARP_WAIT_MS) != 0) {
        console_set_color(0x0CU);
        console_printf("arp: no reply\n");
    }

    if (arp_cache_lookup(g_gateway_ip, mac) == 0) {
        console_printf("arp: lookup ");
        print_ip(g_gateway_ip);
        console_printf(" -> ");
        ethernet_print_mac(mac);
        console_printf("\n");
    }

    request.htype = htons(ARP_HTYPE_ETHERNET);
    request.ptype = htons(ARP_PTYPE_IPV4);
    request.hlen  = ETH_ALEN;
    request.plen  = 4U;
    request.oper  = htons(ARP_OP_REQUEST);
    for (i = 0U; i < ETH_ALEN; i++) {
        request.sha[i] = peer_mac[i];
        request.tha[i] = 0U;
    }
    for (i = 0U; i < 4U; i++) {
        request.spa[i] = peer_ip[i];
        request.tpa[i] = g_local_ip[i];
    }

    arp_rx((const u8 *)&request, ARP_PACKET_LEN);

    if (virtio_net_wait(virtio_net_idle, ARP_WAIT_MS) != 0) {
        console_set_color(0x0CU);
        console_printf("arp: reply not consumed by device\n");
    }

    if (arp_cache_lookup(peer_ip, mac) == 0) {
        console_printf("arp: lookup ");
        print_ip(peer_ip);
        console_printf(" -> ");
        ethernet_print_mac(mac);
        console_printf("\n");
    }

    virtio_net_print_stats();
}
