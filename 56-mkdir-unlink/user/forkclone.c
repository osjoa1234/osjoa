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

static long sys_fork(void)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(57L) : "rcx", "r11", "memory");
    return ret;
}

static long sys_wait4(unsigned int pid, unsigned int *status)
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
    long          wpid;

    tls_block[0] = 0x1234567890ABCDEFUL;
    sys_arch_prctl(ARCH_SET_FS, (unsigned long)tls_block);

    rc = sys_fork();
    writes("forkclone: fork rc = 0x");
    write_hex((unsigned long)rc);
    writes("\n");

    if (rc == 0) {
        __asm__ volatile ("mov %%fs:0, %0" : "=r"(fs_readback));
        writes("forkclone: child fs:0 = 0x");
        write_hex(fs_readback);
        writes("\n");
        sys_exit(42U);
        for (;;) {}
    }

    status = 0xFFFFFFFFU;
    wpid = sys_wait4((unsigned int)-1, &status);
    writes("forkclone: wait4 pid = 0x");
    write_hex((unsigned long)wpid);
    writes(" status = 0x");
    write_hex((unsigned long)status);
    writes(" exitcode = 0x");
    write_hex(((unsigned long)status >> 8) & 0xFFUL);
    writes("\n");

    wpid = sys_wait4((unsigned int)-1, &status);
    writes("forkclone: second wait4 (no children) returned = 0x");
    write_hex((unsigned long)wpid);
    writes("\n");

    sys_exit(0U);
    for (;;) {}
}
