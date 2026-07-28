#include "vfs.h"
#include "kheap.h"

struct mount_entry {
    const char *prefix;
    vfs_ops_t  *ops;
};

static struct mount_entry mounts[VFS_MOUNT_MAX];
static u32 nmounts;

void vfs_init(void)
{
    nmounts = 0U;
}

void vfs_mount(const char *prefix, vfs_ops_t *ops)
{
    if (nmounts < VFS_MOUNT_MAX) {
        mounts[nmounts].prefix = prefix;
        mounts[nmounts].ops    = ops;
        nmounts++;
    }
}

static u32 slen(const char *s)
{
    u32 n = 0U;
    while (s[n]) n++;
    return n;
}

static int smatch(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++; prefix++;
    }
    return 1;
}

vfs_file_t *vfs_open(const char *path)
{
    u32 i;

    for (i = 0U; i < nmounts; i++) {
        const char *pfx = mounts[i].prefix;
        if (smatch(path, pfx)) {
            int bfd = mounts[i].ops->open(path + slen(pfx));
            if (bfd >= 0) {
                vfs_file_t *f = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
                if (!f) { mounts[i].ops->close(bfd); return 0; }
                f->ops        = mounts[i].ops;
                f->backend_fd = bfd;
                return f;
            }
        }
    }
    return 0;
}

u32 vfs_read(vfs_file_t *f, u8 *buf, u32 len)
{
    return f->ops->read(f->backend_fd, buf, len);
}

void vfs_close(vfs_file_t *f)
{
    f->ops->close(f->backend_fd);
    kfree(f);
}
