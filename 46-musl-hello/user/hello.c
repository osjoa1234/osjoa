static unsigned int sys_write(unsigned int fd, const char *buf, unsigned int len)
{
    unsigned int n;
    __asm__ volatile ("int $0x80" : "=a"(n) : "0"(4U), "b"(fd), "c"(buf), "d"(len) : "memory");
    return n;
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("int $0x80" : : "a"(1U), "b"(code));
}

static unsigned int slen(const char *s)
{
    unsigned int n = 0U;
    while (s[n]) n++;
    return n;
}

void _start(void)
{
    const char *msg = "hello: Hello from hello!\n";
    sys_write(1U, msg, slen(msg));
    sys_exit(0U);
    for (;;) {}
}
