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
    switch ((u32)frame->rax) {
    case SYS_WRITE:
        frame->rax = sys_write((u32)frame->rbx, (const u8 *)frame->rcx, (u32)frame->rdx);
        break;
    case SYS_EXIT:
        proc_exit((u32)frame->rbx);
        break;
    case SYS_OPEN:
        frame->rax = sys_open((const char *)frame->rbx);
        break;
    case SYS_READ:
        frame->rax = sys_read((u32)frame->rbx, (u8 *)frame->rcx, (u32)frame->rdx);
        break;
    case SYS_SPAWN:
        frame->rax = proc_spawn((const char *)frame->rbx);
        break;
    case SYS_WAITPID:
        frame->rax = proc_wait((u32)frame->rbx, (u32 *)frame->rcx);
        break;
    case SYS_EXECVE:
        proc_exec((const char *)frame->rbx);
        frame->rax = (u64)-1;
        break;
    case SYS_FORK: {
        fork_resume_t ctx;
        ctx.rdi      = frame->rdi;
        ctx.rsi      = frame->rsi;
        ctx.rbp      = frame->rbp;
        ctx.rbx      = frame->rbx;
        ctx.rdx      = frame->rdx;
        ctx.rcx      = frame->rcx;
        ctx.rip      = frame->rip;
        ctx.user_rsp = frame->user_rsp;
        ctx.rflags   = frame->rflags;
        frame->rax   = proc_fork(&ctx);
        break;
    }
    case SYS_CLONE: {
        fork_resume_t ctx;
        ctx.rdi      = frame->rdi;
        ctx.rsi      = frame->rsi;
        ctx.rbp      = frame->rbp;
        ctx.rbx      = frame->rbx;
        ctx.rdx      = frame->rdx;
        ctx.rcx      = frame->rcx;
        ctx.rip      = frame->rip;
        ctx.user_rsp = frame->rbx;
        ctx.rflags   = frame->rflags;
        frame->rax   = proc_clone(&ctx);
        break;
    }
    case SYS_THREAD_EXIT:
        proc_thread_exit();
        break;
    case SYS_CLOSE:
        sys_close((u32)frame->rbx);
        frame->rax = 0U;
        break;
    case SYS_LSEEK:
        frame->rax = sys_lseek((u32)frame->rbx, (int)frame->rcx, (u32)frame->rdx);
        break;
    default:
        frame->rax = (u64)-1;
        break;
    }
}
