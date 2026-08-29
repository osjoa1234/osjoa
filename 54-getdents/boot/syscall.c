#include "syscall.h"
#include "process.h"
#include "vfs.h"
#include "pipe.h"
#include "gdt.h"
#include "paging.h"
#include "signal.h"

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

static u32 sys_access(const char *path)
{
    vfs_file_t *f = vfs_open(path);

    if (!f) return (u32)-1U;
    vfs_close(f);
    return 0U;
}

static u32 sys_chdir(const char *path)
{
    (void)path;
    return 0U;
}

static u32 sys_getcwd(char *buf, u32 size)
{
    if (size < 2U) return (u32)-1U;
    buf[0] = '/';
    buf[1] = '\0';
    return 2U;
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

static u32 sys_getdents64(u32 fd, u8 *buf, u32 count)
{
    process_t *p = (process_t *)thread_current()->user_data;

    if (fd >= PROC_FD_MAX || !p->fds[fd]) return (u32)-1U;
    return vfs_getdents(p->fds[fd], buf, count);
}

static u32 sys_pipe(int *fds)
{
    process_t  *p = (process_t *)thread_current()->user_data;
    vfs_file_t *rf;
    vfs_file_t *wf;
    u32         rfd = (u32)-1U;
    u32         wfd = (u32)-1U;
    u32         i;

    if (pipe_create(&rf, &wf) != 0) return (u32)-1U;

    for (i = 0U; i < PROC_FD_MAX; i++) {
        if (!p->fds[i]) { p->fds[i] = rf; rfd = i; break; }
    }
    for (i = 0U; i < PROC_FD_MAX; i++) {
        if (!p->fds[i]) { p->fds[i] = wf; wfd = i; break; }
    }

    if (rfd == (u32)-1U || wfd == (u32)-1U) {
        if (rfd != (u32)-1U) { vfs_close(p->fds[rfd]); p->fds[rfd] = 0; }
        if (wfd != (u32)-1U) { vfs_close(p->fds[wfd]); p->fds[wfd] = 0; }
        return (u32)-1U;
    }

    fds[0] = (int)rfd;
    fds[1] = (int)wfd;
    return 0U;
}

static u32 sys_dup2(u32 oldfd, u32 newfd)
{
    process_t *p = (process_t *)thread_current()->user_data;

    if (oldfd >= PROC_FD_MAX || !p->fds[oldfd]) return (u32)-1U;
    if (newfd >= PROC_FD_MAX) return (u32)-1U;
    if (oldfd == newfd) return newfd;

    if (p->fds[newfd]) { vfs_close(p->fds[newfd]); p->fds[newfd] = 0; }
    p->fds[newfd] = vfs_dup(p->fds[oldfd]);
    return newfd;
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
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
    u32 __pad0;
    u64 st_rdev;
    u64 st_size;
    u64 st_blksize;
    u64 st_blocks;
    u64 st_atime;
    u64 st_atime_nsec;
    u64 st_mtime;
    u64 st_mtime_nsec;
    u64 st_ctime;
    u64 st_ctime_nsec;
    u64 __unused[3];
} stat_t;

static u32 sys_fstat(u32 fd, stat_t *buf)
{
    process_t *p = (process_t *)thread_current()->user_data;
    u8        *raw;
    u32        i;

    if (fd >= PROC_FD_MAX || !p->fds[fd]) return (u32)-1U;

    raw = (u8 *)buf;
    for (i = 0U; i < sizeof(stat_t); i++) raw[i] = 0U;

    buf->st_mode    = vfs_mode(p->fds[fd]);
    buf->st_nlink   = 1U;
    buf->st_size    = vfs_size(p->fds[fd]);
    buf->st_blksize = 512U;

    return 0U;
}

static u32 sys_stat(const char *path, stat_t *buf)
{
    vfs_file_t *f;
    u8         *raw;
    u32         i;

    f = vfs_open(path);
    if (!f) return (u32)-1U;

    raw = (u8 *)buf;
    for (i = 0U; i < sizeof(stat_t); i++) raw[i] = 0U;

    buf->st_mode    = vfs_mode(f);
    buf->st_nlink   = 1U;
    buf->st_size    = vfs_size(f);
    buf->st_blksize = 512U;

    vfs_close(f);
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

typedef struct {
    const u8 *iov_base;
    u64       iov_len;
} iovec_t;

static u64 sys_writev(u32 fd, const iovec_t *iov, u32 iovcnt)
{
    u64 total = 0U;
    u32 i;

    for (i = 0U; i < iovcnt; i++)
        total += sys_write(fd, iov[i].iov_base, (u32)iov[i].iov_len);

    return total;
}

static u64 sys_ioctl(void)
{
    return (u64)-25LL;
}

static u32 sys_set_tid_address(u64 addr)
{
    process_t *p = (process_t *)thread_current()->user_data;
    (void)addr;
    return p->pid;
}

static u32 sys_munmap(u64 addr, u64 len)
{
    process_t *p = (process_t *)thread_current()->user_data;
    u64 start    = addr & ~0xFFFULL;
    u64 end      = (addr + len + 0xFFFULL) & ~0xFFFULL;
    u64 va;

    for (va = start; va < end; va += 0x1000ULL)
        paging_unmap_user_page(p->pml4_phys, va);

    return 0U;
}

void syscall_dispatch(struct interrupt_frame *frame)
{
    switch ((u32)frame->rax) {
    case SYS_WRITE:
        frame->rax = sys_write((u32)frame->rdi, (const u8 *)frame->rsi, (u32)frame->rdx);
        break;
    case SYS_READ:
        frame->rax = sys_read((u32)frame->rdi, (u8 *)frame->rsi, (u32)frame->rdx);
        break;
    case SYS_OPEN:
        frame->rax = sys_open((const char *)frame->rdi);
        break;
    case SYS_CLOSE:
        sys_close((u32)frame->rdi);
        frame->rax = 0U;
        break;
    case SYS_STAT:
    case SYS_LSTAT:
        frame->rax = sys_stat((const char *)frame->rdi, (stat_t *)frame->rsi);
        break;
    case SYS_FSTAT:
        frame->rax = sys_fstat((u32)frame->rdi, (stat_t *)frame->rsi);
        break;
    case SYS_LSEEK:
        frame->rax = sys_lseek((u32)frame->rdi, (int)frame->rsi, (u32)frame->rdx);
        break;
    case SYS_GETDENTS64:
        frame->rax = sys_getdents64((u32)frame->rdi, (u8 *)frame->rsi, (u32)frame->rdx);
        break;
    case SYS_ACCESS:
        frame->rax = sys_access((const char *)frame->rdi);
        break;
    case SYS_RT_SIGACTION:
        frame->rax = sys_rt_sigaction((u32)frame->rdi, (const sigaction_t *)frame->rsi,
                                       (sigaction_t *)frame->rdx, frame->r10);
        break;
    case SYS_RT_SIGPROCMASK:
        frame->rax = sys_rt_sigprocmask((u32)frame->rdi, (const u64 *)frame->rsi,
                                         (u64 *)frame->rdx, frame->r10);
        break;
    case SYS_RT_SIGRETURN:
        sys_rt_sigreturn(frame);
        break;
    case SYS_MMAP:
        frame->rax = proc_mmap(frame->rsi, (u32)frame->rdx, (u32)frame->r10);
        break;
    case SYS_MPROTECT:
        frame->rax = sys_mprotect(frame->rdi, frame->rsi, (u32)frame->rdx);
        break;
    case SYS_MUNMAP:
        frame->rax = sys_munmap(frame->rdi, frame->rsi);
        break;
    case SYS_BRK:
        frame->rax = proc_brk(frame->rdi);
        break;
    case SYS_IOCTL:
        frame->rax = sys_ioctl();
        break;
    case SYS_WRITEV:
        frame->rax = sys_writev((u32)frame->rdi, (const iovec_t *)frame->rsi, (u32)frame->rdx);
        break;
    case SYS_PIPE:
        frame->rax = sys_pipe((int *)frame->rdi);
        break;
    case SYS_DUP2:
        frame->rax = sys_dup2((u32)frame->rdi, (u32)frame->rsi);
        break;
    case SYS_GETPID:
        frame->rax = sys_getpid();
        break;
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
        ctx.user_rsp = frame->rdi;
        ctx.rflags   = frame->rflags;
        frame->rax   = proc_clone(&ctx);
        break;
    }
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
    case SYS_EXECVE:
        proc_exec((const char *)frame->rdi, (char *const *)frame->rsi);
        frame->rax = (u64)-1;
        break;
    case SYS_EXIT:
        proc_thread_exit();
        break;
    case SYS_WAIT4:
        frame->rax = proc_wait((u32)frame->rdi, (u32 *)frame->rsi);
        break;
    case SYS_UNAME:
        frame->rax = sys_uname((utsname_t *)frame->rdi);
        break;
    case SYS_GETUID:
        frame->rax = sys_getuid();
        break;
    case SYS_GETCWD:
        frame->rax = sys_getcwd((char *)frame->rdi, (u32)frame->rsi);
        break;
    case SYS_CHDIR:
        frame->rax = sys_chdir((const char *)frame->rdi);
        break;
    case SYS_ARCH_PRCTL:
        frame->rax = sys_arch_prctl((u32)frame->rdi, frame->rsi);
        break;
    case SYS_SET_TID_ADDRESS:
        frame->rax = sys_set_tid_address(frame->rdi);
        break;
    case SYS_EXIT_GROUP:
        proc_exit((u32)frame->rdi);
        break;
    default:
        console_printf("syscall: unimplemented rax=%u\n", (u32)frame->rax);
        frame->rax = (u64)-1;
        break;
    }

    signal_deliver_pending(frame);
}
