#include "console_dev.h"
#include "console.h"
#include "keyboard.h"
#include "kheap.h"

static int  con_open(const char *path, u32 flags) { (void)path; (void)flags; return 0; }
static void con_close(int bfd)         { (void)bfd; }

static u32 con_read(int bfd, u8 *buf, u32 len, u32 pos)
{
    (void)bfd;
    (void)pos;
    u32 i = 0U;
    while (i < len) {
        u8 c = (u8)keyboard_getchar();
        if (c == '\b') {
            if (i > 0U) {
                i--;
                console_putchar('\b');
                console_putchar(' ');
                console_putchar('\b');
            }
            continue;
        }
        buf[i++] = c;
        console_putchar(c);
        if (c == '\n') break;
    }
    return i;
}

static u32 con_write(int bfd, const u8 *buf, u32 len, u32 pos)
{
    u32 i;
    (void)bfd;
    (void)pos;
    for (i = 0U; i < len; i++) console_putchar(buf[i]);
    return len;
}

#define CONSOLE_MODE (0020000U | 0666U)

static u32 con_mode(int bfd) { (void)bfd; return CONSOLE_MODE; }

static vfs_ops_t console_ops = { con_open, con_read, con_write, 0, con_close, 0, 0, con_mode };

vfs_file_t *console_dev_open(void)
{
    vfs_file_t *f = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!f) return 0;
    f->ops        = &console_ops;
    f->backend_fd = 0;
    f->pos        = 0U;
    return f;
}
