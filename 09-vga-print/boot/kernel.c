#include "console.h"

static const u32 multiboot_magic = 0x2BADB002U;

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kernel_main(u32 magic, u32 multiboot_info)
{
    const u8 ready_color = 0x0A;

    console_set_color(0x0F);
    console_clear();

    if (magic != multiboot_magic) {
        console_set_color(0x0C);
        console_printf("Hello world -- protected mode (32-bit), GRUB2 handoff failed\n");
        console_printf("magic=0x%08X expected=0x%08X\n", magic, multiboot_magic);
        halt_forever();
    }

    console_printf("Hello world -- protected mode (32-bit), C kernel loaded by GRUB2 via Multiboot\n");

    console_set_color(ready_color);
    console_printf("VGA console ready: %ux%u color=0x%X\n", VGA_WIDTH, VGA_HEIGHT, ready_color);

    console_set_color(0x0B);
    console_printf("multiboot info at 0x%08X\n", multiboot_info);

    console_set_color(0x07);
    console_printf("printf check: %s %c %d %u 0x%x %%\n", "text", '!', -42, 12345U, 0xC0FFEEU);

    halt_forever();
}