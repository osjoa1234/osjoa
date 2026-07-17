#ifndef PAGING_H
#define PAGING_H

#include "console.h"

u32  paging_init(void);
void page_map_frame(u32 vaddr, u32 paddr);

#endif
