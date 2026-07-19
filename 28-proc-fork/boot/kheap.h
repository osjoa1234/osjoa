#ifndef KHEAP_H
#define KHEAP_H

#include "console.h"

#define KHEAP_START 0x400000U

void  kheap_init(void);
void *kmalloc(u32 size);
void  kfree(void *ptr);
u32   kheap_used(void);

#endif
