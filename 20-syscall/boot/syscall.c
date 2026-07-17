#include "syscall.h"
#include "console.h"

static u32 sys_write(const char *s)
{
    u32 n = 0U;

    while (s[n]) {
        n++;
    }

    console_printf("%s", s);
    return n;
}

static void sys_exit(u32 code)
{
    console_set_color(0x0AU);
    console_printf("user task exited: code=%u\n", code);

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void syscall_dispatch(struct interrupt_frame *frame)
{
    switch (frame->eax) {
    case SYS_WRITE:
        frame->eax = sys_write((const char *)frame->ebx);
        break;
    case SYS_EXIT:
        sys_exit(frame->ebx);
        break;
    default:
        frame->eax = (u32)-1;
        break;
    }
}
