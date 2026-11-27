#include "socket.h"
#include "kheap.h"
#include "tcp.h"
#include "ethernet.h"

#define SOCKET_MAX 8U

#define IPPROTO_TCP 6U

#define EPORT_FIRST 49152U

#define ENOMEM          12
#define EINVAL          22
#define ENFILE          23
#define EPIPE           32
#define ENOTSOCK        88
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EAFNOSUPPORT    97
#define ECONNRESET      104
#define ENOBUFS         105
#define EISCONN         106
#define ENOTCONN        107
#define ECONNREFUSED    111

typedef struct {
    tcp_conn_t *conn;
    u32         refs;
    u32         used;
    u32         shut_rd;
} socket_t;

typedef struct {
    u16 family;
    u16 port;
    u8  addr[IP_ALEN];
    u8  zero[8];
} sockaddr_in_t;

static socket_t sockets[SOCKET_MAX];
static u16      next_port = EPORT_FIRST;

void socket_init(void)
{
    u32 i;
    for (i = 0U; i < SOCKET_MAX; i++) sockets[i].used = 0U;
}

static u32 socket_read(int bfd, u8 *buf, u32 len, u32 pos)
{
    socket_t *s = &sockets[bfd];
    u32       n;

    (void)pos;

    if (!s->conn) return (u32)-ENOTCONN;
    if (len == 0U) return 0U;

    if (!s->shut_rd) tcp_wait_readable(s->conn);

    n = tcp_recv(s->conn, buf, len);
    if (n > 0U || tcp_eof(s->conn) || s->shut_rd) return n;
    return (u32)-ECONNRESET;
}

static u32 socket_write(int bfd, const u8 *buf, u32 len, u32 pos)
{
    socket_t *s    = &sockets[bfd];
    u32       sent = 0U;
    int       n;

    (void)pos;

    if (!s->conn) return (u32)-EPIPE;

    while (sent < len) {
        tcp_wait_writable(s->conn);

        n = tcp_send(s->conn, buf + sent, len - sent);
        if (n < 0) return sent ? sent : (u32)-EPIPE;
        sent += (u32)n;
    }
    return sent;
}

static void socket_close(int bfd)
{
    socket_t *s = &sockets[bfd];
    if (s->refs > 0U) s->refs--;
    if (s->refs == 0U) {
        if (s->conn) {
            tcp_close(s->conn);
            tcp_release(s->conn);
        }
        s->conn = 0;
        s->used = 0U;
    }
}

static void socket_dup(int bfd) { sockets[bfd].refs++; }

#define SOCKET_MODE (0140000U | 0777U)

static u32 socket_mode(int bfd) { (void)bfd; return SOCKET_MODE; }

static vfs_ops_t socket_ops = { 0, socket_read, socket_write, 0, socket_close, socket_dup, 0, socket_mode };

int socket_create(u32 domain, u32 type, u32 protocol, vfs_file_t **out)
{
    socket_t   *s = 0;
    vfs_file_t *f;
    u32         i;

    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (type & ~(SOCK_TYPE_MASK | SOCK_CLOEXEC)) return -EINVAL;
    if ((type & SOCK_TYPE_MASK) != SOCK_STREAM) return -ESOCKTNOSUPPORT;
    if (protocol != 0U && protocol != IPPROTO_TCP) return -EPROTONOSUPPORT;

    for (i = 0U; i < SOCKET_MAX; i++) {
        if (!sockets[i].used) { s = &sockets[i]; break; }
    }
    if (!s) return -ENFILE;

    f = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!f) return -ENOMEM;

    s->conn    = 0;
    s->refs    = 1U;
    s->used    = 1U;
    s->shut_rd = 0U;

    f->ops        = &socket_ops;
    f->backend_fd = (int)i;
    f->pos        = 0U;

    *out = f;
    return 0;
}

static socket_t *socket_of(vfs_file_t *f)
{
    if (f->ops != &socket_ops) return 0;
    return &sockets[f->backend_fd];
}

int socket_connect(vfs_file_t *f, const u8 *addr, u32 addrlen)
{
    socket_t            *s = socket_of(f);
    const sockaddr_in_t *sin = (const sockaddr_in_t *)addr;
    tcp_conn_t          *conn;
    u16                  port;

    if (!s) return -ENOTSOCK;
    if (addrlen < sizeof(sockaddr_in_t)) return -EINVAL;
    if (sin->family != AF_INET) return -EAFNOSUPPORT;
    if (s->conn) return -EISCONN;

    port = next_port;
    next_port = (next_port == 0xFFFFU) ? EPORT_FIRST : (u16)(next_port + 1U);

    conn = tcp_connect(sin->addr, ntohs(sin->port), port, 0);
    if (!conn) return -ENOBUFS;

    s->conn = conn;
    if (tcp_wait_connected(conn) != 0) {
        tcp_release(conn);
        s->conn = 0;
        return -ECONNREFUSED;
    }
    return 0;
}

int socket_shutdown(vfs_file_t *f, u32 how)
{
    socket_t *s = socket_of(f);

    if (!s) return -ENOTSOCK;
    if (how > SHUT_RDWR) return -EINVAL;
    if (!s->conn) return -ENOTCONN;

    if (how != SHUT_WR) s->shut_rd = 1U;
    if (how != SHUT_RD) tcp_close(s->conn);
    return 0;
}

int socket_getname(vfs_file_t *f, u8 *addr, u32 *addrlen, int peer)
{
    socket_t     *s = socket_of(f);
    sockaddr_in_t sin;
    const u8     *src = (const u8 *)&sin;
    u32           i;
    u32           n;

    if (!s) return -ENOTSOCK;
    if (peer && !s->conn) return -ENOTCONN;

    for (i = 0U; i < sizeof(sin); i++) ((u8 *)&sin)[i] = 0U;
    sin.family = AF_INET;

    if (s->conn) {
        if (peer) {
            for (i = 0U; i < IP_ALEN; i++) sin.addr[i] = tcp_remote_ip(s->conn)[i];
            sin.port = htons(tcp_remote_port(s->conn));
        } else {
            for (i = 0U; i < IP_ALEN; i++) sin.addr[i] = ip_local[i];
            sin.port = htons(tcp_local_port(s->conn));
        }
    }

    n = *addrlen < sizeof(sin) ? *addrlen : (u32)sizeof(sin);
    for (i = 0U; i < n; i++) addr[i] = src[i];
    *addrlen = sizeof(sin);
    return 0;
}
