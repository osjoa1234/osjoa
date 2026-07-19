static void sys_write(const char *s)
{
    __asm__ volatile ("int $0x80" : : "a"(0U), "b"(s));
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("int $0x80" : : "a"(1U), "b"(code));
}

void _start(void)
{
    sys_write("Hello from exec'd hello2!\n");
    sys_exit(0U);
    for (;;) {}
}
