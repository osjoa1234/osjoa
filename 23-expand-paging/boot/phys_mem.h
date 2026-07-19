#ifndef PHYS_MEM_H
#define PHYS_MEM_H

#include "console.h"

void phys_mem_init(u32 mmap_vaddr, u32 mmap_length, u32 kernel_phys_end);
u32  page_alloc(void);
void page_free(u32 addr);
u32  phys_mem_free_count(void);

#endif
