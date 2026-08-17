#define SIGINT 2

typedef struct {
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
} sigaction_t;

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

static long sys_rt_sigaction(int signum, const sigaction_t *act, sigaction_t *oldact, unsigned long sigsetsize)
{
    long ret;
    register unsigned long r10 __asm__("r10") = sigsetsize;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(13L), "D"(signum), "S"(act), "d"(oldact), "r"(r10) : "rcx", "r11", "memory");
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

static volatile unsigned int sigint_count;

static void sigint_handler(int signum)
{
    (void)signum;
    sigint_count++;
    writes("signal: SIGINT caught\n");
}

__asm__(
    ".global _start\n"
    "_start:\n"
    "    mov %rsp, %rdi\n"
    "    and $-16, %rsp\n"
    "    call start_main\n"
);

void start_main(unsigned long *stack)
{
    sigaction_t   act;
    unsigned long i;

    (void)stack;

    writes("signal: installing SIGINT handler\n");

    act.handler  = (unsigned long)sigint_handler;
    act.flags    = 0UL;
    act.restorer = 0UL;
    act.mask     = 0UL;
    sys_rt_sigaction(SIGINT, &act, 0, 8UL);

    writes("signal: looping -- press Ctrl+C three times\n");

    while (sigint_count < 3U) {
        for (i = 0UL; i < 50000000UL; i++) {
            __asm__ volatile ("" ::: "memory");
        }
    }

    writes("signal: caught 3 SIGINT, exiting\n");
    sys_exit(0U);
    for (;;) {}
}
