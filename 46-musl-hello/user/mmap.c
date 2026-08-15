#define PROT_READ      0x1U
#define PROT_WRITE     0x2U
#define MAP_PRIVATE    0x02U
#define MAP_ANONYMOUS  0x20U

typedef struct {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int  st_mode;
    unsigned int  st_nlink;
    unsigned long st_size;
    unsigned long st_blksize;
} stat_t;

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

static unsigned long sys_mmap2(unsigned long addr, unsigned long length,
                                unsigned int prot, unsigned int flags, int fd)
{
    long ret;
    register unsigned long r10 __asm__("r10") = flags;
    register long          r8  __asm__("r8")  = fd;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(9L), "D"(addr), "S"(length), "d"(prot), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (unsigned long)ret;
}

static unsigned int sys_mprotect(unsigned long addr, unsigned long length, unsigned int prot)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(10L), "D"(addr), "S"(length), "d"(prot) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static unsigned int sys_fstat(unsigned int fd, stat_t *buf)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(5L), "D"(fd), "S"(buf) : "rcx", "r11", "memory");
    return (unsigned int)ret;
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
    unsigned long addr;
    unsigned int  rc;
    stat_t        st;
    char         *region;
    unsigned int  i;

    addr = sys_mmap2(0UL, 8192UL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1);
    writes("mmap: addr = 0x");
    write_hex(addr);
    writes("\n");

    if (addr == (unsigned long)-1) {
        writes("mmap: failed\n");
        sys_exit(1U);
        for (;;) {}
    }

    region = (char *)addr;
    for (i = 0U; i < 8192U; i++) region[i] = (char)('a' + (i % 26U));

    rc = sys_mprotect(addr, 8192UL, PROT_READ);
    writes("mmap: mprotect rc = 0x");
    write_hex(rc);
    writes("\n");

    rc = sys_fstat(1U, &st);
    writes("mmap: fstat rc = 0x");
    write_hex(rc);
    writes("\n");
    writes("mmap: fstat st_mode = 0x");
    write_hex(st.st_mode);
    writes("\n");

    sys_exit(0U);
    for (;;) {}
}
