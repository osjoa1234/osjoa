static unsigned int sys_write(unsigned int fd, const char *buf, unsigned int len)
{
    unsigned int n;
    __asm__ volatile ("int $0x80" : "=a"(n) : "0"(0U), "b"(fd), "c"(buf), "d"(len) : "memory");
    return n;
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("int $0x80" : : "a"(1U), "b"(code));
}

static unsigned int sys_read(unsigned int fd, char *buf, unsigned int len)
{
    unsigned int n;
    __asm__ volatile ("int $0x80" : "=a"(n) : "a"(3U), "b"(fd), "c"(buf), "d"(len) : "memory");
    return n;
}

static unsigned int sys_fork(void)
{
    unsigned int pid;
    __asm__ volatile ("int $0x80" : "=a"(pid) : "0"(7U));
    return pid;
}

static unsigned int sys_wait(unsigned int pid, unsigned int *code)
{
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(5U), "b"(pid), "c"(code) : "memory");
    return ret;
}

static void sys_exec(const char *name)
{
    __asm__ volatile ("int $0x80" : : "a"(6U), "b"(name));
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

    writes("shell: user-shell ready\n");
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
