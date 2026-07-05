#include "console.h"
#include "interrupts.h"
#include "keyboard.h"
#include "paging.h"

struct multiboot_mmap_entry {
    u32 size;
    u32 addr_low;
    u32 addr_high;
    u32 len_low;
    u32 len_high;
    u32 type;
} __attribute__((packed));

struct multiboot_info {
    u32 flags;
    u32 mem_lower;
    u32 mem_upper;
    u32 boot_device;
    u32 cmdline;
    u32 mods_count;
    u32 mods_addr;
    u8  syms[16];
    u32 mmap_length;
    u32 mmap_addr;
} __attribute__((packed));

static const u32 multiboot_magic = 0x2BADB002U;

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void print_e820(const struct multiboot_info *mbi)
{
    u32 offset = 0U;

    if (!(mbi->flags & (1U << 6U))) {
        console_printf("e820: not available\n");
        return;
    }

    while (offset < mbi->mmap_length) {
        const struct multiboot_mmap_entry *entry =
            (const struct multiboot_mmap_entry *)(mbi->mmap_addr + offset);
        const char *type_name;

        switch (entry->type) {
        case 1U: type_name = "usable";          break;
        case 2U: type_name = "reserved";         break;
        case 3U: type_name = "ACPI reclaimable"; break;
        case 4U: type_name = "ACPI NVS";         break;
        case 5U: type_name = "bad memory";       break;
        default: type_name = "unknown";          break;
        }

        console_set_color(entry->type == 1U ? 0x0AU : 0x08U);
        console_printf("e820: 0x%08X len=0x%08X type=%u %s\n",
                       entry->addr_low, entry->len_low,
                       entry->type, type_name);

        offset += entry->size + 4U;
    }
}

void kernel_main(u32 magic, u32 multiboot_info)
{
    const struct multiboot_info *mbi =
        (const struct multiboot_info *)multiboot_info;
    const u8 ready_color = 0x0A;
    u32 cr3;

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

    interrupts_init();

    console_set_color(0x0D);
    console_printf("IDT ready: 256 entries PIC=0x20/0x28\n");

    console_set_color(0x0E);
    print_e820(mbi);

    cr3 = paging_init();

    console_set_color(0x0B);
    console_printf("paging enabled: 4MB identity mapped CR3=0x%08X\n", cr3);

    keyboard_init();

    console_set_color(0x0F);
    console_printf("keyboard demo: type to echo keystrokes\n");

    interrupts_enable();

    halt_forever();
}
