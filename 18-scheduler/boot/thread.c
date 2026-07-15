#include "thread.h"
#include "interrupts.h"
#include "kheap.h"
#include "timer.h"

#define THREAD_STACK_SIZE 4096U
#define SCHED_QUANTUM     10U

extern void switch_context(u32 *old_esp, u32 new_esp);

static thread_t  idle_task;
static thread_t *current;
static u32       next_id;
static u32       ticks_to_switch;
static u32       scheduler_ready;

void threads_init(void)
{
    idle_task.esp       = 0U;
    idle_task.id        = next_id++;
    idle_task.state     = THREAD_RUNNING;
    idle_task.wake_tick = 0U;
    idle_task.next      = &idle_task;
    current         = &idle_task;
    ticks_to_switch = SCHED_QUANTUM;
    scheduler_ready = 1U;
}

thread_t *thread_create(thread_fn_t fn)
{
    thread_t *t;
    u32      *stack;
    u32      *sp;
    thread_t *last;

    t = (thread_t *)kmalloc(sizeof(thread_t));
    if (!t) return 0;

    stack = (u32 *)kmalloc(THREAD_STACK_SIZE);
    if (!stack) { kfree(t); return 0; }

    sp = (u32 *)((u8 *)stack + THREAD_STACK_SIZE);

    *--sp = (u32)thread_exit;
    *--sp = (u32)fn;
    *--sp = 0x00000202U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;
    *--sp = 0U;

    t->esp       = (u32)sp;
    t->id        = next_id++;
    t->state     = THREAD_RUNNING;
    t->wake_tick = 0U;

    last = &idle_task;
    while (last->next != &idle_task) last = last->next;
    last->next = t;
    t->next    = &idle_task;

    return t;
}

static void wake_sleeping(void)
{
    u32       now = timer_ticks();
    thread_t *t   = idle_task.next;

    while (t != &idle_task) {
        if (t->state == THREAD_SLEEPING && now >= t->wake_tick) {
            t->state = THREAD_RUNNING;
        }
        t = t->next;
    }
}

static thread_t *next_runnable(thread_t *from)
{
    thread_t *t = from->next;

    while (t != from && t->state != THREAD_RUNNING) {
        t = t->next;
    }
    return t;
}

void thread_yield(void)
{
    thread_t *prev;
    thread_t *next;

    interrupts_disable();
    wake_sleeping();

    prev = current;
    next = next_runnable(prev);

    if (next == prev) {
        interrupts_enable();
        return;
    }

    current = next;
    switch_context(&prev->esp, next->esp);
    interrupts_enable();
}

void thread_sleep(u32 ms)
{
    thread_t *prev;
    thread_t *next;

    interrupts_disable();

    current->state     = THREAD_SLEEPING;
    current->wake_tick = timer_ticks() + ms * timer_hz() / 1000U;

    prev = current;
    next = next_runnable(prev);

    if (next == prev) {
        interrupts_enable();
        while (current->state == THREAD_SLEEPING) {
            __asm__ volatile ("hlt");
        }
        return;
    }

    current = next;
    switch_context(&prev->esp, next->esp);
    interrupts_enable();
}

void thread_exit(void)
{
    thread_t *prev;
    thread_t *next;

    interrupts_disable();
    current->state = THREAD_DEAD;

    prev = current;
    next = next_runnable(prev);

    if (next == prev) {
        interrupts_enable();
        return;
    }

    current = next;
    switch_context(&prev->esp, next->esp);
}

void scheduler_tick(void)
{
    thread_t *prev;
    thread_t *next;

    if (!scheduler_ready) return;

    wake_sleeping();

    prev = current;
    next = next_runnable(prev);

    if (next == prev) return;

    if (prev == &idle_task) {
        ticks_to_switch = SCHED_QUANTUM;
        current = next;
        switch_context(&idle_task.esp, next->esp);
        return;
    }

    if (ticks_to_switch > 0U) {
        ticks_to_switch--;
        return;
    }
    ticks_to_switch = SCHED_QUANTUM;

    current = next;
    switch_context(&prev->esp, next->esp);
}

thread_t *thread_current(void)
{
    return current;
}

thread_t *thread_list(void)
{
    return &idle_task;
}

u32 thread_any_runnable(void)
{
    const thread_t *t = idle_task.next;

    while (t != &idle_task) {
        if (t->state != THREAD_DEAD) return 1U;
        t = t->next;
    }
    return 0U;
}
