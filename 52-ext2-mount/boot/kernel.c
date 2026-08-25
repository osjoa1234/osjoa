#include "ata.h"
#include "console.h"
#include "ext2.h"
#include "gdt.h"
#include "initrd.h"
#include "interrupts.h"
#include "keyboard.h"
#include "kheap.h"
#include "paging.h"
#include "phys_mem.h"
#include "pipe.h"
#include "process.h"
#include "syscall.h"
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

extern char kernel_end[];
extern char stack_top[];

static void initrd_vfs_close(int fd) { (void)fd; }
static vfs_ops_t initrd_ops = { initrd_open, initrd_read, 0, initrd_size, initrd_vfs_close, 0 };

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

    console_printf("Hello world -- long mode (64-bit), processes\n");

    gdt_init((u64)stack_top);
    gdt_enable_syscall((u64)syscall_entry);
    syscall_kernel_rsp = (u64)stack_top;

    console_set_color(0x0D);
    console_printf("GDT ready: null/kcode64/kdata/ucode64/udata/TSS(0x28)\n");
    console_printf("syscall: SYSCALL/SYSRET MSR entry ready (LSTAR=0x%016lX)\n", (u64)syscall_entry);
    console_printf("paging: 4-level PAE active, 2MB huge pages, 1GB identity+kernel\n");

    if (mbi->flags & (1U << 6U)) {
        u32 kend_phys = (u32)((u64)kernel_end - KERNEL_OFFSET);

        paging_init(mbi->mmap_addr + KERNEL_OFFSET, mbi->mmap_length);
        phys_mem_init(mbi->mmap_addr + KERNEL_OFFSET, mbi->mmap_length, kend_phys);

        console_set_color(0x0F);
        console_printf("phys mem: %u free pages (%uMB usable)\n",
                       phys_mem_free_count(),
                       phys_mem_free_count() / 256U);
    }

    if (mbi->flags & (1U << 3U)) {
        const struct multiboot_mod *mod0 =
            (const struct multiboot_mod *)(u64)(mbi->mods_addr + KERNEL_OFFSET);

        phys_mem_reserve(mod0[0].mod_start, mod0[0].mod_end);
    }

    interrupts_init();

    console_set_color(0x0D);
    console_printf("IDT ready: 256 entries (16-byte gates) PIC=0x20/0x28\n");

    kheap_init();

    console_set_color(0x0B);
    console_printf("heap: kernel dir adopted, window at 0x%016lX (mapped=%uMB)\n",
                   KHEAP_START, paging_mapped_mb());

    ata_init();

    if (ext2_mount() != 0) {
        console_set_color(0x0CU);
        console_printf("ext2: mount failed\n");
    }

    if (mbi->flags & (1U << 3U)) {
        const struct multiboot_mod *mod =
            (const struct multiboot_mod *)(u64)(mbi->mods_addr + KERNEL_OFFSET);

        initrd_init((u64)mod[0].mod_start + KERNEL_OFFSET,
                    (u64)mod[0].mod_end   + KERNEL_OFFSET);

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

    console_set_color(0x0B);
    console_printf("timer: PIT 100Hz IRQ0 ready\n");

    keyboard_init();

    interrupts_enable();

    threads_init((u64)stack_top);
    proc_init();
    pipe_init();

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

    console_set_color(0x0A);
    console_printf("long mode: port-64 complete\n");

    halt_forever();
}
