#define ARCH_SET_FS 0x1002U
#define ARCH_GET_FS 0x1003U

typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} utsname_t;

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

static unsigned int sys_getpid(void)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(39L) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static unsigned int sys_getuid(void)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(102L) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static long sys_uname(utsname_t *buf)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(63L), "D"(buf) : "rcx", "r11", "memory");
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
    unsigned long fs_get;
    unsigned int  pid;
    unsigned int  uid;
    utsname_t     uts;

    tls_block[0] = 0x1234567890ABCDEFUL;

    rc = sys_arch_prctl(ARCH_SET_FS, (unsigned long)tls_block);
    writes("syscall64: arch_prctl(SET_FS) rc = 0x");
    write_hex((unsigned long)rc);
    writes("\n");

    __asm__ volatile ("mov %%fs:0, %0" : "=r"(fs_readback));
    writes("syscall64: fs:0 = 0x");
    write_hex(fs_readback);
    writes("\n");

    rc = sys_arch_prctl(ARCH_GET_FS, (unsigned long)&fs_get);
    writes("syscall64: arch_prctl(GET_FS) rc = 0x");
    write_hex((unsigned long)rc);
    writes("\n");
    writes("syscall64: fs_base readback = 0x");
    write_hex(fs_get);
    writes("\n");

    pid = sys_getpid();
    writes("syscall64: getpid = 0x");
    write_hex(pid);
    writes("\n");

    uid = sys_getuid();
    writes("syscall64: getuid = 0x");
    write_hex(uid);
    writes("\n");

    rc = sys_uname(&uts);
    writes("syscall64: uname rc = 0x");
    write_hex((unsigned long)rc);
    writes("\n");
    writes("syscall64: uname sysname = ");
    writes(uts.sysname);
    writes("\n");
    writes("syscall64: uname release = ");
    writes(uts.release);
    writes("\n");

    sys_exit(0U);
    for (;;) {}
}
