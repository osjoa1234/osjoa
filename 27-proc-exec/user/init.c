static void sys_write(const char *s)
{
    __asm__ volatile ("int $0x80" : : "a"(0U), "b"(s));
}

static int sys_exec(const char *name)
{
    int rc;
    __asm__ volatile ("int $0x80" : "=a"(rc) : "0"(6U), "b"(name));
    return rc;
}

void _start(void)
{
    sys_write("init: before exec\n");
    sys_exec("hello2");
    sys_write("init: exec failed\n");
    for (;;) {}
}
