#include "console.h"
#include "elf.h"
#include "gdt.h"
#include "initrd.h"
#include "interrupts.h"
#include "keyboard.h"
#include "syscall.h"
#include "kheap.h"
#include "monitor.h"
#include "paging.h"
#include "phys_mem.h"
#include "thread.h"
#include "timer.h"

struct multiboot_mmap_entry {
    u32 size;
    u32 addr_low;
    u32 addr_high;
    u32 len_low;
    u32 len_high;
    u32 type;
} __attribute__((packed));

struct multiboot_mod {
    u32 mod_start;
    u32 mod_end;
    u32 cmdline;
    u32 pad;
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
extern u32 stack_top;

extern void enter_user_mode(u32 eip, u32 user_esp);

static u8 user_stack[4096] __attribute__((aligned(16)));

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

static void task_a(void)
{
    u32 n;

    for (n = 0U; n < 3U; n++) {
        console_set_color(0x0EU);
        console_printf("thread A: step %u\n", n);
        thread_sleep(200U);
    }
}

static void task_b(void)
{
    u32 n;

    for (n = 0U; n < 3U; n++) {
        console_set_color(0x0BU);
        console_printf("thread B: step %u\n", n);
        thread_sleep(300U);
    }
}

void kernel_main(u32 magic, u32 phys_mbi)
{
    const struct multiboot_info *mbi =
        (const struct multiboot_info *)(phys_mbi + KERNEL_OFFSET);
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
    console_printf("multiboot info at 0x%08X\n", phys_mbi);

    gdt_init((u32)&stack_top);

    console_set_color(0x0DU);
    console_printf("GDT ready: 6 entries kernel(0x08/0x10) user(0x1B/0x23) TSS(0x28)\n");

    interrupts_init();

    console_set_color(0x0D);
    console_printf("IDT ready: 256 entries PIC=0x20/0x28 syscall=0x80(DPL=3)\n");

    console_set_color(0x0E);
    print_e820(mbi);

    cr3 = paging_init(mbi->mmap_addr + KERNEL_OFFSET, mbi->mmap_length);

    console_set_color(0x0B);
    console_printf("paging: %uMB direct mapped CR3=0x%08X\n", paging_mapped_mb(), cr3);

    phys_mem_init(mbi->mmap_addr + KERNEL_OFFSET,
                  mbi->mmap_length,
                  (u32)&kernel_end - KERNEL_OFFSET);

    console_set_color(0x0F);
    console_printf("phys mem: %u free pages (%uMB usable)\n",
                   phys_mem_free_count(),
                   phys_mem_free_count() / 256U);

    kheap_init();

    console_set_color(0x0B);
    console_printf("heap: init at 0x%08X\n", KHEAP_START);

    if (mbi->flags & (1U << 3U)) {
        const struct multiboot_mod *mod =
            (const struct multiboot_mod *)(mbi->mods_addr + KERNEL_OFFSET);
        u32 i;

        console_set_color(0x0EU);
        for (i = 0U; i < mbi->mods_count; i++) {
            console_printf("module[%u]: 0x%08X-0x%08X len=%u\n",
                           i, mod[i].mod_start, mod[i].mod_end,
                           mod[i].mod_end - mod[i].mod_start);
        }

        initrd_init(mod[0].mod_start + KERNEL_OFFSET,
                    mod[0].mod_end + KERNEL_OFFSET);

        console_set_color(0x0BU);
        console_printf("initramfs: %u file(s) found\n", initrd_file_count());
    } else {
        console_set_color(0x0CU);
        console_printf("initramfs: no modules loaded\n");
    }

    timer_init(100U);

    console_set_color(0x0BU);
    console_printf("timer: PIT 100Hz IRQ0 ready\n");

    keyboard_init();

    interrupts_enable();

    threads_init();

    console_set_color(0x0DU);
    console_printf("threads: init (main=id0)\n");

    thread_create(task_a);
    thread_create(task_b);

    console_set_color(0x07U);
    console_printf("threads: created A(id=1) B(id=2) -- preemptive scheduler\n");

    while (thread_any_runnable()) {
        __asm__ volatile ("hlt");
    }

    {
        int fd = initrd_open("init");
        u32 entry;

        if (fd < 0) {
            console_set_color(0x0CU);
            console_printf("elf: init not found in initramfs\n");
            halt_forever();
        }

        entry = elf_load(initrd_data(fd), initrd_size(fd));

        if (entry == 0U) {
            console_set_color(0x0CU);
            console_printf("elf: load failed\n");
            halt_forever();
        }

        console_set_color(0x0AU);
        console_printf("elf: loaded init (entry=0x%08X)\n", entry);
        console_printf("threads: all done -- entering user mode (ring 3), ELF init\n");

        enter_user_mode(entry, (u32)(user_stack + sizeof(user_stack)));
    }
}
