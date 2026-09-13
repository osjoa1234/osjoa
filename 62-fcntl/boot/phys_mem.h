#ifndef PHYS_MEM_H
#define PHYS_MEM_H

#include "console.h"

void phys_mem_init(u64 mmap_addr, u32 mmap_length, u32 kernel_end);
void phys_mem_reserve(u32 start, u32 end);
u32  page_alloc(void);
void page_free(u32 addr);
u32  phys_mem_free_count(void);

#endif
