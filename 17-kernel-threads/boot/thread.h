#ifndef THREAD_H
#define THREAD_H

#include "console.h"

typedef void (*thread_fn_t)(void);

#define THREAD_RUNNING 0U
#define THREAD_DEAD    1U

typedef struct thread {
    u32           esp;
    u32           id;
    u32           state;
    struct thread *next;
} thread_t;

void      threads_init(void);
thread_t *thread_create(thread_fn_t fn);
void      thread_yield(void);
void      thread_exit(void);
thread_t *thread_current(void);
thread_t *thread_list(void);
u32       thread_any_runnable(void);

#endif
