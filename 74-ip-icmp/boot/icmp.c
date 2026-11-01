#include "icmp.h"
#include "ethernet.h"
#include "ip.h"
#include "thread.h"
#include "timer.h"

enum {
    ICMP_ECHO_HDR_LEN      = 8,
    ICMP_TYPE_ECHO_REPLY   = 0,
    ICMP_TYPE_ECHO_REQUEST = 8,
    ICMP_ECHO_ID           = 0x4F53,
    ICMP_ECHO_DATA_LEN     = 56,
    ICMP_PING_COUNT        = 3,
    ICMP_WAIT_MS           = 1000U,

    RFLAGS_IF = 1U << 9
};

struct icmp_echo_hdr {
    u8  type;
    u8  code;
    u16 checksum;
    u16 identifier;
    u16 sequence;
} __attribute__((packed));

static thread_t *volatile g_ping_waiter;
static volatile u16 g_ping_seq;
static volatile int g_ping_done;

static void icmp_echo_reply(const u8 *src_ip, const u8 *packet, u32 len)
{
    u8 reply[ICMP_ECHO_HDR_LEN + IP_MAX_PAYLOAD];
    struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr *)reply;
    u32 i;

    for (i = 0U; i < len; i++) reply[i] = packet[i];

    icmp->type     = ICMP_TYPE_ECHO_REPLY;
    icmp->checksum = 0U;
    icmp->checksum = htons(ip_checksum(reply, len));

    console_printf("icmp: tx echo reply to ");
    ip_print(src_ip);
    console_printf(" id=0x%04X seq=%u\n", ntohs(icmp->identifier), ntohs(icmp->sequence));

    if (ip_send_cached(src_ip, IP_PROTO_ICMP, reply, len) != 0) {
        console_printf("icmp: tx echo reply dropped\n");
    }
}

void icmp_rx(const u8 *src_ip, u8 ttl, const u8 *packet, u32 len)
{
    const struct icmp_echo_hdr *icmp = (const struct icmp_echo_hdr *)packet;

    if (len < ICMP_ECHO_HDR_LEN) return;

    if (ip_checksum(packet, len) != 0U) {
        console_printf("icmp: rx bad checksum\n");
        return;
    }

    if (icmp->type == ICMP_TYPE_ECHO_REQUEST && icmp->code == 0U) {
        console_printf("icmp: rx echo request from ");
        ip_print(src_ip);
        console_printf(" id=0x%04X seq=%u (%u bytes)\n", ntohs(icmp->identifier), ntohs(icmp->sequence), len);
        icmp_echo_reply(src_ip, packet, len);
        return;
    }

    if (icmp->type == ICMP_TYPE_ECHO_REPLY && icmp->code == 0U) {
        console_printf("icmp: rx echo reply from ");
        ip_print(src_ip);
        console_printf(" id=0x%04X seq=%u ttl=%u (%u bytes)\n", ntohs(icmp->identifier), ntohs(icmp->sequence), ttl, len);

        if (ntohs(icmp->identifier) == ICMP_ECHO_ID && ntohs(icmp->sequence) == g_ping_seq) {
            g_ping_done = 1;
            if (g_ping_waiter) thread_unpark(g_ping_waiter);
        }
        return;
    }

    console_printf("icmp: rx type=%u code=%u from ", icmp->type, icmp->code);
    ip_print(src_ip);
    console_printf("\n");
}

int icmp_ping(const u8 *dst_ip, u16 seq, u32 ms)
{
    u8 message[ICMP_ECHO_HDR_LEN + ICMP_ECHO_DATA_LEN];
    struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr *)message;
    u64 flags;
    int result = 0;
    u32 deadline;
    u32 i;

    icmp->type       = ICMP_TYPE_ECHO_REQUEST;
    icmp->code       = 0U;
    icmp->checksum   = 0U;
    icmp->identifier = htons(ICMP_ECHO_ID);
    icmp->sequence   = htons(seq);
    for (i = 0U; i < ICMP_ECHO_DATA_LEN; i++) message[ICMP_ECHO_HDR_LEN + i] = (u8)i;
    icmp->checksum = htons(ip_checksum(message, sizeof(message)));

    g_ping_seq  = seq;
    g_ping_done = 0;

    console_printf("icmp: tx echo request to ");
    ip_print(dst_ip);
    console_printf(" id=0x%04X seq=%u\n", ICMP_ECHO_ID, seq);

    deadline = timer_ticks() + ms * timer_hz() / 1000U;

    if (ip_send(dst_ip, IP_PROTO_ICMP, message, sizeof(message)) != 0) {
        console_set_color(0x0CU);
        console_printf("icmp: tx echo request failed\n");
        return -1;
    }

    __asm__ volatile ("pushfq; pop %0" : "=r" (flags) : : "memory");

    for (;;) {
        __asm__ volatile ("cli" : : : "memory");

        if (g_ping_done) break;

        if ((int)(timer_ticks() - deadline) >= 0) {
            result = -1;
            break;
        }

        g_ping_waiter = thread_current();
        thread_sleep((deadline - timer_ticks()) * 1000U / timer_hz());
        g_ping_waiter = 0;
    }

    if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

    if (result != 0) {
        console_set_color(0x0CU);
        console_printf("icmp: no echo reply seq=%u\n", seq);
    }

    return result;
}

void icmp_test(void)
{
    static const u8 peer_ip[IP_ALEN] = { 10U, 0U, 2U, 99U };
    u8 packet[IP_HDR_LEN + ICMP_ECHO_HDR_LEN + ICMP_ECHO_DATA_LEN];
    struct ip_hdr *ip = (struct ip_hdr *)packet;
    struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr *)(packet + IP_HDR_LEN);
    u32 i;

    if (!ethernet_ready()) return;

    console_set_color(0x0BU);

    for (i = 1U; i <= ICMP_PING_COUNT; i++) {
        if (icmp_ping(ip_gateway, (u16)i, ICMP_WAIT_MS) == 0) {
            console_printf("icmp: ping ");
            ip_print(ip_gateway);
            console_printf(" seq=%u ok\n", i);
        }
    }

    ip->version_ihl = 0x45U;
    ip->dscp_ecn    = 0U;
    ip->total_len   = htons(sizeof(packet));
    ip->id          = htons(1U);
    ip->flags_frag  = htons(0x4000U);
    ip->ttl         = 64U;
    ip->protocol    = IP_PROTO_ICMP;
    ip->checksum    = 0U;
    for (i = 0U; i < IP_ALEN; i++) {
        ip->src[i] = peer_ip[i];
        ip->dst[i] = ip_local[i];
    }
    ip->checksum = htons(ip_checksum(packet, IP_HDR_LEN));

    icmp->type       = ICMP_TYPE_ECHO_REQUEST;
    icmp->code       = 0U;
    icmp->checksum   = 0U;
    icmp->identifier = htons(0x0099U);
    icmp->sequence   = htons(1U);
    for (i = 0U; i < ICMP_ECHO_DATA_LEN; i++) packet[IP_HDR_LEN + ICMP_ECHO_HDR_LEN + i] = (u8)i;
    icmp->checksum = htons(ip_checksum((const u8 *)icmp, ICMP_ECHO_HDR_LEN + ICMP_ECHO_DATA_LEN));

    ip_rx(packet, sizeof(packet));
}
