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

static int smatch(const char *str, const char *prefix, u32 *out_skip)
{
    u32 n = 0U;

    while (prefix[n] && str[n] == prefix[n]) n++;

    if (prefix[n] == '\0') {
        *out_skip = n;
        return 1;
    }

    if (prefix[n] == '/' && prefix[n + 1U] == '\0' && str[n] == '\0') {
        *out_skip = n;
        return 1;
    }

    return 0;
}

vfs_file_t *vfs_open(const char *path, u32 flags)
{
    u32 i;

    for (i = 0U; i < nmounts; i++) {
        const char *pfx = mounts[i].prefix;
        u32         skip;
        if (smatch(path, pfx, &skip)) {
            const char *rest = path + skip;
            int         bfd  = mounts[i].ops->open(rest[0] ? rest : "/", flags);
            if (bfd >= 0) {
                vfs_file_t *f = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
                if (!f) { mounts[i].ops->close(bfd); return 0; }
                f->ops        = mounts[i].ops;
                f->backend_fd = bfd;
                f->pos        = 0U;
                return f;
            }
        }
    }
    return 0;
}

vfs_file_t *vfs_dup(vfs_file_t *f)
{
    vfs_file_t *n = (vfs_file_t *)kmalloc(sizeof(vfs_file_t));
    if (!n) return 0;
    n->ops        = f->ops;
    n->backend_fd = f->backend_fd;
    n->pos        = f->pos;
    if (n->ops->dup) n->ops->dup(n->backend_fd);
    return n;
}

u32 vfs_read(vfs_file_t *f, u8 *buf, u32 len)
{
    u32 n = f->ops->read(f->backend_fd, buf, len, f->pos);
    f->pos += n;
    return n;
}

u32 vfs_write(vfs_file_t *f, const u8 *buf, u32 len)
{
    u32 n;
    if (!f->ops->write) return 0U;
    n = f->ops->write(f->backend_fd, buf, len, f->pos);
    f->pos += n;
    return n;
}

u32 vfs_seek(vfs_file_t *f, int offset, u32 whence)
{
    u32 newpos;
    u32 sz;

    switch (whence) {
    case SEEK_SET:
        newpos = (u32)offset;
        break;
    case SEEK_CUR:
        newpos = (u32)((int)f->pos + offset);
        break;
    case SEEK_END:
        if (!f->ops->size) return f->pos;
        sz     = f->ops->size(f->backend_fd);
        newpos = (u32)((int)sz + offset);
        break;
    default:
        return (u32)-1;
    }
    f->pos = newpos;
    return newpos;
}

void vfs_close(vfs_file_t *f)
{
    f->ops->close(f->backend_fd);
    kfree(f);
}

u32 vfs_getdents(vfs_file_t *f, u8 *buf, u32 len)
{
    if (!f->ops->getdents) return 0U;
    return f->ops->getdents(f->backend_fd, buf, len, &f->pos);
}

u32 vfs_mode(vfs_file_t *f)
{
    if (!f->ops->mode) return 0U;
    return f->ops->mode(f->backend_fd);
}

u32 vfs_size(vfs_file_t *f)
{
    if (!f->ops->size) return 0U;
    return f->ops->size(f->backend_fd);
}
