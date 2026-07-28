#include "syscall.h"
#include "process.h"
#include "vfs.h"
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

static u32 sys_open(const char *path)
{
    process_t  *p = (process_t *)thread_current()->user_data;
    vfs_file_t *f;
    u32         i;

    f = vfs_open(path);
    if (!f) return (u32)-1U;

    for (i = 0U; i < PROC_FD_MAX; i++) {
        if (!p->fds[i]) {
            p->fds[i] = f;
            return i;
        }
    }
    vfs_close(f);
    return (u32)-1U;
}

static u32 sys_read(u32 fd, u8 *buf, u32 len)
{
    process_t *p = (process_t *)thread_current()->user_data;

    if (fd >= PROC_FD_MAX || !p->fds[fd]) return 0U;
    return vfs_read(p->fds[fd], buf, len);
}

static void sys_close(u32 fd)
{
    process_t *p = (process_t *)thread_current()->user_data;

    if (fd < PROC_FD_MAX && p->fds[fd]) {
        vfs_close(p->fds[fd]);
        p->fds[fd] = 0;
    }
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
    case SYS_EXEC:
        proc_exec((const char *)frame->ebx);
        frame->eax = (u32)-1U;
        break;
    case SYS_FORK:
        frame->eax = proc_fork(frame->eip, frame->user_esp);
        break;
    case SYS_CLONE:
        frame->eax = proc_clone(frame->eip, frame->ebx);
        break;
    case SYS_THREAD_EXIT:
        proc_thread_exit();
        break;
    case SYS_CLOSE:
        sys_close(frame->ebx);
        frame->eax = 0U;
        break;
    default:
        frame->eax = (u32)-1;
        break;
    }
}
