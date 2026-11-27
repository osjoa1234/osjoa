#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BIG_LEN 6000

static char tx[BIG_LEN];
static char rx[BIG_LEN];

static ssize_t read_full(int fd, char *buf, size_t len)
{
    size_t  got = 0;
    ssize_t n;

    while (got < len) {
        n = read(fd, buf + got, len - got);
        if (n <= 0) return got ? (ssize_t)got : n;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

static void print_addr(const char *what, int fd, int peer)
{
    struct sockaddr_in a;
    socklen_t          len = sizeof(a);
    char               ip[INET_ADDRSTRLEN];
    int                r;

    r = peer ? getpeername(fd, (struct sockaddr *)&a, &len) : getsockname(fd, (struct sockaddr *)&a, &len);
    if (r < 0) {
        printf("echo: %s: %s\n", what, strerror(errno));
        return;
    }
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    printf("echo: %s %s:%d len=%d\n", what, ip, ntohs(a.sin_port), (int)len);
}

int main(int argc, char **argv)
{
    struct sockaddr_in peer;
    const char        *msg = "hello socket\n";
    char               buf[64];
    int                port = argc > 1 ? atoi(argv[1]) : 7000;
    int                fd;
    ssize_t            n;
    int                i;

    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(port);
    inet_pton(AF_INET, "10.0.2.2", &peer.sin_addr);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("echo: socket fd=%d\n", fd);

    print_addr("getpeername before connect", fd, 1);

    printf("echo: connect 10.0.2.2:%d\n", port);
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        printf("echo: connect: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("echo: connect ok\n");

    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0)
        printf("echo: connect again: %s\n", strerror(errno));

    print_addr("local", fd, 0);
    print_addr("peer ", fd, 1);

    n = write(fd, msg, strlen(msg));
    printf("echo: sent %zd bytes\n", n);
    n = read_full(fd, buf, strlen(msg));
    printf("echo: recv %zd bytes: %.*s", n, (int)n, buf);

    for (i = 0; i < BIG_LEN; i++) tx[i] = (char)('A' + (i * 7) % 26);
    n = write(fd, tx, BIG_LEN);
    printf("echo: big write=%zd\n", n);
    n = read_full(fd, rx, BIG_LEN);
    printf("echo: big read=%zd %s\n", n, n == BIG_LEN && memcmp(tx, rx, BIG_LEN) == 0 ? "OK" : "FAIL");

    shutdown(fd, SHUT_WR);
    n = read(fd, buf, sizeof(buf));
    printf("echo: read after peer close=%zd\n", n);
    close(fd);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    peer.sin_port = htons(7999);
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0)
        printf("echo: connect to closed port: %s\n", strerror(errno));
    close(fd);

    return 0;
}
