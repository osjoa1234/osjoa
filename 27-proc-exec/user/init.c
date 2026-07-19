static void sys_write(const char *s)
{
    __asm__ volatile ("int $0x80" : : "a"(0U), "b"(s));
}

static void sys_exec(const char *name)
{
    __asm__ volatile ("int $0x80" : : "a"(6U), "b"(name));
}

void _start(void)
{
    sys_write("init: before exec\n");
    sys_exec("hello2");
    for (;;) {}
}
