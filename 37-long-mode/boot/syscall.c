#include "syscall.h"
#include "process.h"
#include "vfs.h"

static u32 sys_write(u32 fd, const u8 *buf, u32 len)
{
    process_t *p = (process_t *)thread_current()->user_data;

    if (fd >= PROC_FD_MAX || !p->fds[fd]) return 0U;
    return vfs_write(p->fds[fd], buf, len);
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

static u32 sys_lseek(u32 fd, int offset, u32 whence)
{
    process_t *p = (process_t *)thread_current()->user_data;

    if (fd >= PROC_FD_MAX || !p->fds[fd]) return (u32)-1U;
    return vfs_seek(p->fds[fd], offset, whence);
}

void syscall_dispatch(struct interrupt_frame *frame)
{
    switch (frame->eax) {
    case SYS_WRITE:
        frame->eax = sys_write(frame->ebx, (const u8 *)frame->ecx, frame->edx);
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
    case SYS_WAITPID:
        frame->eax = proc_wait(frame->ebx, (u32 *)frame->ecx);
        break;
    case SYS_EXECVE:
        proc_exec((const char *)frame->ebx);
        frame->eax = (u32)-1U;
        break;
    case SYS_FORK: {
        fork_resume_t ctx;
        ctx.edi      = frame->edi;
        ctx.esi      = frame->esi;
        ctx.ebp      = frame->ebp;
        ctx.ebx      = frame->ebx;
        ctx.edx      = frame->edx;
        ctx.ecx      = frame->ecx;
        ctx.eip      = frame->eip;
        ctx.user_esp = frame->user_esp;
        ctx.eflags   = frame->eflags;
        frame->eax   = proc_fork(&ctx);
        break;
    }
    case SYS_CLONE: {
        fork_resume_t ctx;
        ctx.edi      = frame->edi;
        ctx.esi      = frame->esi;
        ctx.ebp      = frame->ebp;
        ctx.ebx      = frame->ebx;
        ctx.edx      = frame->edx;
        ctx.ecx      = frame->ecx;
        ctx.eip      = frame->eip;
        ctx.user_esp = frame->ebx;
        ctx.eflags   = frame->eflags;
        frame->eax   = proc_clone(&ctx);
        break;
    }
    case SYS_THREAD_EXIT:
        proc_thread_exit();
        break;
    case SYS_CLOSE:
        sys_close(frame->ebx);
        frame->eax = 0U;
        break;
    case SYS_LSEEK:
        frame->eax = sys_lseek(frame->ebx, (int)frame->ecx, frame->edx);
        break;
    default:
        frame->eax = (u32)-1;
        break;
    }
}
