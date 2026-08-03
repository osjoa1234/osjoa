#include "console.h"
#include "gdt.h"
#include "initrd.h"
#include "interrupts.h"
#include "keyboard.h"
#include "syscall.h"
#include "kheap.h"
#include "monitor.h"
#include "paging.h"
#include "phys_mem.h"
#include "process.h"
#include "thread.h"
#include "timer.h"
#include "vfs.h"

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

static void initrd_vfs_close(int fd) { (void)fd; }
static vfs_ops_t initrd_ops = { initrd_open, initrd_read, 0, initrd_size, initrd_vfs_close };

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kernel_main(u32 magic, u32 phys_mbi)
{
    const struct multiboot_info *mbi =
        (const struct multiboot_info *)(phys_mbi + KERNEL_OFFSET);
    u32 cr3;

    console_set_color(0x0F);
    console_clear();

    if (magic != multiboot_magic) {
        console_set_color(0x0C);
        console_printf("Hello world -- protected mode (32-bit), GRUB2 handoff failed\n");
        console_printf("magic=0x%08X expected=0x%08X\n", magic, multiboot_magic);
        halt_forever();
    }

    console_printf("Hello world -- protected mode (32-bit), C kernel, linux-abi\n");

    gdt_init((u32)&stack_top);

    console_set_color(0x0DU);
    console_printf("GDT ready: 6 entries kernel(0x08/0x10) user(0x1B/0x23) TSS(0x28)\n");

    interrupts_init();

    console_set_color(0x0D);
    console_printf("IDT ready: 256 entries PIC=0x20/0x28 syscall=0x80(DPL=3)\n");

    cr3 = paging_init(mbi->mmap_addr + KERNEL_OFFSET, mbi->mmap_length);

    console_set_color(0x0B);
    console_printf("paging: %uMB direct mapped CR3=0x%08X\n", paging_mapped_mb(), cr3);

    phys_mem_init(mbi->mmap_addr + KERNEL_OFFSET, mbi->mmap_length, (u32)&kernel_end - KERNEL_OFFSET);

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

        initrd_init(mod[0].mod_start + KERNEL_OFFSET, mod[0].mod_end + KERNEL_OFFSET);

        console_set_color(0x0BU);
        console_printf("initramfs: %u file(s) found\n", initrd_file_count());

        vfs_init();
        vfs_mount("/", &initrd_ops);
        console_printf("vfs: initrd mounted at /\n");
    } else {
        console_set_color(0x0CU);
        console_printf("initramfs: no modules loaded\n");
    }

    timer_init(100U);

    console_set_color(0x0BU);
    console_printf("timer: PIT 100Hz IRQ0 ready\n");

    keyboard_init();

    interrupts_enable();

    threads_init((u32)&stack_top);
    proc_init();

    {
        u32 init_pid = proc_spawn("init");

        if (init_pid == (u32)-1U) {
            console_set_color(0x0CU);
            console_printf("processes: failed to spawn init\n");
            halt_forever();
        }

        console_set_color(0x0AU);
        console_printf("processes: init spawned pid=%u\n", init_pid);

        {
            u32 exit_code = (u32)-1U;
            proc_wait(init_pid, &exit_code);
            console_set_color(0x0AU);
            console_printf("processes: init exited code=%u\n", exit_code);
        }
    }

    halt_forever();
}
