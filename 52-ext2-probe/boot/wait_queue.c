#include "wait_queue.h"
#include "kheap.h"

void wq_init(wait_queue_t *wq)
{
    wq->head = 0;
}

void wq_add(wait_queue_t *wq, thread_t *t)
{
    wq_node_t *node = (wq_node_t *)kmalloc(sizeof(wq_node_t));
    node->thread = t;
    node->next   = wq->head;
    wq->head     = node;
}

void wq_wake_all(wait_queue_t *wq)
{
    wq_node_t *n = wq->head;

    while (n) {
        wq_node_t *next = n->next;
        thread_unpark(n->thread);
        kfree(n);
        n = next;
    }
    wq->head = 0;
}
