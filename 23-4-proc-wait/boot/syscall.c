#include "syscall.h"
#include "process.h"
#include "initrd.h"
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

static u32 sys_open(const char *name)
{
    int fd = initrd_open(name);
    return (fd < 0) ? (u32)-1 : (u32)fd;
}

static u32 sys_read(u32 fd, u8 *buf, u32 len)
{
    return initrd_read((int)fd, buf, len);
}

void syscall_dispatch(struct interrupt_frame *frame)
{
    switch (frame->eax) {
    case SYS_WRITE:
        frame->eax = sys_write((const char *)frame->ebx);
        break;
    case SYS_EXIT:
        proc_exit(frame->ebx);
        break;
    case SYS_OPEN:
        frame->eax = sys_open((const char *)frame->ebx);
        break;
    case SYS_READ:
        frame->eax = sys_read(frame->ebx, (u8 *)frame->ecx, frame->edx);
        break;
    case SYS_SPAWN:
        frame->eax = proc_spawn((const char *)frame->ebx);
        break;
    case SYS_WAIT:
        frame->eax = proc_wait(frame->ebx, (u32 *)frame->ecx);
        break;
    default:
        frame->eax = (u32)-1;
        break;
    }
}
