#include "tcp.h"
#include "ethernet.h"
#include "thread.h"
#include "timer.h"

enum {
    TCP_FLAG_FIN = 0x01,
    TCP_FLAG_SYN = 0x02,
    TCP_FLAG_RST = 0x04,
    TCP_FLAG_PSH = 0x08,
    TCP_FLAG_ACK = 0x10,

    TCP_OPT_END     = 0,
    TCP_OPT_NOP     = 1,
    TCP_OPT_MSS     = 2,
    TCP_OPT_MSS_LEN = 4,
    TCP_DEFAULT_MSS = 536,

    TCP_CONN_SLOTS   = 6,
    TCP_LISTEN_SLOTS = 4,
    TCP_WINDOW       = 4096,
    TCP_ISN_PER_TICK = 2500U,
    TCP_ISN_BUMP     = 64000U,

    TCP_PORT_SERVER = 8080,
    TCP_PORT_CLIENT = 40001,
    TCP_PORT_HOST   = 5000,
    TCP_WAIT_MS     = 3000U,

    RFLAGS_IF = 1U << 9
};

enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED
};

struct tcp_hdr {
    u16 src_port;
    u16 dst_port;
    u32 seq;
    u32 ack;
    u8  data_offset;
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urgent_ptr;
} __attribute__((packed));

struct tcp_conn {
    volatile u32 state;
    u16 local_port;
    u16 remote_port;
    u8  remote_ip[IP_ALEN];
    u32 snd_una;
    u32 snd_nxt;
    u32 rcv_nxt;
    u16 peer_mss;
    tcp_established_t on_established;
};

struct tcp_listener {
    volatile u32 active;
    u16 port;
    tcp_established_t on_established;
};

typedef struct tcp_listener tcp_listener_t;

struct tcp_seg {
    const u8 *src_ip;
    const u8 *raw;
    u32 hdr_len;
    u32 data_len;
    u16 src_port;
    u16 dst_port;
    u32 seq;
    u32 ack;
    u8  flags;
};

static const char *const g_state_names[] = { "CLOSED", "SYN_SENT", "SYN_RECEIVED", "ESTABLISHED" };

static tcp_conn_t g_conns[TCP_CONN_SLOTS];
static tcp_listener_t g_listeners[TCP_LISTEN_SLOTS];
static u32 g_isn;

static thread_t *volatile g_waiter;
static volatile int g_active_done;
static volatile int g_passive_done;

static u32 tcp_isn(void)
{
    u32 isn = timer_ticks() * TCP_ISN_PER_TICK + g_isn;

    g_isn += TCP_ISN_BUMP;

    return isn;
}

static u16 tcp_checksum(const u8 *src_ip, const u8 *dst_ip, const u8 *segment, u32 len)
{
    u32 sum = ip_pseudo_sum(src_ip, dst_ip, IP_PROTO_TCP, (u16)len);

    sum = ip_checksum_add(sum, segment, len);

    return ip_checksum_finish(sum);
}

static void tcp_print_flags(u8 flags)
{
    static const char *const names[] = { "FIN", "SYN", "RST", "PSH", "ACK" };
    int first = 1;
    u32 i;

    console_printf("[");
    for (i = 0U; i < 5U; i++) {
        if (flags & (1U << i)) {
            console_printf("%s%s", first ? "" : "|", names[i]);
            first = 0;
        }
    }
    console_printf("]");
}

static void tcp_log_state(const tcp_conn_t *conn, u32 from, u32 to)
{
    console_printf("tcp: %u <-> ", conn->local_port);
    ip_print(conn->remote_ip);
    console_printf(":%u %s -> %s\n", conn->remote_port, g_state_names[from], g_state_names[to]);
}

static void tcp_set_state(tcp_conn_t *conn, u32 to)
{
    tcp_log_state(conn, conn->state, to);
    conn->state = to;
}

static u16 tcp_parse_mss(const u8 *segment, u32 hdr_len)
{
    u32 i = TCP_HDR_LEN;
    u32 opt_len;

    while (i < hdr_len) {
        if (segment[i] == TCP_OPT_END) break;
        if (segment[i] == TCP_OPT_NOP) {
            i++;
            continue;
        }

        if (i + 1U >= hdr_len) break;
        opt_len = segment[i + 1U];
        if (opt_len < 2U || i + opt_len > hdr_len) break;

        if (segment[i] == TCP_OPT_MSS && opt_len == TCP_OPT_MSS_LEN) {
            return (u16)(((u16)segment[i + 2U] << 8) | segment[i + 3U]);
        }

        i += opt_len;
    }

    return TCP_DEFAULT_MSS;
}

static int tcp_xmit(const u8 *dst_ip, u16 src_port, u16 dst_port, u32 seq, u32 ack, u8 flags, int with_mss)
{
    u8 segment[TCP_HDR_LEN + TCP_OPT_MSS_LEN];
    struct tcp_hdr *tcp = (struct tcp_hdr *)segment;
    u32 hdr_len = TCP_HDR_LEN;

    if (with_mss) {
        segment[TCP_HDR_LEN]      = TCP_OPT_MSS;
        segment[TCP_HDR_LEN + 1U] = TCP_OPT_MSS_LEN;
        segment[TCP_HDR_LEN + 2U] = (u8)(TCP_MSS >> 8);
        segment[TCP_HDR_LEN + 3U] = (u8)(TCP_MSS & 0xFFU);
        hdr_len += TCP_OPT_MSS_LEN;
    }

    tcp->src_port    = htons(src_port);
    tcp->dst_port    = htons(dst_port);
    tcp->seq         = htonl(seq);
    tcp->ack         = htonl(ack);
    tcp->data_offset = (u8)((hdr_len / 4U) << 4);
    tcp->flags       = flags;
    tcp->window      = htons(TCP_WINDOW);
    tcp->checksum    = 0U;
    tcp->urgent_ptr  = 0U;
    tcp->checksum    = htons(tcp_checksum(ip_local, dst_ip, segment, hdr_len));

    console_printf("tcp: tx ");
    ip_print(ip_local);
    console_printf(":%u -> ", src_port);
    ip_print(dst_ip);
    console_printf(":%u ", dst_port);
    tcp_print_flags(flags);
    console_printf(" seq=%u ack=%u\n", seq, ack);

    return ip_send(dst_ip, IP_PROTO_TCP, segment, hdr_len);
}

static tcp_conn_t *tcp_conn_open(u32 state, u16 local_port, const u8 *remote_ip, u16 remote_port, tcp_established_t handler)
{
    tcp_conn_t *slot = 0;
    tcp_conn_t *conn;
    int dup = 0;
    u64 flags;
    u32 i;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r" (flags) : : "memory");

    for (i = 0U; i < TCP_CONN_SLOTS; i++) {
        conn = &g_conns[i];

        if (conn->state == TCP_CLOSED) {
            if (slot == 0) slot = conn;
            continue;
        }

        dup = conn->local_port == local_port && conn->remote_port == remote_port && ip_equal(conn->remote_ip, remote_ip);

        if (dup) break;
    }

    if (dup) slot = 0;

    if (slot != 0) {
        slot->local_port  = local_port;
        slot->remote_port = remote_port;
        for (i = 0U; i < IP_ALEN; i++) slot->remote_ip[i] = remote_ip[i];
        slot->snd_una        = tcp_isn();
        slot->snd_nxt        = slot->snd_una + 1U;
        slot->rcv_nxt        = 0U;
        slot->peer_mss       = TCP_DEFAULT_MSS;
        slot->on_established = handler;
        __asm__ volatile ("" ::: "memory");
        slot->state = state;
    }

    if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

    return slot;
}

static tcp_conn_t *tcp_conn_find(const struct tcp_seg *seg)
{
    tcp_conn_t *conn;
    u32 i;

    for (i = 0U; i < TCP_CONN_SLOTS; i++) {
        conn = &g_conns[i];

        if (conn->state == TCP_CLOSED) continue;

        if (conn->local_port == seg->dst_port && conn->remote_port == seg->src_port && ip_equal(conn->remote_ip, seg->src_ip)) {
            return conn;
        }
    }

    return 0;
}

static tcp_listener_t *tcp_listener_open(u16 port, tcp_established_t handler)
{
    tcp_listener_t *slot = 0;
    tcp_listener_t *listener;
    int dup = 0;
    u64 flags;
    u32 i;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r" (flags) : : "memory");

    for (i = 0U; i < TCP_LISTEN_SLOTS; i++) {
        listener = &g_listeners[i];

        if (!listener->active) {
            if (slot == 0) slot = listener;
            continue;
        }

        if (listener->port == port) {
            dup = 1;
            break;
        }
    }

    if (dup) slot = 0;

    if (slot != 0) {
        slot->port           = port;
        slot->on_established = handler;
        __asm__ volatile ("" ::: "memory");
        slot->active = 1U;
    }

    if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

    return slot;
}

static tcp_listener_t *tcp_listener_find(u16 port)
{
    tcp_listener_t *listener;
    u32 i;

    for (i = 0U; i < TCP_LISTEN_SLOTS; i++) {
        listener = &g_listeners[i];

        if (listener->active && listener->port == port) return listener;
    }

    return 0;
}

int tcp_listen(u16 port, tcp_established_t handler)
{
    return tcp_listener_open(port, handler) != 0 ? 0 : -1;
}

int tcp_connect(const u8 *dst_ip, u16 dst_port, u16 src_port, tcp_established_t handler)
{
    tcp_conn_t *conn = tcp_conn_open(TCP_SYN_SENT, src_port, dst_ip, dst_port, handler);

    if (conn == 0) return -1;

    tcp_log_state(conn, TCP_CLOSED, TCP_SYN_SENT);

    if (tcp_xmit(dst_ip, src_port, dst_port, conn->snd_una, 0U, TCP_FLAG_SYN, 1) != 0) {
        conn->state = TCP_CLOSED;
        return -1;
    }

    return 0;
}

static u32 tcp_seg_len(const struct tcp_seg *seg)
{
    u32 len = seg->data_len;

    if (seg->flags & TCP_FLAG_SYN) len++;
    if (seg->flags & TCP_FLAG_FIN) len++;

    return len;
}

static void tcp_rx_closed(const struct tcp_seg *seg)
{
    if (seg->flags & TCP_FLAG_RST) return;

    console_printf("tcp: rx port %u has no connection, RST\n", seg->dst_port);

    if (seg->flags & TCP_FLAG_ACK) {
        tcp_xmit(seg->src_ip, seg->dst_port, seg->src_port, seg->ack, 0U, TCP_FLAG_RST, 0);
    } else {
        tcp_xmit(seg->src_ip, seg->dst_port, seg->src_port, 0U, seg->seq + tcp_seg_len(seg), TCP_FLAG_RST | TCP_FLAG_ACK, 0);
    }
}

static void tcp_rx_listen(const tcp_listener_t *listener, const struct tcp_seg *seg)
{
    tcp_conn_t *conn;

    if (seg->flags & TCP_FLAG_RST) return;

    if (seg->flags & TCP_FLAG_ACK) {
        tcp_xmit(seg->src_ip, seg->dst_port, seg->src_port, seg->ack, 0U, TCP_FLAG_RST, 0);
        return;
    }

    if (!(seg->flags & TCP_FLAG_SYN)) return;

    conn = tcp_conn_open(TCP_SYN_RECEIVED, seg->dst_port, seg->src_ip, seg->src_port, listener->on_established);
    if (conn == 0) {
        console_printf("tcp: connection table full, SYN dropped\n");
        return;
    }

    conn->rcv_nxt  = seg->seq + 1U;
    conn->peer_mss = tcp_parse_mss(seg->raw, seg->hdr_len);

    tcp_xmit(conn->remote_ip, conn->local_port, conn->remote_port, conn->snd_una, conn->rcv_nxt, TCP_FLAG_SYN | TCP_FLAG_ACK, 1);
}

static void tcp_rx_syn_sent(tcp_conn_t *conn, const struct tcp_seg *seg)
{
    int ack_ok = (seg->flags & TCP_FLAG_ACK) && seg->ack == conn->snd_nxt;

    if (seg->flags & TCP_FLAG_ACK) {
        if (!ack_ok) {
            if (!(seg->flags & TCP_FLAG_RST)) {
                tcp_xmit(seg->src_ip, seg->dst_port, seg->src_port, seg->ack, 0U, TCP_FLAG_RST, 0);
            }
            return;
        }

        if (seg->flags & TCP_FLAG_RST) {
            console_printf("tcp: connection refused\n");
            tcp_set_state(conn, TCP_CLOSED);
            return;
        }
    }

    if (!ack_ok || !(seg->flags & TCP_FLAG_SYN)) return;

    conn->rcv_nxt  = seg->seq + 1U;
    conn->snd_una  = seg->ack;
    conn->peer_mss = tcp_parse_mss(seg->raw, seg->hdr_len);

    tcp_xmit(conn->remote_ip, conn->local_port, conn->remote_port, conn->snd_nxt, conn->rcv_nxt, TCP_FLAG_ACK, 0);

    tcp_set_state(conn, TCP_ESTABLISHED);
    if (conn->on_established) conn->on_established(conn);
}

static void tcp_rx_syn_received(tcp_conn_t *conn, const struct tcp_seg *seg)
{
    if (seg->flags & TCP_FLAG_RST) {
        if (seg->seq == conn->rcv_nxt) tcp_set_state(conn, TCP_CLOSED);
        return;
    }

    if (!(seg->flags & TCP_FLAG_ACK)) return;

    if (seg->ack != conn->snd_nxt) {
        tcp_xmit(seg->src_ip, seg->dst_port, seg->src_port, seg->ack, 0U, TCP_FLAG_RST, 0);
        return;
    }

    conn->snd_una = seg->ack;

    tcp_set_state(conn, TCP_ESTABLISHED);
    if (conn->on_established) conn->on_established(conn);
}

static void tcp_rx_established(tcp_conn_t *conn, const struct tcp_seg *seg)
{
    if (seg->flags & TCP_FLAG_RST) {
        tcp_set_state(conn, TCP_CLOSED);
        return;
    }

    console_printf("tcp: rx in ESTABLISHED ignored, ");
    tcp_print_flags(seg->flags);
    console_printf(" seq=%u (%u bytes)\n", seg->seq, seg->data_len);
}

void tcp_rx(const u8 *src_ip, const u8 *segment, u32 len)
{
    const struct tcp_hdr *tcp = (const struct tcp_hdr *)segment;
    struct tcp_seg seg;
    tcp_conn_t *conn;
    tcp_listener_t *listener;

    if (len < TCP_HDR_LEN) return;

    seg.hdr_len = (u32)(tcp->data_offset >> 4) * 4U;
    if (seg.hdr_len < TCP_HDR_LEN || seg.hdr_len > len) return;

    if (tcp_checksum(src_ip, ip_local, segment, len) != 0U) {
        console_printf("tcp: rx bad checksum from ");
        ip_print(src_ip);
        console_printf("\n");
        return;
    }

    seg.src_ip   = src_ip;
    seg.raw      = segment;
    seg.data_len = len - seg.hdr_len;
    seg.src_port = ntohs(tcp->src_port);
    seg.dst_port = ntohs(tcp->dst_port);
    seg.seq      = ntohl(tcp->seq);
    seg.ack      = ntohl(tcp->ack);
    seg.flags    = tcp->flags;

    console_printf("tcp: rx ");
    ip_print(src_ip);
    console_printf(":%u -> ", seg.src_port);
    ip_print(ip_local);
    console_printf(":%u ", seg.dst_port);
    tcp_print_flags(seg.flags);
    console_printf(" seq=%u ack=%u\n", seg.seq, seg.ack);

    conn = tcp_conn_find(&seg);
    if (conn == 0) {
        listener = tcp_listener_find(seg.dst_port);
        if (listener != 0) {
            tcp_rx_listen(listener, &seg);
        } else {
            tcp_rx_closed(&seg);
        }
        return;
    }

    switch (conn->state) {
    case TCP_SYN_SENT:
        tcp_rx_syn_sent(conn, &seg);
        break;
    case TCP_SYN_RECEIVED:
        tcp_rx_syn_received(conn, &seg);
        break;
    case TCP_ESTABLISHED:
        tcp_rx_established(conn, &seg);
        break;
    default:
        break;
    }
}

static void tcp_active_handler(tcp_conn_t *conn)
{
    (void)conn;

    g_active_done = 1;
    if (g_waiter) thread_unpark(g_waiter);
}

static void tcp_passive_handler(tcp_conn_t *conn)
{
    (void)conn;

    g_passive_done = 1;
    if (g_waiter) thread_unpark(g_waiter);
}

static int tcp_wait(volatile int *done, u32 ms)
{
    u64 flags;
    int result = 0;
    u32 deadline = timer_ticks() + ms * timer_hz() / 1000U;

    __asm__ volatile ("pushfq; pop %0" : "=r" (flags) : : "memory");

    for (;;) {
        __asm__ volatile ("cli" : : : "memory");

        if (*done) break;

        if ((int)(timer_ticks() - deadline) >= 0) {
            result = -1;
            break;
        }

        g_waiter = thread_current();
        thread_sleep((deadline - timer_ticks()) * 1000U / timer_hz());
        g_waiter = 0;
    }

    if (flags & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

    return result;
}

void tcp_test(void)
{
    static const u8 host_ip[IP_ALEN] = { 10U, 0U, 2U, 2U };

    if (!ethernet_ready()) return;

    console_set_color(0x0BU);

    if (tcp_listen(TCP_PORT_SERVER, tcp_passive_handler) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: listen failed\n");
        return;
    }

    g_active_done  = 0;
    g_passive_done = 0;

    if (tcp_connect(host_ip, TCP_PORT_HOST, TCP_PORT_CLIENT, tcp_active_handler) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: connect failed\n");
    } else if (tcp_wait(&g_active_done, TCP_WAIT_MS) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: connect timeout\n");
    } else if (tcp_wait(&g_passive_done, TCP_WAIT_MS) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: no passive open\n");
    }

    console_set_color(0x0BU);
}
