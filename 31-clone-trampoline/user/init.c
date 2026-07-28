#define STACK_SIZE 4096

static char stacks[2][STACK_SIZE];
static unsigned int stack_idx = 0;

static void sys_write(const char *s)
{
    __asm__ volatile ("int $0x80" : : "a"(0U), "b"(s));
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("int $0x80" : : "a"(1U), "b"(code));
}

extern unsigned int clone_trampoline(unsigned int *sp);

static unsigned int thread_create(void (*fn)(unsigned int), unsigned int arg)
{
    unsigned int *sp = (unsigned int *)(stacks[stack_idx++] + STACK_SIZE);
    *(--sp) = arg;
    *(--sp) = (unsigned int)fn;
    return clone_trampoline(sp);
}

static volatile unsigned int done_count = 0;

static void worker(unsigned int n)
{
    if (n == 1U)
        sys_write("thread 1: hello from clone\n");
    else
        sys_write("thread 2: hello from clone\n");
    done_count++;
}

void _start(void)
{
    sys_write("init: before clone\n");
    thread_create(worker, 1U);
    thread_create(worker, 2U);
    sys_write("init: clones spawned\n");
    while (done_count < 2U) {}
    sys_write("init: both clones done\n");
    sys_exit(0U);
    for (;;) {}
}
