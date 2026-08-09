#ifndef PAGING_H
#define PAGING_H

#include "console.h"

void paging_init(u64 mmap_addr, u32 mmap_length);
u32  paging_mapped_mb(void);
void page_map_frame(u64 vaddr, u32 paddr);
u32  paging_clone_dir(void);
void paging_set_dir(u32 pml4_phys);
void paging_restore_kernel_dir(void);
void paging_map_user_page(u32 pml4_phys, u64 vaddr, u32 paddr);
void paging_free_user_pages(u32 pml4_phys);
void paging_copy_user_pages(u32 src_pml4_phys, u32 dst_pml4_phys);

#endif
