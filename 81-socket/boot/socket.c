#include "socket.h"
#include "kheap.h"
#include "tcp.h"

#define SOCKET_MAX 8U

#define IPPROTO_TCP 6U

#define EAGAIN          11
#define EINVAL          22
#define ENFILE          23
#define ENOMEM          12
#define EPIPE           32
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EAFNOSUPPORT    97
#define ENOTCONN        107

typedef struct {
    tcp_conn_t *conn;
    u32         refs;
    u32         used;
} socket_t;

static socket_t sockets[SOCKET_MAX];

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

    n = tcp_recv(s->conn, buf, len);
    if (n > 0U || tcp_eof(s->conn)) return n;
    return (u32)-EAGAIN;
}

static u32 socket_write(int bfd, const u8 *buf, u32 len, u32 pos)
{
    socket_t *s = &sockets[bfd];
    int       n;

    (void)pos;

    if (!s->conn) return (u32)-EPIPE;

    n = tcp_send(s->conn, buf, len);
    if (n < 0) return (u32)-EPIPE;
    if (n == 0 && len > 0U) return (u32)-EAGAIN;
    return (u32)n;
}

static void socket_close(int bfd)
{
    socket_t *s = &sockets[bfd];
    if (s->refs > 0U) s->refs--;
    if (s->refs == 0U) {
        if (s->conn) tcp_close(s->conn);
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

    s->conn = 0;
    s->refs = 1U;
    s->used = 1U;

    f->ops        = &socket_ops;
    f->backend_fd = (int)i;
    f->pos        = 0U;

    *out = f;
    return 0;
}
