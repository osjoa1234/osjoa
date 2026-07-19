static void sys_write(const char *s)
{
    __asm__ volatile ("int $0x80" : : "a"(0U), "b"(s));
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("int $0x80" : : "a"(1U), "b"(code));
}

static unsigned int sys_fork(void)
{
    unsigned int pid;
    __asm__ volatile ("int $0x80" : "=a"(pid) : "0"(7U));
    return pid;
}

static int sys_wait(unsigned int pid, unsigned int *exit_code)
{
    int rc;
    __asm__ volatile ("int $0x80" : "=a"(rc) : "0"(5U), "b"(pid), "c"(exit_code));
    return rc;
}

static void sys_exec(const char *name)
{
    __asm__ volatile ("int $0x80" : : "a"(6U), "b"(name));
}

void _start(void)
{
    unsigned int pid = sys_fork();
    unsigned int code;

    if (pid == 0U) {
        sys_exec("hello");
        for (;;) {}
    }

    sys_write("init: waiting for child\n");
    sys_wait(pid, &code);
    sys_write("init: child done\n");
    sys_exit(0U);
    for (;;) {}
}
