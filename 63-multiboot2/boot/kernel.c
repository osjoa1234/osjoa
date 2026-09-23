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

struct multiboot2_info {
    u32 total_size;
    u32 reserved;
} __attribute__((packed));

struct multiboot2_tag {
    u32 type;
    u32 size;
} __attribute__((packed));

struct multiboot2_tag_module {
    u32  type;
    u32  size;
    u32  mod_start;
    u32  mod_end;
    char cmdline[];
} __attribute__((packed));

struct multiboot2_tag_mmap_entry {
    u64 base_addr;
    u64 length;
    u32 type;
    u32 reserved;
} __attribute__((packed));

struct multiboot2_tag_mmap {
    u32                               type;
    u32                               size;
    u32                               entry_size;
    u32                               entry_version;
    struct multiboot2_tag_mmap_entry entries[];
} __attribute__((packed));

struct multiboot2_tag_rsdp {
    u32 type;
    u32 size;
    u8  rsdp[];
} __attribute__((packed));

#define MULTIBOOT2_TAG_END      0U
#define MULTIBOOT2_TAG_MODULE   3U
#define MULTIBOOT2_TAG_MMAP     6U
#define MULTIBOOT2_TAG_ACPI_OLD 14U
#define MULTIBOOT2_TAG_ACPI_NEW 15U

static const u32 multiboot2_magic = 0x36D76289U;

static const struct multiboot2_tag *multiboot2_find_tag(const struct multiboot2_info *mbi, u32 type)
{
    const u8 *ptr = (const u8 *)mbi + sizeof(struct multiboot2_info);
    const u8 *end = (const u8 *)mbi + mbi->total_size;

    while (ptr < end) {
        const struct multiboot2_tag *tag = (const struct multiboot2_tag *)ptr;

        if (tag->type == MULTIBOOT2_TAG_END) break;
        if (tag->type == type) return tag;

        ptr += (tag->size + 7U) & ~7U;
    }

    return 0;
}

extern char kernel_end[];
extern char stack_top[];

static void initrd_vfs_close(int fd) { (void)fd; }
static int  initrd_vfs_open(const char *path, u32 flags) { (void)flags; return initrd_open(path); }
static vfs_ops_t initrd_ops = { initrd_vfs_open, initrd_read, 0, initrd_size, initrd_vfs_close, 0, 0, initrd_mode };
static vfs_ops_t ext2_ops   = { ext2_open, ext2_read, ext2_write, ext2_size, ext2_close, ext2_dup, ext2_getdents, ext2_mode, ext2_mkdir, ext2_unlink, ext2_symlink, ext2_readlink, ext2_lstat };

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kernel_main(u32 magic, u32 phys_mbi)
{
    const struct multiboot2_info *mbi =
        (const struct multiboot2_info *)(u64)(phys_mbi + KERNEL_OFFSET);

    console_set_color(0x0F);
    console_clear();

    if (magic != multiboot2_magic) {
        console_set_color(0x0C);
        console_printf("long mode: multiboot2 handoff failed\n");
        console_printf("magic=0x%08X expected=0x%08X\n", magic, multiboot2_magic);
        halt_forever();
    }

    console_printf("Hello world -- long mode (64-bit), processes, multiboot2\n");

    gdt_init((u64)stack_top);
    gdt_enable_syscall((u64)syscall_entry);
    syscall_kernel_rsp = (u64)stack_top;

    console_set_color(0x0D);
    console_printf("GDT ready: null/kcode64/kdata/ucode64/udata/TSS(0x28)\n");
    console_printf("syscall: SYSCALL/SYSRET MSR entry ready (LSTAR=0x%016lX)\n", (u64)syscall_entry);
    console_printf("paging: 4-level PAE active, 2MB huge pages, 1GB identity+kernel\n");

    {
        const struct multiboot2_tag_mmap *mmap_tag =
            (const struct multiboot2_tag_mmap *)multiboot2_find_tag(mbi, MULTIBOOT2_TAG_MMAP);

        if (mmap_tag) {
            u32 kend_phys       = (u32)((u64)kernel_end - KERNEL_OFFSET);
            u64 entries_vaddr   = (u64)mmap_tag->entries;
            u32 entries_length  = mmap_tag->size - (u32)sizeof(struct multiboot2_tag_mmap);

            paging_init(entries_vaddr, entries_length, mmap_tag->entry_size);
            phys_mem_init(entries_vaddr, entries_length, mmap_tag->entry_size, kend_phys);

            console_set_color(0x0F);
            console_printf("phys mem: %u free pages (%uMB usable)\n",
                           phys_mem_free_count(),
                           phys_mem_free_count() / 256U);
        }
    }

    {
        const struct multiboot2_tag_module *mod0 =
            (const struct multiboot2_tag_module *)multiboot2_find_tag(mbi, MULTIBOOT2_TAG_MODULE);

        if (mod0) {
            phys_mem_reserve(mod0->mod_start, mod0->mod_end);
        }
    }

    {
        const struct multiboot2_tag_rsdp *rsdp_tag =
            (const struct multiboot2_tag_rsdp *)multiboot2_find_tag(mbi, MULTIBOOT2_TAG_ACPI_NEW);

        if (!rsdp_tag) {
            rsdp_tag = (const struct multiboot2_tag_rsdp *)multiboot2_find_tag(mbi, MULTIBOOT2_TAG_ACPI_OLD);
        }

        if (rsdp_tag) {
            console_set_color(0x0B);
            console_printf("acpi: RSDP tag received (%s, %u bytes) at 0x%016lX\n",
                           rsdp_tag->type == MULTIBOOT2_TAG_ACPI_NEW ? "ACPI 2.0+" : "ACPI 1.0",
                           rsdp_tag->size - 8U,
                           (u64)rsdp_tag->rsdp);
        } else {
            console_set_color(0x0C);
            console_printf("acpi: no RSDP tag from bootloader\n");
        }
    }

    interrupts_init();

    console_set_color(0x0D);
    console_printf("IDT ready: 256 entries (16-byte gates) PIC=0x20/0x28\n");

    kheap_init();

    console_set_color(0x0B);
    console_printf("heap: kernel dir adopted, window at 0x%016lX (mapped=%uMB)\n",
                   KHEAP_START, paging_mapped_mb());

    ata_init();

    if (ext2_init() != 0) {
        console_set_color(0x0CU);
        console_printf("ext2: init failed\n");
    }

    {
        const struct multiboot2_tag_module *mod =
            (const struct multiboot2_tag_module *)multiboot2_find_tag(mbi, MULTIBOOT2_TAG_MODULE);

        if (mod) {
            initrd_init((u64)mod->mod_start + KERNEL_OFFSET,
                        (u64)mod->mod_end   + KERNEL_OFFSET);

            console_set_color(0x0BU);
            console_printf("initramfs: %u file(s) found\n", initrd_file_count());

            vfs_init();
            vfs_mount("/", &initrd_ops);
            console_printf("vfs: initrd mounted at /\n");

            vfs_mount("/disk/", &ext2_ops);
            console_printf("vfs: ext2 mounted at /disk/\n");
        } else {
            console_set_color(0x0CU);
            console_printf("initramfs: no modules loaded\n");
        }
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
        u32 init_pid = proc_spawn("/init");

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
