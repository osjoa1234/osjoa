#include "console.h"
#include "interrupts.h"
#include "keyboard.h"
#include "kheap.h"
#include "paging.h"
#include "phys_mem.h"

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

extern u32 kernel_end;

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
    u32 *a;
    u32 *b;
    char *c;
    u32 *d;

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

    phys_mem_init(mbi->mmap_addr, mbi->mmap_length, (u32)&kernel_end);

    console_set_color(0x0F);
    console_printf("phys mem: %u free pages (%uMB usable)\n",
                   phys_mem_free_count(),
                   phys_mem_free_count() / 256U);

    kheap_init();

    console_set_color(0x0B);
    console_printf("heap: init at 0x%08X\n", KHEAP_START);

    a = (u32  *)kmalloc(sizeof(u32));
    b = (u32  *)kmalloc(sizeof(u32));
    c = (char *)kmalloc(8);

    *a = 1U;
    *b = 2U;
    c[0] = 'h'; c[1] = 'e'; c[2] = 'l'; c[3] = 'l'; c[4] = 'o'; c[5] = '\0';

    console_set_color(0x0E);
    console_printf("kmalloc: a=0x%08X b=0x%08X c=0x%08X\n",
                   (u32)a, (u32)b, (u32)c);
    console_set_color(0x0A);
    console_printf("*a=%u *b=%u c=\"%s\"\n", *a, *b, c);

    kfree(b);
    kfree(a);

    console_set_color(0x07);
    console_printf("kfree b+a: used=%u bytes\n", kheap_used());

    d = (u32 *)kmalloc(sizeof(u32));
    *d = 42U;

    console_set_color(0x0D);
    console_printf("kmalloc after free: d=0x%08X *d=%u %s\n",
                   (u32)d, *d, (u32)d == (u32)a ? "reused" : "new");

    keyboard_init();

    console_set_color(0x0F);
    console_printf("keyboard demo: type to echo keystrokes\n");

    interrupts_enable();

    halt_forever();
}
