static void sys_write(const char *s)
{
    __asm__ volatile ("int $0x80" : : "a"(0U), "b"(s));
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("int $0x80" : : "a"(1U), "b"(code));
}

static void sys_thread_exit(void)
{
    __asm__ volatile ("int $0x80" : : "a"(9U));
}

static volatile unsigned int done_count = 0;

static void worker(unsigned int n)
{
    if (n == 1U)
        sys_write("thread 1: hello from clone\n");
    else
        sys_write("thread 2: hello from clone\n");
    done_count++;
    sys_thread_exit();
    for (;;) {}
}

void _start(void)
{
    unsigned int tid;

    sys_write("init: before clone\n");

    __asm__ volatile ("int $0x80" : "=a"(tid) : "0"(8U));
    if (tid == 0) { worker(1U); for (;;) {} }

    __asm__ volatile ("int $0x80" : "=a"(tid) : "0"(8U));
    if (tid == 0) { worker(2U); for (;;) {} }

    sys_write("init: clones spawned\n");
    while (done_count < 2U) {}
    sys_write("init: both clones done\n");
    sys_exit(0U);
    for (;;) {}
}
