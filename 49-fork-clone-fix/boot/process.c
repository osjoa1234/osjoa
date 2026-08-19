#include "process.h"
#include "console_dev.h"
#include "elf.h"
#include "initrd.h"
#include "paging.h"
#include "phys_mem.h"
#include "kheap.h"
#include "console.h"
#include "signal.h"

static process_t    proc_table[PROC_MAX];
static wait_queue_t kernel_wait_chldexit;

extern void enter_user_mode(u64 rip, u64 user_rsp);
extern void enter_user_mode_fork(const fork_resume_t *ctx);

void proc_init(void)
{
    u32 i;
    for (i = 0U; i < PROC_MAX; i++) proc_table[i].state = PROC_FREE;
    wq_init(&kernel_wait_chldexit);
}

static process_t *proc_alloc(void)
{
    u32 i;
    u32 j;

    for (i = 0U; i < PROC_MAX; i++) {
        if (proc_table[i].state == PROC_FREE) {
            proc_table[i].state       = PROC_RUNNING;
            proc_table[i].pid         = i;
            proc_table[i].parent_pid  = PROC_NO_PARENT;
            proc_table[i].threads     = 0;
            wq_init(&proc_table[i].wait_chldexit);
            for (j = 0U; j < PROC_FD_MAX; j++) proc_table[i].fds[j] = 0;
            signal_reset_handlers(&proc_table[i]);
            proc_table[i].sig_pending = 0ULL;
            proc_table[i].sig_blocked = 0ULL;
            return &proc_table[i];
        }
    }
    return 0;
}

static process_t *find_zombie_child(u32 parent_pid)
{
    u32 i;

    for (i = 0U; i < PROC_MAX; i++) {
        if (proc_table[i].state == PROC_ZOMBIE &&
            proc_table[i].parent_pid == parent_pid)
            return &proc_table[i];
    }
    return 0;
}

static void proc_run_trampoline(void)
{
    process_t *p = (process_t *)thread_current()->user_data;
    enter_user_mode(p->entry, p->user_rsp);
}

static void fork_child_trampoline(void)
{
    process_t *p = (process_t *)thread_current()->user_data;
    enter_user_mode_fork(&p->fork_ctx);
}

typedef struct {
    process_t    *proc;
    fork_resume_t resume;
} clone_fork_ctx_t;

static void clone_fork_trampoline(void)
{
    clone_fork_ctx_t *ctx  = (clone_fork_ctx_t *)thread_current()->user_data;
    process_t        *proc = ctx->proc;
    fork_resume_t     r    = ctx->resume;
    thread_current()->user_data = proc;
    kfree(ctx);
    enter_user_mode_fork(&r);
}

u32 proc_spawn(const char *name)
{
    process_t *p;
    int        bin_fd;
    u32        pml4_phys;
    u64        entry;
    u64        brk_start;
    u64        phdr;
    u32        phnum;
    u32        phentsize;
    thread_t  *t;
    char      *argv[2];

    p = proc_alloc();
    if (!p) return (u32)-1U;

    bin_fd = initrd_open(name);
    if (bin_fd < 0) { p->state = PROC_FREE; return (u32)-1U; }

    pml4_phys = paging_clone_dir();
    entry     = elf_load_process(initrd_data(bin_fd), initrd_size(bin_fd), pml4_phys,
                                  &brk_start, &phdr, &phnum, &phentsize);
    if (!entry) { p->state = PROC_FREE; return (u32)-1U; }

    argv[0] = (char *)name;
    argv[1] = 0;

    p->entry      = entry;
    p->user_rsp   = elf_setup_stack(pml4_phys, argv, 1U, entry, phdr, phnum, phentsize);
    p->pml4_phys  = pml4_phys;
    p->heap_start = brk_start;
    p->heap_end   = brk_start;
    p->mmap_next  = PROC_MMAP_TOP;
    p->parent_pid = PROC_NO_PARENT;

    signal_setup_trampoline(pml4_phys);

    p->fds[0] = console_dev_open();
    p->fds[1] = console_dev_open();
    p->fds[2] = console_dev_open();

    t = thread_create_with_data(proc_run_trampoline, p, pml4_phys, 0ULL);
    if (!t) { p->state = PROC_FREE; return (u32)-1U; }

    t->proc_next = 0;
    p->threads   = t;

    return p->pid;
}

u32 proc_fork(const fork_resume_t *ctx)
{
    process_t *parent = (process_t *)thread_current()->user_data;
    process_t *child;
    u32        child_pml4_phys;
    u32        j;
    thread_t  *t;

    child = proc_alloc();
    if (!child) return (u32)-1U;

    for (j = 0U; j < PROC_FD_MAX; j++) {
        if (parent->fds[j]) child->fds[j] = vfs_dup(parent->fds[j]);
    }

    child_pml4_phys = paging_clone_dir();
    paging_copy_user_pages(parent->pml4_phys, child_pml4_phys);

    child->fork_ctx   = *ctx;
    child->entry      = ctx->rip;
    child->pml4_phys  = child_pml4_phys;
    child->heap_start = parent->heap_start;
    child->heap_end   = parent->heap_end;
    child->mmap_next  = parent->mmap_next;
    child->parent_pid = parent->pid;
    child->sig_blocked = parent->sig_blocked;
    for (j = 0U; j < NSIG; j++) child->sig_handler[j] = parent->sig_handler[j];

    t = thread_create_with_data(fork_child_trampoline, child,
                                 child_pml4_phys, thread_current()->fs_base);
    if (!t) { child->state = PROC_FREE; return (u32)-1U; }

    t->proc_next  = 0;
    child->threads = t;

    return child->pid;
}

void proc_exec(const char *name, char *const argv[])
{
    process_t *p = (process_t *)thread_current()->user_data;
    int        fd;
    u64        entry;
    u64        brk_start;
    u64        phdr;
    u32        phnum;
    u32        phentsize;
    u64        user_rsp;
    u32        j, k;
    char       argv_buf[PROC_EXEC_ARGMAX][PROC_EXEC_ARGLEN];
    char      *argv_copy[PROC_EXEC_ARGMAX + 1U];
    u32        argc;

    for (j = 3U; j < PROC_FD_MAX; j++) {
        if (p->fds[j]) { vfs_close(p->fds[j]); p->fds[j] = 0; }
    }

    fd = initrd_open(name);
    if (fd < 0) return;

    argc = 0U;
    while (argv && argv[argc] && argc < PROC_EXEC_ARGMAX) {
        const char *src = argv[argc];
        for (k = 0U; k < PROC_EXEC_ARGLEN - 1U && src[k]; k++) argv_buf[argc][k] = src[k];
        argv_buf[argc][k] = '\0';
        argv_copy[argc] = argv_buf[argc];
        argc++;
    }
    if (argc == 0U) {
        for (k = 0U; k < PROC_EXEC_ARGLEN - 1U && name[k]; k++) argv_buf[0][k] = name[k];
        argv_buf[0][k] = '\0';
        argv_copy[0] = argv_buf[0];
        argc = 1U;
    }
    argv_copy[argc] = 0;

    paging_free_user_pages(p->pml4_phys);

    entry = elf_load_process(initrd_data(fd), initrd_size(fd), p->pml4_phys,
                              &brk_start, &phdr, &phnum, &phentsize);
    if (!entry) return;

    signal_setup_trampoline(p->pml4_phys);
    signal_reset_handlers(p);

    user_rsp = elf_setup_stack(p->pml4_phys, argv_copy, argc, entry, phdr, phnum, phentsize);

    p->entry      = entry;
    p->user_rsp   = user_rsp;
    p->heap_start = brk_start;
    p->heap_end   = brk_start;
    p->mmap_next  = PROC_MMAP_TOP;
    enter_user_mode(entry, user_rsp);
    for (;;) { __asm__ volatile ("hlt"); }
}

void proc_exit(u32 code)
{
    process_t *p      = (process_t *)thread_current()->user_data;
    process_t *parent;
    thread_t  *t;
    u32        j;

    for (j = 0U; j < PROC_FD_MAX; j++) {
        if (p->fds[j]) { vfs_close(p->fds[j]); p->fds[j] = 0; }
    }

    console_set_color(0x0AU);
    console_printf("process %u exited: code=%u\n", p->pid, code);

    p->state     = PROC_ZOMBIE;
    p->exit_code = code;

    for (t = p->threads; t; t = t->proc_next) {
        if (t != thread_current())
            t->state = THREAD_DEAD;
    }

    if (p->parent_pid != PROC_NO_PARENT) {
        parent = proc_get(p->parent_pid);
        if (parent)
            wq_wake_all(&parent->wait_chldexit);
    } else {
        wq_wake_all(&kernel_wait_chldexit);
    }

    thread_exit();
    for (;;) { __asm__ volatile ("hlt"); }
}

void proc_thread_exit(void)
{
    process_t *p = (process_t *)thread_current()->user_data;
    thread_t  *t;
    u32        alive = 0U;

    for (t = p->threads; t; t = t->proc_next) {
        if (t != thread_current() && t->state != THREAD_DEAD)
            alive++;
    }

    if (alive == 0U) {
        proc_exit(0U);
        return;
    }

    thread_exit();
    for (;;) { __asm__ volatile ("hlt"); }
}

u32 proc_clone(const fork_resume_t *ctx)
{
    process_t        *p    = (process_t *)thread_current()->user_data;
    clone_fork_ctx_t *cctx;
    thread_t         *t;
    thread_t         *last;

    cctx = (clone_fork_ctx_t *)kmalloc(sizeof(clone_fork_ctx_t));
    if (!cctx) return (u32)-1U;
    cctx->proc   = p;
    cctx->resume = *ctx;

    t = thread_create_with_data(clone_fork_trampoline, cctx, p->pml4_phys, 0ULL);
    if (!t) { kfree(cctx); return (u32)-1U; }

    last = p->threads;
    while (last->proc_next) last = last->proc_next;
    last->proc_next = t;

    return t->id;
}

static int has_any_child(u32 parent_pid)
{
    u32 i;

    for (i = 0U; i < PROC_MAX; i++) {
        if (proc_table[i].state != PROC_FREE &&
            proc_table[i].parent_pid == parent_pid)
            return 1;
    }
    return 0;
}

u32 proc_wait(u32 pid, u32 *exit_code)
{
    if (pid == (u32)-1U) {
        process_t *caller = (process_t *)thread_current()->user_data;
        process_t *zombie;
        u32        child_pid;

        if (!has_any_child(caller->pid)) return (u32)-1U;

        for (;;) {
            zombie = find_zombie_child(caller->pid);
            if (zombie) break;
            wq_add(&caller->wait_chldexit, thread_current());
            thread_park();
        }

        child_pid = zombie->pid;
        if (exit_code) *exit_code = (zombie->exit_code & 0xFFU) << 8;
        zombie->state = PROC_FREE;
        return child_pid;
    }

    {
        process_t    *caller = (process_t *)thread_current()->user_data;
        wait_queue_t *wq     = caller ? &caller->wait_chldexit : &kernel_wait_chldexit;
        process_t    *p;

        for (;;) {
            p = proc_get(pid);
            if (!p || p->state == PROC_ZOMBIE) break;
            wq_add(wq, thread_current());
            thread_park();
        }

        if (!p) return (u32)-1U;
        if (exit_code) *exit_code = (p->exit_code & 0xFFU) << 8;
        p->state = PROC_FREE;
        return pid;
    }
}

process_t *proc_get(u32 pid)
{
    if (pid >= PROC_MAX) return 0;
    if (proc_table[pid].state == PROC_FREE) return 0;
    return &proc_table[pid];
}

u64 proc_brk(u64 new_brk)
{
    process_t *p = (process_t *)thread_current()->user_data;
    u64        old_top;
    u64        new_top;
    u64        va;

    if (new_brk < p->heap_start) return p->heap_end;

    old_top = (p->heap_end + 0xFFFULL) & ~0xFFFULL;
    new_top = (new_brk    + 0xFFFULL) & ~0xFFFULL;

    for (va = old_top; va < new_top; va += 0x1000ULL) {
        u32 frame = page_alloc();
        u8 *fdst  = (u8 *)((u64)frame + KERNEL_OFFSET);
        u32 k;
        for (k = 0U; k < 0x1000U; k++) fdst[k] = 0U;
        paging_map_user_page(p->pml4_phys, va, frame);
    }

    for (va = new_top; va < old_top; va += 0x1000ULL) {
        paging_unmap_user_page(p->pml4_phys, va);
    }

    p->heap_end = new_brk;
    return p->heap_end;
}

u64 proc_mmap(u64 length, u32 prot, u32 flags)
{
    process_t *p = (process_t *)thread_current()->user_data;
    u64        len;
    u64        start;
    u64        va;

    (void)prot;

    if (!(flags & MAP_ANONYMOUS)) return (u64)-1;
    if (length == 0U) return (u64)-1;

    len   = (length + 0xFFFULL) & ~0xFFFULL;
    start = p->mmap_next - len;

    for (va = start; va < p->mmap_next; va += 0x1000ULL) {
        u32 frame = page_alloc();
        u8 *fdst  = (u8 *)((u64)frame + KERNEL_OFFSET);
        u32 k;
        for (k = 0U; k < 0x1000U; k++) fdst[k] = 0U;
        paging_map_user_page(p->pml4_phys, va, frame);
    }

    p->mmap_next = start;
    return start;
}
