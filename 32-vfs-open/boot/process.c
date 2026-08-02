#include "process.h"
#include "elf.h"
#include "initrd.h"
#include "paging.h"
#include "phys_mem.h"
#include "kheap.h"
#include "console.h"

static process_t    proc_table[PROC_MAX];
static wait_queue_t kernel_wait_chldexit;

extern void enter_user_mode(u32 eip, u32 esp);
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
    enter_user_mode(p->entry, PROC_USTACK_TOP);
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
    int        fd;
    u32        pd;
    u32        entry;
    thread_t  *t;

    p = proc_alloc();
    if (!p) return (u32)-1U;

    fd = initrd_open(name);
    if (fd < 0) { p->state = PROC_FREE; return (u32)-1U; }

    pd    = paging_clone_dir();
    entry = elf_load_process(initrd_data(fd), initrd_size(fd), pd);
    if (!entry) { p->state = PROC_FREE; return (u32)-1U; }

    p->entry      = entry;
    p->pd_phys    = pd;
    p->parent_pid = PROC_NO_PARENT;

    t = thread_create_with_data(proc_run_trampoline, p);
    if (!t) { p->state = PROC_FREE; return (u32)-1U; }

    t->pd        = pd;
    t->proc_next = 0;
    p->threads   = t;

    return p->pid;
}

u32 proc_fork(const fork_resume_t *ctx)
{
    process_t *parent = (process_t *)thread_current()->user_data;
    process_t *child;
    u32        child_pd;
    thread_t  *t;

    child = proc_alloc();
    if (!child) return (u32)-1U;

    child_pd = paging_clone_dir();
    paging_copy_user_pages(parent->pd_phys, child_pd);

    child->fork_ctx   = *ctx;
    child->entry      = ctx->eip;
    child->pd_phys    = child_pd;
    child->parent_pid = parent->pid;

    t = thread_create_with_data(fork_child_trampoline, child);
    if (!t) { child->state = PROC_FREE; return (u32)-1U; }

    t->pd         = child_pd;
    t->proc_next  = 0;
    child->threads = t;

    return child->pid;
}

void proc_exec(const char *name)
{
    process_t *p = (process_t *)thread_current()->user_data;
    int        fd;
    u32        entry;
    u32        j;

    for (j = 0U; j < PROC_FD_MAX; j++) {
        if (p->fds[j]) { vfs_close(p->fds[j]); p->fds[j] = 0; }
    }

    fd = initrd_open(name);
    if (fd < 0) return;

    paging_free_user_pages(p->pd_phys);

    entry = elf_load_process(initrd_data(fd), initrd_size(fd), p->pd_phys);
    if (!entry) return;

    p->entry = entry;
    enter_user_mode(entry, PROC_USTACK_TOP);
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

    t = thread_create_with_data(clone_fork_trampoline, cctx);
    if (!t) { kfree(cctx); return (u32)-1U; }
    t->pd = p->pd_phys;

    last = p->threads;
    while (last->proc_next) last = last->proc_next;
    last->proc_next = t;

    return t->id;
}

u32 proc_wait(u32 pid, u32 *exit_code)
{
    if (pid == (u32)-1U) {
        process_t *caller = (process_t *)thread_current()->user_data;
        process_t *zombie;
        u32        child_pid;

        for (;;) {
            zombie = find_zombie_child(caller->pid);
            if (zombie) break;
            wq_add(&caller->wait_chldexit, thread_current());
            thread_park();
        }

        child_pid = zombie->pid;
        if (exit_code) *exit_code = zombie->exit_code;
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
        if (exit_code) *exit_code = p->exit_code;
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
