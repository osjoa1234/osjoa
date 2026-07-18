#ifndef PAGING_H
#define PAGING_H

#include "console.h"

u32  paging_init(u32 mmap_addr, u32 mmap_length);
u32  paging_mapped_mb(void);
void page_map_frame(u32 vaddr, u32 paddr);
u32  paging_clone_dir(void);
void paging_set_dir(u32 pd_phys);
void paging_restore_kernel_dir(void);
void paging_map_user_page(u32 pd_phys, u32 vaddr, u32 paddr);
void paging_free_user_pages(u32 pd_phys);

#endif
