#include "console_dev.h"
#include "console.h"
#include "keyboard.h"
#include "kheap.h"

static int  con_open(const char *path) { (void)path; return 0; }
static void con_close(int bfd)         { (void)bfd; }

static u32 con_read(int bfd, u8 *buf, u32 len)
{
    (void)bfd;
    u32 i;
    for (i = 0U; i < len; i++) {
        buf[i] = (u8)keyboard_getchar();
        console_putchar(buf[i]);
        if (buf[i] == '\n') { i++; break; }
    }
    return i;
}

static u32 con_write(int bfd, const u8 *buf, u32 len)
{
    u32 i;
    (void)bfd;
    for (i = 0U; i < len; i++) console_putchar(buf[i]);
    return len;
}

static vfs_ops_t console_ops = { con_open, con_read, con_write, con_close };

vfs_file_t *console_dev_open(void)
{
    vfs_file_t *f = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!f) return 0;
    f->ops        = &console_ops;
    f->backend_fd = 0;
    return f;
}
