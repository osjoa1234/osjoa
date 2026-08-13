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

static unsigned long sys_brk(unsigned long addr)
{
    unsigned long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(45U), "b"(addr) : "memory");
    return ret;
}

static unsigned int slen(const char *s)
{
    unsigned int n = 0U;
    while (s[n]) n++;
    return n;
}

static void writes(const char *s)
{
    sys_write(1U, s, slen(s));
}

static void write_hex(unsigned long v)
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[16];
    int  i;

    for (i = 15; i >= 0; i--) {
        buf[i] = digits[v & 0xFUL];
        v >>= 4;
    }
    sys_write(1U, buf, 16U);
}

void _start(void)
{
    unsigned long  base;
    unsigned long  grown;
    char          *heap;
    unsigned int   i;

    base = sys_brk(0UL);
    writes("brk: heap start = 0x");
    write_hex(base);
    writes("\n");

    grown = sys_brk(base + 8192UL);
    writes("brk: heap end   = 0x");
    write_hex(grown);
    writes("\n");

    if (grown != base + 8192UL) {
        writes("brk: grow failed\n");
        sys_exit(1U);
        for (;;) {}
    }

    heap = (char *)base;
    for (i = 0U; i < 8192U; i++) heap[i] = (char)('A' + (i % 26U));

    sys_exit(0U);
    for (;;) {}
}
