#include "icmp.h"
#include "ethernet.h"
#include "ip.h"
#include "net_rx.h"
#include "thread.h"
#include "timer.h"

enum {
    ICMP_ECHO_HDR_LEN      = 8,
    ICMP_TYPE_ECHO_REPLY   = 0,
    ICMP_TYPE_ECHO_REQUEST = 8,
    ICMP_ECHO_ID           = 0x4F53,
    ICMP_ECHO_DATA_LEN     = 56,

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

    if (ip_send(src_ip, IP_PROTO_ICMP, reply, len) != 0) {
        console_printf("icmp: tx echo reply dropped\n");
    }
}

void icmp_rx(const u8 *src_ip, const u8 *packet, u32 len)
{
    const struct icmp_echo_hdr *icmp = (const struct icmp_echo_hdr *)packet;

    if (len < ICMP_ECHO_HDR_LEN) return;

    if (ip_checksum(packet, len) != 0U) return;

    if (icmp->type == ICMP_TYPE_ECHO_REQUEST && icmp->code == 0U) {
        icmp_echo_reply(src_ip, packet, len);
        return;
    }

    if (icmp->type == ICMP_TYPE_ECHO_REPLY && icmp->code == 0U) {
        if (ntohs(icmp->identifier) == ICMP_ECHO_ID && ntohs(icmp->sequence) == g_ping_seq) {
            g_ping_done = 1;
            if (g_ping_waiter) thread_unpark(g_ping_waiter);
        }
        return;
    }
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
