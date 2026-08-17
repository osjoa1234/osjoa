#define ARCH_SET_FS 0x1002U

static unsigned long tls_block[8];

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

static long sys_arch_prctl(unsigned int code, unsigned long addr)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(158L), "D"(code), "S"(addr) : "rcx", "r11", "memory");
    return ret;
}

static long sys_clone_fork(void)
{
    long          ret;
    unsigned long tidbuf;
    register unsigned long r10 __asm__("r10") = (unsigned long)&tidbuf;

    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(56L), "D"(0x1200011UL), "S"(0UL), "d"(0UL), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static long sys_wait4(long pid, unsigned int *status)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(61L), "D"(pid), "S"(status) : "rcx", "r11", "memory");
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
    long          rc;
    unsigned long fs_readback;
    unsigned int  status;
    long          waited;

    tls_block[0] = 0x1234567890ABCDEFUL;
    sys_arch_prctl(ARCH_SET_FS, (unsigned long)tls_block);

    rc = sys_clone_fork();
    if (rc == 0) {
        __asm__ volatile ("mov %%fs:0, %0" : "=r"(fs_readback));
        writes("forkclone: child fs:0 = 0x");
        write_hex(fs_readback);
        writes("\n");
        sys_exit(42U);
        for (;;) {}
    }

    writes("forkclone: parent clone rc = 0x");
    write_hex((unsigned long)rc);
    writes("\n");

    waited = sys_wait4(-1L, &status);
    writes("forkclone: wait4 pid = 0x");
    write_hex((unsigned long)waited);
    writes(" status = 0x");
    write_hex((unsigned long)status);
    writes(" exitcode = 0x");
    write_hex((unsigned long)((status >> 8) & 0xFFU));
    writes("\n");

    waited = sys_wait4(-1L, &status);
    writes("forkclone: second wait4 (no children) returned = 0x");
    write_hex((unsigned long)waited);
    writes("\n");

    sys_exit(0U);
    for (;;) {}
}
