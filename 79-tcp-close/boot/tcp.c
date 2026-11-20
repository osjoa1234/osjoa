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
    TCP_BUF_SIZE     = 4096,
    TCP_BUF_MASK     = TCP_BUF_SIZE - 1,
    TCP_ISN_PER_TICK = 2500U,
    TCP_ISN_BUMP     = 64000U,

    TCP_PORT_SERVER = 8080,
    TCP_PORT_CLIENT = 40001,
    TCP_PORT_HOST   = 5000,
    TCP_WAIT_MS     = 3000U,
    TCP_TIME_WAIT_MS = 1000U,
    TCP_TIMER_MS     = 50U,

    TCP_TEST_SEND   = 4000,
    TCP_TEST_RECV   = 6000,
    TCP_TEST_CHUNK  = 512,
    TCP_TEST_PAUSE  = 300U,

    RFLAGS_IF = 1U << 9
};

enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
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
    u32 write_seq;
    u32 snd_wnd;
    u32 rcv_nxt;
    u32 copied_seq;
    u32 rcv_wnd_adv;
    u32 rcv_fin;
    u32 fin_queued;
    u32 fin_sent;
    u32 tw_at;
    u16 peer_mss;
    tcp_established_t on_established;
    u8  snd_buf[TCP_BUF_SIZE];
    u8  rcv_buf[TCP_BUF_SIZE];
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
    u16 window;
    u8  flags;
};

static const char *const g_state_names[] = { "CLOSED", "SYN_SENT", "SYN_RECEIVED", "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };

static tcp_conn_t g_conns[TCP_CONN_SLOTS];
static tcp_listener_t g_listeners[TCP_LISTEN_SLOTS];
static u32 g_isn;

static thread_t *volatile g_waiter;
static tcp_conn_t *volatile g_active_conn;
static tcp_conn_t *volatile g_passive_conn;

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

static int tcp_seq_lt(u32 a, u32 b)
{
    return (int)(a - b) < 0;
}

static u32 tcp_min(u32 a, u32 b)
{
    return a < b ? a : b;
}

static u32 tcp_rcv_window(const tcp_conn_t *conn)
{
    return TCP_BUF_SIZE - (conn->rcv_nxt - conn->copied_seq);
}

static u32 tcp_rcv_ack(const tcp_conn_t *conn)
{
    return conn->rcv_nxt + conn->rcv_fin;
}

static int tcp_fin_acked(const tcp_conn_t *conn)
{
    return conn->fin_sent && conn->snd_una == conn->write_seq;
}

static int tcp_may_recv(u32 state)
{
    return state == TCP_ESTABLISHED || state == TCP_FIN_WAIT_1 || state == TCP_FIN_WAIT_2;
}

static u32 tcp_fill_hdr(u8 *segment, u16 src_port, u16 dst_port, u32 seq, u32 ack, u8 flags, u16 window, int with_mss)
{
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
    tcp->window      = htons(window);
    tcp->checksum    = 0U;
    tcp->urgent_ptr  = 0U;

    return hdr_len;
}

static int tcp_emit(const u8 *dst_ip, u8 *segment, u32 len)
{
    struct tcp_hdr *tcp = (struct tcp_hdr *)segment;
    u32 hdr_len = (u32)(tcp->data_offset >> 4) * 4U;

    tcp->checksum = htons(tcp_checksum(ip_local, dst_ip, segment, len));

    console_printf("tcp: tx ");
    ip_print(ip_local);
    console_printf(":%u -> ", ntohs(tcp->src_port));
    ip_print(dst_ip);
    console_printf(":%u ", ntohs(tcp->dst_port));
    tcp_print_flags(tcp->flags);
    console_printf(" seq=%u ack=%u", ntohl(tcp->seq), ntohl(tcp->ack));
    if (len > hdr_len) console_printf(" (%u bytes)", len - hdr_len);
    console_printf("\n");

    return ip_send(dst_ip, IP_PROTO_TCP, segment, len);
}

static int tcp_xmit(const u8 *dst_ip, u16 src_port, u16 dst_port, u32 seq, u32 ack, u8 flags)
{
    u8 segment[TCP_HDR_LEN];
    u32 hdr_len = tcp_fill_hdr(segment, src_port, dst_port, seq, ack, flags, 0U, 0);

    return tcp_emit(dst_ip, segment, hdr_len);
}

static int tcp_xmit_conn(tcp_conn_t *conn, u32 seq, u8 flags, u32 data_len, int with_mss)
{
    u8 segment[TCP_HDR_LEN + TCP_OPT_MSS_LEN + TCP_MSS];
    u32 window = tcp_rcv_window(conn);
    u32 hdr_len;
    u32 i;

    hdr_len = tcp_fill_hdr(segment, conn->local_port, conn->remote_port, seq, tcp_rcv_ack(conn), flags, (u16)window, with_mss);

    for (i = 0U; i < data_len; i++) segment[hdr_len + i] = conn->snd_buf[(seq + i) & TCP_BUF_MASK];

    conn->rcv_wnd_adv = window;

    return tcp_emit(conn->remote_ip, segment, hdr_len + data_len);
}

static void tcp_ack_now(tcp_conn_t *conn)
{
    tcp_xmit_conn(conn, conn->snd_nxt, TCP_FLAG_ACK, 0U, 0);
}

static void tcp_output(tcp_conn_t *conn)
{
    u32 mss = tcp_min(conn->peer_mss, TCP_MSS);
    u32 in_flight;
    u32 unsent;
    u32 len;
    u8  flags;
    u64 irq;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r" (irq) : : "memory");

    for (;;) {
        in_flight = conn->snd_nxt - conn->snd_una;
        unsent    = conn->write_seq - conn->snd_nxt;

        if (unsent == 0U || in_flight >= conn->snd_wnd) break;

        len   = tcp_min(tcp_min(unsent, conn->snd_wnd - in_flight), mss);
        flags = TCP_FLAG_ACK;
        if (len == unsent) flags |= TCP_FLAG_PSH;

        if (tcp_xmit_conn(conn, conn->snd_nxt, flags, len, 0) != 0) break;

        conn->snd_nxt += len;
    }

    if (conn->fin_queued && !conn->fin_sent && conn->snd_nxt == conn->write_seq) {
        if (tcp_xmit_conn(conn, conn->snd_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK, 0U, 0) == 0) {
            conn->snd_nxt++;
            conn->write_seq++;
            conn->fin_sent = 1U;
        }
    }

    if (irq & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");
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
        slot->write_seq      = slot->snd_nxt;
        slot->snd_wnd        = 0U;
        slot->rcv_nxt        = 0U;
        slot->copied_seq     = 0U;
        slot->rcv_wnd_adv    = 0U;
        slot->rcv_fin        = 0U;
        slot->fin_queued     = 0U;
        slot->fin_sent       = 0U;
        slot->tw_at          = 0U;
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

    if (tcp_xmit_conn(conn, conn->snd_una, TCP_FLAG_SYN, 0U, 1) != 0) {
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
        tcp_xmit(seg->src_ip, seg->dst_port, seg->src_port, seg->ack, 0U, TCP_FLAG_RST);
    } else {
        tcp_xmit(seg->src_ip, seg->dst_port, seg->src_port, 0U, seg->seq + tcp_seg_len(seg), TCP_FLAG_RST | TCP_FLAG_ACK);
    }
}

static void tcp_rx_listen(const tcp_listener_t *listener, const struct tcp_seg *seg)
{
    tcp_conn_t *conn;

    if (seg->flags & TCP_FLAG_RST) return;

    if (seg->flags & TCP_FLAG_ACK) {
        tcp_xmit(seg->src_ip, seg->dst_port, seg->src_port, seg->ack, 0U, TCP_FLAG_RST);
        return;
    }

    if (!(seg->flags & TCP_FLAG_SYN)) return;

    conn = tcp_conn_open(TCP_SYN_RECEIVED, seg->dst_port, seg->src_ip, seg->src_port, listener->on_established);
    if (conn == 0) {
        console_printf("tcp: connection table full, SYN dropped\n");
        return;
    }

    conn->rcv_nxt    = seg->seq + 1U;
    conn->copied_seq = conn->rcv_nxt;
    conn->snd_wnd    = seg->window;
    conn->peer_mss   = tcp_parse_mss(seg->raw, seg->hdr_len);

    tcp_xmit_conn(conn, conn->snd_una, TCP_FLAG_SYN | TCP_FLAG_ACK, 0U, 1);
}

static void tcp_rx_syn_sent(tcp_conn_t *conn, const struct tcp_seg *seg)
{
    int ack_ok = (seg->flags & TCP_FLAG_ACK) && seg->ack == conn->snd_nxt;

    if (seg->flags & TCP_FLAG_ACK) {
        if (!ack_ok) {
            if (!(seg->flags & TCP_FLAG_RST)) {
                tcp_xmit(seg->src_ip, seg->dst_port, seg->src_port, seg->ack, 0U, TCP_FLAG_RST);
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

    conn->rcv_nxt    = seg->seq + 1U;
    conn->copied_seq = conn->rcv_nxt;
    conn->snd_una    = seg->ack;
    conn->snd_wnd    = seg->window;
    conn->peer_mss   = tcp_parse_mss(seg->raw, seg->hdr_len);

    tcp_ack_now(conn);

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
        tcp_xmit(seg->src_ip, seg->dst_port, seg->src_port, seg->ack, 0U, TCP_FLAG_RST);
        return;
    }

    conn->snd_una = seg->ack;
    conn->snd_wnd = seg->window;

    tcp_set_state(conn, TCP_ESTABLISHED);
    if (conn->on_established) conn->on_established(conn);
}

static int tcp_process_ack(tcp_conn_t *conn, const struct tcp_seg *seg)
{
    if (tcp_seq_lt(conn->snd_nxt, seg->ack)) {
        console_printf("tcp: rx ack=%u beyond snd_nxt=%u\n", seg->ack, conn->snd_nxt);
        tcp_ack_now(conn);
        return -1;
    }

    if (tcp_seq_lt(seg->ack, conn->snd_una)) return 0;

    conn->snd_una = seg->ack;
    conn->snd_wnd = seg->window;

    tcp_output(conn);

    return 0;
}

static void tcp_process_data(tcp_conn_t *conn, const struct tcp_seg *seg)
{
    u32 n;
    u32 i;

    if (seg->data_len == 0U) return;

    if (seg->seq != conn->rcv_nxt) {
        console_printf("tcp: rx seq=%u but rcv_nxt=%u, dropped\n", seg->seq, conn->rcv_nxt);
        tcp_ack_now(conn);
        return;
    }

    n = tcp_min(seg->data_len, tcp_rcv_window(conn));

    for (i = 0U; i < n; i++) conn->rcv_buf[(conn->rcv_nxt + i) & TCP_BUF_MASK] = seg->raw[seg->hdr_len + i];

    __asm__ volatile ("" ::: "memory");
    conn->rcv_nxt += n;

    tcp_ack_now(conn);
}

static int tcp_process_fin(tcp_conn_t *conn, const struct tcp_seg *seg)
{
    if (!(seg->flags & TCP_FLAG_FIN)) return 0;

    if (seg->seq + seg->data_len != conn->rcv_nxt) return 0;

    if (conn->rcv_fin) {
        tcp_ack_now(conn);
        return 0;
    }

    conn->rcv_fin = 1U;
    tcp_ack_now(conn);

    return 1;
}

static void tcp_enter_time_wait(tcp_conn_t *conn)
{
    conn->tw_at = timer_ticks() + TCP_TIME_WAIT_MS * timer_hz() / 1000U;
    tcp_set_state(conn, TCP_TIME_WAIT);
}

static void tcp_advance_state(tcp_conn_t *conn, int fin)
{
    switch (conn->state) {
    case TCP_ESTABLISHED:
        if (fin) tcp_set_state(conn, TCP_CLOSE_WAIT);
        break;
    case TCP_FIN_WAIT_1:
        if (tcp_fin_acked(conn)) {
            if (fin) {
                tcp_enter_time_wait(conn);
            } else {
                tcp_set_state(conn, TCP_FIN_WAIT_2);
            }
        } else if (fin) {
            tcp_set_state(conn, TCP_CLOSING);
        }
        break;
    case TCP_FIN_WAIT_2:
        if (fin) tcp_enter_time_wait(conn);
        break;
    case TCP_CLOSING:
        if (tcp_fin_acked(conn)) tcp_enter_time_wait(conn);
        break;
    case TCP_LAST_ACK:
        if (tcp_fin_acked(conn)) tcp_set_state(conn, TCP_CLOSED);
        break;
    default:
        break;
    }
}

static void tcp_rx_synchronized(tcp_conn_t *conn, const struct tcp_seg *seg)
{
    int fin;

    if (seg->flags & TCP_FLAG_RST) {
        tcp_set_state(conn, TCP_CLOSED);
        return;
    }

    if (!(seg->flags & TCP_FLAG_ACK)) return;

    if (tcp_process_ack(conn, seg) != 0) return;

    tcp_process_data(conn, seg);

    fin = tcp_process_fin(conn, seg);

    tcp_advance_state(conn, fin);

    if (g_waiter) thread_unpark(g_waiter);
}

static void tcp_rx_time_wait(tcp_conn_t *conn, const struct tcp_seg *seg)
{
    if (seg->flags & TCP_FLAG_RST) {
        tcp_set_state(conn, TCP_CLOSED);
        return;
    }

    if (seg->flags & TCP_FLAG_FIN) {
        tcp_ack_now(conn);
        conn->tw_at = timer_ticks() + TCP_TIME_WAIT_MS * timer_hz() / 1000U;
    }
}

int tcp_send(tcp_conn_t *conn, const u8 *data, u32 len)
{
    u32 room;
    u32 n;
    u32 i;
    u64 irq;

    if (conn->state != TCP_ESTABLISHED && conn->state != TCP_CLOSE_WAIT) return -1;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r" (irq) : : "memory");

    room = TCP_BUF_SIZE - (conn->write_seq - conn->snd_una);
    n    = tcp_min(len, room);

    for (i = 0U; i < n; i++) conn->snd_buf[(conn->write_seq + i) & TCP_BUF_MASK] = data[i];

    __asm__ volatile ("" ::: "memory");
    conn->write_seq += n;

    tcp_output(conn);

    if (irq & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

    return (int)n;
}

u32 tcp_recv(tcp_conn_t *conn, u8 *buf, u32 max)
{
    u32 n = tcp_min(conn->rcv_nxt - conn->copied_seq, max);
    u32 i;

    for (i = 0U; i < n; i++) buf[i] = conn->rcv_buf[(conn->copied_seq + i) & TCP_BUF_MASK];

    __asm__ volatile ("" ::: "memory");
    conn->copied_seq += n;

    if (n != 0U && tcp_may_recv(conn->state) && (int)(tcp_rcv_window(conn) - conn->rcv_wnd_adv) >= TCP_MSS) {
        tcp_ack_now(conn);
    }

    return n;
}

static void tcp_rx_segment(const u8 *src_ip, const u8 *segment, u32 len)
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
    seg.window   = ntohs(tcp->window);
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
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
        tcp_rx_synchronized(conn, &seg);
        break;
    case TCP_TIME_WAIT:
        tcp_rx_time_wait(conn, &seg);
        break;
    default:
        break;
    }
}

void tcp_rx(const u8 *src_ip, const u8 *segment, u32 len)
{
    u64 irq;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r" (irq) : : "memory");

    tcp_rx_segment(src_ip, segment, len);

    if (irq & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");
}

int tcp_close(tcp_conn_t *conn)
{
    int rc = 0;
    u64 irq;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r" (irq) : : "memory");

    switch (conn->state) {
    case TCP_ESTABLISHED:
        conn->fin_queued = 1U;
        tcp_set_state(conn, TCP_FIN_WAIT_1);
        tcp_output(conn);
        break;
    case TCP_CLOSE_WAIT:
        conn->fin_queued = 1U;
        tcp_set_state(conn, TCP_LAST_ACK);
        tcp_output(conn);
        break;
    default:
        rc = -1;
        break;
    }

    if (irq & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");

    return rc;
}

int tcp_eof(const tcp_conn_t *conn)
{
    return conn->rcv_fin && conn->rcv_nxt == conn->copied_seq;
}

static void tcp_timer_thread(void)
{
    tcp_conn_t *conn;
    u64 irq;
    u32 i;

    for (;;) {
        thread_sleep(TCP_TIMER_MS);

        for (i = 0U; i < TCP_CONN_SLOTS; i++) {
            conn = &g_conns[i];

            __asm__ volatile ("pushfq; pop %0; cli" : "=r" (irq) : : "memory");

            if (conn->state == TCP_TIME_WAIT && (int)(timer_ticks() - conn->tw_at) >= 0) {
                tcp_set_state(conn, TCP_CLOSED);
                if (g_waiter) thread_unpark(g_waiter);
            }

            if (irq & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");
        }
    }
}

void tcp_init(void)
{
    if (thread_create(tcp_timer_thread) == 0) {
        console_set_color(0x0CU);
        console_printf("tcp: timer thread create failed\n");
        return;
    }

    console_set_color(0x0BU);
    console_printf("tcp: time-wait timer thread started\n");
}

static void tcp_active_handler(tcp_conn_t *conn)
{
    g_active_conn = conn;
    if (g_waiter) thread_unpark(g_waiter);
}

static void tcp_passive_handler(tcp_conn_t *conn)
{
    g_passive_conn = conn;
    if (g_waiter) thread_unpark(g_waiter);
}

static int tcp_cond_set(void *arg)
{
    return *(tcp_conn_t *volatile *)arg != 0;
}

static int tcp_cond_readable(void *arg)
{
    const tcp_conn_t *conn = arg;

    return conn->rcv_nxt != conn->copied_seq;
}

static int tcp_cond_acked(void *arg)
{
    const tcp_conn_t *conn = arg;

    return conn->snd_una == conn->write_seq;
}

static int tcp_cond_eof(void *arg)
{
    return tcp_eof(arg);
}

static int tcp_cond_closed(void *arg)
{
    const tcp_conn_t *conn = arg;

    return conn->state == TCP_CLOSED;
}

static int tcp_wait(int (*cond)(void *), void *arg, u32 ms)
{
    u64 flags;
    int result = 0;
    u32 deadline = timer_ticks() + ms * timer_hz() / 1000U;

    __asm__ volatile ("pushfq; pop %0" : "=r" (flags) : : "memory");

    for (;;) {
        __asm__ volatile ("cli" : : : "memory");

        if (cond(arg)) break;

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

static u8 tcp_pattern(u32 i)
{
    return (u8)('A' + i % 26U);
}

static int tcp_test_send(tcp_conn_t *conn)
{
    u8 data[TCP_TEST_SEND];
    u32 i;

    for (i = 0U; i < TCP_TEST_SEND; i++) data[i] = tcp_pattern(i);

    if (tcp_send(conn, data, TCP_TEST_SEND) != TCP_TEST_SEND) return -1;

    console_printf("tcp: queued %u bytes\n", TCP_TEST_SEND);

    if (tcp_wait(tcp_cond_acked, conn, TCP_WAIT_MS) != 0) return -1;

    console_printf("tcp: %u bytes acked\n", TCP_TEST_SEND);

    return 0;
}

static int tcp_test_recv(tcp_conn_t *conn)
{
    u8 chunk[TCP_TEST_CHUNK];
    u32 total = 0U;
    u32 bad = 0U;
    u32 n;
    u32 i;

    thread_sleep(TCP_TEST_PAUSE);

    console_printf("tcp: receive buffer holds %u bytes, window %u\n", conn->rcv_nxt - conn->copied_seq, tcp_rcv_window(conn));

    while (total < TCP_TEST_RECV) {
        n = tcp_recv(conn, chunk, TCP_TEST_CHUNK);

        if (n == 0U) {
            if (tcp_wait(tcp_cond_readable, conn, TCP_WAIT_MS) != 0) return -1;
            continue;
        }

        for (i = 0U; i < n; i++) {
            if (chunk[i] != tcp_pattern(total + i)) bad++;
        }

        total += n;
    }

    console_printf("tcp: received %u bytes, %u mismatched\n", total, bad);

    return bad == 0U ? 0 : -1;
}

static int tcp_test_echo(tcp_conn_t *conn)
{
    u8 data[TCP_TEST_CHUNK];
    u32 n;

    if (tcp_wait(tcp_cond_readable, conn, TCP_WAIT_MS) != 0) return -1;

    n = tcp_recv(conn, data, TCP_TEST_CHUNK - 1U);
    data[n] = 0U;
    console_printf("tcp: passive rx %u bytes \"%s\"\n", n, data);

    if (tcp_send(conn, data, n) != (int)n) return -1;

    return tcp_wait(tcp_cond_acked, conn, TCP_WAIT_MS);
}

static int tcp_test_close_passive(tcp_conn_t *conn)
{
    if (tcp_wait(tcp_cond_eof, conn, TCP_WAIT_MS) != 0) return -1;

    console_printf("tcp: passive peer closed, EOF\n");

    if (tcp_close(conn) != 0) return -1;

    return tcp_wait(tcp_cond_closed, conn, TCP_WAIT_MS);
}

static int tcp_test_close_active(tcp_conn_t *conn)
{
    u8 data[TCP_TEST_CHUNK];
    u32 n;

    if (tcp_close(conn) != 0) return -1;

    if (tcp_wait(tcp_cond_readable, conn, TCP_WAIT_MS) != 0) return -1;

    n = tcp_recv(conn, data, TCP_TEST_CHUNK - 1U);
    data[n] = 0U;
    console_printf("tcp: active rx %u bytes \"%s\" after close\n", n, data);

    if (tcp_wait(tcp_cond_eof, conn, TCP_WAIT_MS) != 0) return -1;

    console_printf("tcp: active peer closed, EOF\n");

    return tcp_wait(tcp_cond_closed, conn, TCP_WAIT_MS + TCP_TIME_WAIT_MS);
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

    g_active_conn  = 0;
    g_passive_conn = 0;

    if (tcp_connect(host_ip, TCP_PORT_HOST, TCP_PORT_CLIENT, tcp_active_handler) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: connect failed\n");
    } else if (tcp_wait(tcp_cond_set, (void *)&g_active_conn, TCP_WAIT_MS) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: connect timeout\n");
    } else if (tcp_wait(tcp_cond_set, (void *)&g_passive_conn, TCP_WAIT_MS) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: no passive open\n");
    } else if (tcp_test_send(g_active_conn) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: send failed\n");
    } else if (tcp_test_recv(g_active_conn) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: recv failed\n");
    } else if (tcp_test_echo(g_passive_conn) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: echo failed\n");
    } else if (tcp_test_close_passive(g_passive_conn) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: passive close failed\n");
    } else if (tcp_test_close_active(g_active_conn) != 0) {
        console_set_color(0x0CU);
        console_printf("tcp: active close failed\n");
    }

    console_set_color(0x0BU);
}
