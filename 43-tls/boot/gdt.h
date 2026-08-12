#ifndef GDT_H
#define GDT_H

#include "console.h"

void gdt_init(u64 kernel_stack);
void gdt_set_kernel_stack(u64 rsp0);
void gdt_set_fs_base(u64 base);

#endif
