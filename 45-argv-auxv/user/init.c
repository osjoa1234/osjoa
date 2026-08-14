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

static unsigned int sys_read(unsigned int fd, char *buf, unsigned long len)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(0L), "D"(fd), "S"(buf), "d"(len) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static unsigned int sys_fork(void)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(57L) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static unsigned int sys_wait(unsigned int pid, unsigned int *code)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(61L), "D"(pid), "S"(code) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static void sys_exec(const char *name)
{
    __asm__ volatile ("syscall" : : "a"(59L), "D"(name) : "rcx", "r11", "memory");
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

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

void _start(void)
{
    char         buf[64];
    unsigned int n;
    unsigned int pid;
    unsigned int exit_code;

    writes("shell: linux-abi ready\n");
    for (;;) {
        writes("$ ");
        n = sys_read(0U, buf, 63U);
        if (n == 0U) continue;
        buf[n] = '\0';
        if (n > 0U && buf[n - 1U] == '\n') { buf[n - 1U] = '\0'; n--; }
        if (n == 0U) continue;

        if (streq(buf, "exit")) {
            writes("shell: bye\n");
            sys_exit(0U);
            for (;;) {}
        }

        pid = sys_fork();
        if (pid == 0U) {
            sys_exec(buf);
            writes("shell: not found\n");
            sys_exit(1U);
            for (;;) {}
        }
        exit_code = (unsigned int)-1U;
        sys_wait(pid, &exit_code);
    }
}
