static void sys_write(const char *s)
{
    __asm__ volatile ("int $0x80" : : "a"(0U), "b"(s));
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("int $0x80" : : "a"(1U), "b"(code));
}

static int sys_fork(void)
{
    int pid;
    __asm__ volatile ("int $0x80" : "=a"(pid) : "0"(7U));
    return pid;
}

static void sys_exec(const char *name)
{
    __asm__ volatile ("int $0x80" : : "a"(6U), "b"(name));
}

static int sys_wait(int pid)
{
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(5U), "b"((unsigned int)pid), "c"(0U));
    return ret;
}

void _start(void)
{
    int pid;

    sys_write("init: before fork\n");

    pid = sys_fork();
    if (pid == 0) {
        sys_write("child1: exec hello\n");
        sys_exec("hello");
        for (;;) {}
    }

    pid = sys_fork();
    if (pid == 0) {
        sys_write("child2: exec hello2\n");
        sys_exec("hello2");
        for (;;) {}
    }

    sys_wait(-1);
    sys_wait(-1);
    sys_write("init: both children done\n");
    sys_exit(0U);
    for (;;) {}
}
