#include "signal.h"
#include "paging.h"
#include "phys_mem.h"
#include "thread.h"

#define SIG_TRAMP_VA PROC_USTACK_TOP

typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 rip;
    u64 rflags;
    u64 user_rsp;
} sigctx_t;

static const u8 trampoline_code[] = { 0xB8, 0x0FU, 0x00U, 0x00U, 0x00U, 0x0FU, 0x05U };

void signal_setup_trampoline(u32 pml4_phys)
{
    u32 frame = page_alloc();
    u8 *dst   = (u8 *)((u64)frame + KERNEL_OFFSET);
    u32 i;

    for (i = 0U; i < 0x1000U; i++) dst[i] = 0U;
    for (i = 0U; i < sizeof(trampoline_code); i++) dst[i] = trampoline_code[i];

    paging_map_user_page(pml4_phys, SIG_TRAMP_VA, frame);
}

void signal_reset_handlers(process_t *p)
{
    u32 i;
    for (i = 0U; i < NSIG; i++) p->sig_handler[i] = SIG_DFL;
}

void signal_raise_current(u32 signum)
{
    process_t *p = (process_t *)thread_current()->user_data;

    if (!p || signum >= NSIG) return;
    p->sig_pending |= (1ULL << signum);
}

static void deliver_signal(process_t *p, struct interrupt_frame *frame, u32 signum)
{
    u64       handler = p->sig_handler[signum];
    u64       sp;
    sigctx_t *ctx;

    if (handler == SIG_IGN) return;

    if (handler == SIG_DFL) {
        proc_exit(128U + signum);
        return;
    }

    sp  = frame->user_rsp & ~0xFULL;
    sp -= sizeof(sigctx_t);

    ctx = (sigctx_t *)sp;
    ctx->r15 = frame->r15; ctx->r14 = frame->r14; ctx->r13 = frame->r13; ctx->r12 = frame->r12;
    ctx->r11 = frame->r11; ctx->r10 = frame->r10; ctx->r9  = frame->r9;  ctx->r8  = frame->r8;
    ctx->rdi = frame->rdi; ctx->rsi = frame->rsi; ctx->rbp = frame->rbp; ctx->rbx = frame->rbx;
    ctx->rdx = frame->rdx; ctx->rcx = frame->rcx; ctx->rax = frame->rax;
    ctx->rip      = frame->rip;
    ctx->rflags   = frame->rflags;
    ctx->user_rsp = frame->user_rsp;

    sp -= 8ULL;
    *(u64 *)sp = SIG_TRAMP_VA;

    frame->rdi      = (u64)signum;
    frame->rip      = handler;
    frame->user_rsp = sp;
}

void signal_deliver_pending(struct interrupt_frame *frame)
{
    process_t *p;
    u64        ready;
    u32        signum;

    if (frame->cs != 0x1BULL) return;

    p = (process_t *)thread_current()->user_data;
    if (!p) return;

    ready = p->sig_pending & ~p->sig_blocked;
    if (!ready) return;

    for (signum = 0U; signum < NSIG; signum++) {
        if (ready & (1ULL << signum)) {
            p->sig_pending &= ~(1ULL << signum);
            deliver_signal(p, frame, signum);
            return;
        }
    }
}

u32 sys_rt_sigaction(u32 signum, const sigaction_t *act, sigaction_t *oldact, u64 sigsetsize)
{
    process_t *p = (process_t *)thread_current()->user_data;

    (void)sigsetsize;
    if (!p || signum >= NSIG) return (u32)-1U;

    if (oldact) {
        oldact->handler  = p->sig_handler[signum];
        oldact->flags    = 0ULL;
        oldact->restorer = 0ULL;
        oldact->mask     = 0ULL;
    }

    if (act) p->sig_handler[signum] = act->handler;

    return 0U;
}

u32 sys_rt_sigprocmask(u32 how, const u64 *set, u64 *oldset, u64 sigsetsize)
{
    process_t *p = (process_t *)thread_current()->user_data;

    (void)sigsetsize;
    if (!p) return (u32)-1U;

    if (oldset) *oldset = p->sig_blocked;

    if (set) {
        switch (how) {
        case SIG_BLOCK:   p->sig_blocked |= *set;      break;
        case SIG_UNBLOCK: p->sig_blocked &= ~(*set);   break;
        case SIG_SETMASK: p->sig_blocked = *set;       break;
        default: break;
        }
    }

    return 0U;
}

void sys_rt_sigreturn(struct interrupt_frame *frame)
{
    sigctx_t *ctx = (sigctx_t *)frame->user_rsp;

    frame->r15 = ctx->r15; frame->r14 = ctx->r14; frame->r13 = ctx->r13; frame->r12 = ctx->r12;
    frame->r11 = ctx->r11; frame->r10 = ctx->r10; frame->r9  = ctx->r9;  frame->r8  = ctx->r8;
    frame->rdi = ctx->rdi; frame->rsi = ctx->rsi; frame->rbp = ctx->rbp; frame->rbx = ctx->rbx;
    frame->rdx = ctx->rdx; frame->rcx = ctx->rcx; frame->rax = ctx->rax;
    frame->rip      = ctx->rip;
    frame->rflags   = ctx->rflags;
    frame->user_rsp = ctx->user_rsp;
}
