static void sys_write(const char *s)
{
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(0U), "b"(s) : "memory");
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("int $0x80" : : "a"(1U), "b"(code));
}

static unsigned int sys_open(const char *path)
{
    unsigned int fd;
    __asm__ volatile ("int $0x80" : "=a"(fd) : "a"(2U), "b"(path));
    return fd;
}

static unsigned int sys_read(unsigned int fd, char *buf, unsigned int len)
{
    unsigned int n;
    __asm__ volatile ("int $0x80" : "=a"(n) : "a"(3U), "b"(fd), "c"(buf), "d"(len) : "memory");
    return n;
}

static void sys_close(unsigned int fd)
{
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(10U), "b"(fd) : "memory");
}

void _start(void)
{
    char         buf[64];
    unsigned int fd;
    unsigned int n;

    sys_write("init: open /hello.txt\n");
    fd = sys_open("/hello.txt");
    n  = sys_read(fd, buf, 63U);
    buf[n] = '\0';
    sys_write("init: read: ");
    sys_write(buf);
    sys_close(fd);
    sys_write("init: vfs done\n");
    sys_exit(0U);
    for (;;) {}
}
