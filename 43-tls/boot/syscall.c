#include "syscall.h"
#include "process.h"
#include "vfs.h"
#include "gdt.h"

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

static u32 sys_mprotect(u64 addr, u64 length, u32 prot)
{
    (void)addr;
    (void)length;
    (void)prot;
    return 0U;
}

typedef struct {
    u64 st_dev;
    u64 st_ino;
    u32 st_mode;
    u32 st_nlink;
    u64 st_size;
    u64 st_blksize;
} stat_t;

static u32 sys_fstat(u32 fd, stat_t *buf)
{
    process_t *p = (process_t *)thread_current()->user_data;
    u8        *raw;
    u32        i;

    if (fd >= PROC_FD_MAX || !p->fds[fd]) return (u32)-1U;

    raw = (u8 *)buf;
    for (i = 0U; i < sizeof(stat_t); i++) raw[i] = 0U;

    buf->st_mode    = 0020000U | 0666U;
    buf->st_nlink   = 1U;
    buf->st_blksize = 512U;

    return 0U;
}

#define ARCH_SET_GS 0x1001U
#define ARCH_SET_FS 0x1002U
#define ARCH_GET_FS 0x1003U
#define ARCH_GET_GS 0x1004U

static u64 sys_arch_prctl(u32 code, u64 addr)
{
    thread_t *t = thread_current();

    switch (code) {
    case ARCH_SET_FS:
        t->fs_base = addr;
        gdt_set_fs_base(addr);
        return 0U;
    case ARCH_GET_FS:
        *(u64 *)addr = t->fs_base;
        return 0U;
    default:
        return (u64)-1;
    }
}

static u32 sys_getpid(void)
{
    process_t *p = (process_t *)thread_current()->user_data;
    return p->pid;
}

static u32 sys_getuid(void)
{
    return 0U;
}

typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} utsname_t;

static void uts_set(char *dst, const char *src)
{
    u32 i = 0U;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static u32 sys_uname(utsname_t *buf)
{
    u8 *raw = (u8 *)buf;
    u32 i;

    for (i = 0U; i < sizeof(utsname_t); i++) raw[i] = 0U;

    uts_set(buf->sysname,  "custom-os");
    uts_set(buf->nodename, "custom-os");
    uts_set(buf->release,  "0.43.0");
    uts_set(buf->version,  "#43");
    uts_set(buf->machine,  "x86_64");

    return 0U;
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
        ctx.r8       = frame->r8;
        ctx.r9       = frame->r9;
        ctx.r10      = frame->r10;
        ctx.r11      = frame->r11;
        ctx.r12      = frame->r12;
        ctx.r13      = frame->r13;
        ctx.r14      = frame->r14;
        ctx.r15      = frame->r15;
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
        ctx.r8       = frame->r8;
        ctx.r9       = frame->r9;
        ctx.r10      = frame->r10;
        ctx.r11      = frame->r11;
        ctx.r12      = frame->r12;
        ctx.r13      = frame->r13;
        ctx.r14      = frame->r14;
        ctx.r15      = frame->r15;
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
    case SYS_BRK:
        frame->rax = proc_brk(frame->rbx);
        break;
    case SYS_FSTAT:
        frame->rax = sys_fstat((u32)frame->rbx, (stat_t *)frame->rcx);
        break;
    case SYS_MPROTECT:
        frame->rax = sys_mprotect(frame->rbx, frame->rcx, (u32)frame->rdx);
        break;
    case SYS_MMAP2:
        frame->rax = proc_mmap(frame->rcx, (u32)frame->rdx, (u32)frame->rsi);
        break;
    case SYS_ARCH_PRCTL:
        frame->rax = sys_arch_prctl((u32)frame->rbx, frame->rcx);
        break;
    case SYS_GETPID:
        frame->rax = sys_getpid();
        break;
    case SYS_GETUID:
        frame->rax = sys_getuid();
        break;
    case SYS_UNAME:
        frame->rax = sys_uname((utsname_t *)frame->rbx);
        break;
    default:
        frame->rax = (u64)-1;
        break;
    }
}
