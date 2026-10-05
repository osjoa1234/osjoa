static unsigned int sys_write(unsigned int fd, const char *buf, unsigned long len)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(1L), "D"(fd), "S"(buf), "d"(len) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("syscall" : : "a"(231L), "D"(code) : "rcx", "r11", "memory");
}

static unsigned long sys_brk(unsigned long addr)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(12L), "D"(addr) : "rcx", "r11", "memory");
    return (unsigned long)ret;
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
