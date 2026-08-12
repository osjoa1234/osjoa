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

static long sys_arch_prctl(unsigned int code, unsigned long addr)
{
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(172U), "b"(code), "c"(addr) : "memory");
    return ret;
}

static unsigned int sys_getpid(void)
{
    unsigned int pid;
    __asm__ volatile ("int $0x80" : "=a"(pid) : "0"(20U) : "memory");
    return pid;
}

static unsigned int sys_getuid(void)
{
    unsigned int uid;
    __asm__ volatile ("int $0x80" : "=a"(uid) : "0"(24U) : "memory");
    return uid;
}

static unsigned int sys_uname(utsname_t *buf)
{
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(122U), "b"(buf) : "memory");
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
    unsigned int  rc;
    unsigned long fs_readback;
    unsigned long fs_get;
    unsigned int  pid;
    unsigned int  uid;
    utsname_t     uts;

    tls_block[0] = 0x1234567890ABCDEFUL;

    rc = sys_arch_prctl(ARCH_SET_FS, (unsigned long)tls_block);
    writes("tls: arch_prctl(SET_FS) rc = 0x");
    write_hex(rc);
    writes("\n");

    __asm__ volatile ("mov %%fs:0, %0" : "=r"(fs_readback));
    writes("tls: fs:0 = 0x");
    write_hex(fs_readback);
    writes("\n");

    rc = sys_arch_prctl(ARCH_GET_FS, (unsigned long)&fs_get);
    writes("tls: arch_prctl(GET_FS) rc = 0x");
    write_hex(rc);
    writes("\n");
    writes("tls: fs_base readback = 0x");
    write_hex(fs_get);
    writes("\n");

    pid = sys_getpid();
    writes("tls: getpid = 0x");
    write_hex(pid);
    writes("\n");

    uid = sys_getuid();
    writes("tls: getuid = 0x");
    write_hex(uid);
    writes("\n");

    rc = sys_uname(&uts);
    writes("tls: uname rc = 0x");
    write_hex(rc);
    writes("\n");
    writes("tls: uname sysname = ");
    writes(uts.sysname);
    writes("\n");
    writes("tls: uname release = ");
    writes(uts.release);
    writes("\n");

    sys_exit(0U);
    for (;;) {}
}
