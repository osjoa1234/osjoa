#include "console.h"
#include "gdt.h"
#include "interrupts.h"
#include "keyboard.h"
#include "phys_mem.h"
#include "timer.h"

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

extern char kernel_end[];
extern char stack_top[];

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kernel_main(u32 magic, u32 phys_mbi)
{
    const struct multiboot_info *mbi =
        (const struct multiboot_info *)(u64)(phys_mbi + KERNEL_OFFSET);

    console_set_color(0x0F);
    console_clear();

    if (magic != multiboot_magic) {
        console_set_color(0x0C);
        console_printf("long mode: multiboot handoff failed\n");
        console_printf("magic=0x%08X expected=0x%08X\n", magic, multiboot_magic);
        halt_forever();
    }

    console_printf("Hello world -- long mode (64-bit)\n");

    gdt_init((u64)stack_top);

    console_set_color(0x0D);
    console_printf("GDT ready: null/kcode64/kdata/ucode64/udata/TSS(0x28)\n");
    console_printf("paging: 4-level PAE active, 2MB huge pages, 1GB identity+kernel\n");

    if (mbi->flags & (1U << 6U)) {
        u32 kend_phys = (u32)((u64)kernel_end - KERNEL_OFFSET);
        phys_mem_init(mbi->mmap_addr + KERNEL_OFFSET, mbi->mmap_length, kend_phys);

        console_set_color(0x0F);
        console_printf("phys mem: %u free pages (%uMB usable)\n",
                       phys_mem_free_count(),
                       phys_mem_free_count() / 256U);
    }

    interrupts_init();

    console_set_color(0x0D);
    console_printf("IDT ready: 256 entries (16-byte gates) PIC=0x20/0x28 syscall=0x80(DPL=3)\n");

    timer_init(100U);

    console_set_color(0x0B);
    console_printf("timer: PIT 100Hz IRQ0 ready\n");

    keyboard_init();

    interrupts_enable();

    __asm__ volatile ("int3");

    {
        u32 t0 = timer_ticks();
        console_set_color(0x0EU);
        console_printf("sleep 200ms: ticks before=%u\n", t0);
        timer_sleep(200U);
        console_set_color(0x0AU);
        console_printf("sleep done: ticks after=%u (delta=%u)\n",
                       timer_ticks(), timer_ticks() - t0);
    }

    console_set_color(0x0A);
    console_printf("long mode: IDT-64 ready (processes in 39)\n");

    halt_forever();
}
