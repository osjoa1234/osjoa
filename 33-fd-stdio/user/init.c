static unsigned int sys_write(unsigned int fd, const char *buf, unsigned int len)
{
    unsigned int n;
    __asm__ volatile ("int $0x80" : "=a"(n) : "0"(0U), "b"(fd), "c"(buf), "d"(len) : "memory");
    return n;
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

static unsigned int sys_fork(void)
{
    unsigned int pid;
    __asm__ volatile ("int $0x80" : "=a"(pid) : "0"(7U));
    return pid;
}

static unsigned int sys_wait(unsigned int pid, unsigned int *code)
{
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(5U), "b"(pid), "c"(code) : "memory");
    return ret;
}

static void sys_close(unsigned int fd)
{
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(10U), "b"(fd) : "memory");
}

static void writes(const char *s)
{
    unsigned int n = 0U;
    while (s[n]) n++;
    sys_write(1U, s, n);
}

void _start(void)
{
    char         buf[64];
    unsigned int pid;
    unsigned int fd;
    unsigned int n;

    writes("init: stdout via fd 1\n");

    pid = sys_fork();
    if (pid == 0U) {
        writes("child: inherited fd 1 works\n");
        sys_exit(0U);
        for (;;) {}
    }
    sys_wait(pid, 0U);
    writes("init: child done\n");

    fd = sys_open("/hello.txt");
    n  = sys_read(fd, buf, 63U);
    sys_write(1U, "init: read: ", 12U);
    sys_write(1U, buf, n);
    sys_close(fd);

    writes("init: type something: ");
    n = sys_read(0U, buf, 63U);
    writes("init: stdin got: ");
    sys_write(1U, buf, n);

    writes("init: fd-stdio done\n");
    sys_exit(0U);
    for (;;) {}
}
