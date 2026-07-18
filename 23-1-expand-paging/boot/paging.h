#ifndef PAGING_H
#define PAGING_H

#include "console.h"

u32  paging_init(u32 mmap_addr, u32 mmap_length);
u32  paging_mapped_mb(void);
void page_map_frame(u32 vaddr, u32 paddr);

#endif
