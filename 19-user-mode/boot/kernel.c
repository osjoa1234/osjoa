#include "console.h"
#include "gdt.h"
#include "interrupts.h"
#include "keyboard.h"
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

static void user_task(void)
{
    volatile u16 *vga = (volatile u16 *)0x000B8000U;
    const char   *msg = "Hello from user mode -- ring 3 direct VGA write";
    u32           i;

    for (i = 0U; msg[i]; i++) {
        vga[80U + i] = (u16)(0x1F00U | (u8)msg[i]);
    }

    for (;;) {}
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

    gdt_init((u32)&stack_top);

    console_set_color(0x0DU);
    console_printf("GDT ready: 6 entries kernel(0x08/0x10) user(0x1B/0x23) TSS(0x28)\n");

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

    console_set_color(0x0AU);
    console_printf("threads: all done -- entering user mode (ring 3)\n");

    enter_user_mode((u32)user_task,
                    (u32)(user_stack + sizeof(user_stack)));
}
