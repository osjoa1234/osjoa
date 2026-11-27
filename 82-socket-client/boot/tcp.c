#include "tcp.h"
#include "ethernet.h"
#include "thread.h"
#include "timer.h"
#include "wait_queue.h"

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

    TCP_TIME_WAIT_MS = 1000U,
    TCP_TIMER_MS     = 50U,

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
    u32 held;
    wait_queue_t wait;
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

static tcp_conn_t g_conns[TCP_CONN_SLOTS];
static tcp_listener_t g_listeners[TCP_LISTEN_SLOTS];
static u32 g_isn;

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

static void tcp_wake(tcp_conn_t *conn)
{
    wq_wake_all(&conn->wait);
}

static void tcp_set_state(tcp_conn_t *conn, u32 to)
{
    conn->state = to;
    tcp_wake(conn);
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

    tcp->checksum = htons(tcp_checksum(ip_local, dst_ip, segment, len));

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
            if (slot == 0 && !conn->held) slot = conn;
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
        slot->held           = 0U;
        wq_init(&slot->wait);
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

tcp_conn_t *tcp_connect(const u8 *dst_ip, u16 dst_port, u16 src_port, tcp_established_t handler)
{
    tcp_conn_t *conn = tcp_conn_open(TCP_SYN_SENT, src_port, dst_ip, dst_port, handler);

    if (conn == 0) return 0;

    conn->held = 1U;

    if (tcp_xmit_conn(conn, conn->snd_una, TCP_FLAG_SYN, 0U, 1) != 0) {
        conn->state = TCP_CLOSED;
        conn->held  = 0U;
        return 0;
    }

    return conn;
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
    if (conn == 0) return;

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
        tcp_ack_now(conn);
        return -1;
    }

    if (tcp_seq_lt(seg->ack, conn->snd_una)) return 0;

    conn->snd_una = seg->ack;
    conn->snd_wnd = seg->window;

    tcp_wake(conn);
    tcp_output(conn);

    return 0;
}

static void tcp_process_data(tcp_conn_t *conn, const struct tcp_seg *seg)
{
    u32 n;
    u32 i;

    if (seg->data_len == 0U) return;

    if (seg->seq != conn->rcv_nxt) {
        tcp_ack_now(conn);
        return;
    }

    n = tcp_min(seg->data_len, tcp_rcv_window(conn));

    for (i = 0U; i < n; i++) conn->rcv_buf[(conn->rcv_nxt + i) & TCP_BUF_MASK] = seg->raw[seg->hdr_len + i];

    __asm__ volatile ("" ::: "memory");
    conn->rcv_nxt += n;

    tcp_wake(conn);
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
    tcp_wake(conn);
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

    if (tcp_checksum(src_ip, ip_local, segment, len) != 0U) return;

    seg.src_ip   = src_ip;
    seg.raw      = segment;
    seg.data_len = len - seg.hdr_len;
    seg.src_port = ntohs(tcp->src_port);
    seg.dst_port = ntohs(tcp->dst_port);
    seg.seq      = ntohl(tcp->seq);
    seg.ack      = ntohl(tcp->ack);
    seg.window   = ntohs(tcp->window);
    seg.flags    = tcp->flags;

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

void tcp_release(tcp_conn_t *conn)
{
    conn->held = 0U;
}

u16 tcp_local_port(const tcp_conn_t *conn)
{
    return conn->local_port;
}

u16 tcp_remote_port(const tcp_conn_t *conn)
{
    return conn->remote_port;
}

const u8 *tcp_remote_ip(const tcp_conn_t *conn)
{
    return conn->remote_ip;
}

static int tcp_ready_connected(const tcp_conn_t *conn)
{
    return conn->state != TCP_SYN_SENT;
}

static int tcp_ready_readable(const tcp_conn_t *conn)
{
    return conn->rcv_nxt != conn->copied_seq || conn->rcv_fin || conn->state == TCP_CLOSED;
}

static int tcp_ready_writable(const tcp_conn_t *conn)
{
    int sendable = conn->state == TCP_ESTABLISHED || conn->state == TCP_CLOSE_WAIT;

    return !sendable || TCP_BUF_SIZE - (conn->write_seq - conn->snd_una) > 0U;
}

static void tcp_wait(tcp_conn_t *conn, int (*ready)(const tcp_conn_t *))
{
    u64 irq;

    for (;;) {
        __asm__ volatile ("pushfq; pop %0; cli" : "=r" (irq) : : "memory");

        if (ready(conn)) {
            if (irq & RFLAGS_IF) __asm__ volatile ("sti" : : : "memory");
            return;
        }

        wq_add(&conn->wait, thread_current());
        thread_park();
    }
}

int tcp_wait_connected(tcp_conn_t *conn)
{
    tcp_wait(conn, tcp_ready_connected);

    return conn->state == TCP_ESTABLISHED ? 0 : -1;
}

void tcp_wait_readable(tcp_conn_t *conn)
{
    tcp_wait(conn, tcp_ready_readable);
}

void tcp_wait_writable(tcp_conn_t *conn)
{
    tcp_wait(conn, tcp_ready_writable);
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
