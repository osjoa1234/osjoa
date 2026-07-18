#ifndef GDT_H
#define GDT_H

#include "console.h"

void gdt_init(u32 kernel_stack);
void gdt_set_kernel_stack(u32 esp0);
void enter_user_mode_fork_child(u32 eip, u32 user_esp);

#endif
