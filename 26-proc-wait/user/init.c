static void sys_write(const char *s)
{
    __asm__ volatile ("int $0x80" : : "a"(0U), "b"(s));
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("int $0x80" : : "a"(1U), "b"(code));
}

static unsigned int sys_spawn(const char *name)
{
    unsigned int pid;
    __asm__ volatile ("int $0x80" : "=a"(pid) : "0"(4U), "b"(name));
    return pid;
}

static int sys_wait(unsigned int pid, unsigned int *exit_code)
{
    int rc;
    __asm__ volatile ("int $0x80" : "=a"(rc) : "0"(5U), "b"(pid), "c"(exit_code));
    return rc;
}

void _start(void)
{
    unsigned int pid;
    unsigned int code;

    sys_write("init: spawning hello\n");
    pid = sys_spawn("hello");
    sys_write("init: waiting for hello\n");
    sys_wait(pid, &code);
    sys_write("init: hello done\n");
    sys_exit(0U);
    for (;;) {}
}
