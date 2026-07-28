#ifndef WAIT_QUEUE_H
#define WAIT_QUEUE_H

#include "thread.h"

typedef struct wq_node {
    thread_t        *thread;
    struct wq_node  *next;
} wq_node_t;

typedef struct {
    wq_node_t *head;
} wait_queue_t;

void wq_init(wait_queue_t *wq);
void wq_add(wait_queue_t *wq, thread_t *t);
void wq_wake_all(wait_queue_t *wq);

#endif
