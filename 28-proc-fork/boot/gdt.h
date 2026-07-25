#ifndef GDT_H
#define GDT_H

#include "console.h"

void gdt_init(u32 kernel_stack);
void gdt_set_kernel_stack(u32 esp0);

#endif
