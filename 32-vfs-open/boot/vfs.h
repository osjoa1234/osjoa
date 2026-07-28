#ifndef VFS_H
#define VFS_H

#include "console.h"

#define VFS_MOUNT_MAX 4

typedef struct vfs_ops vfs_ops_t;

typedef struct {
    vfs_ops_t *ops;
    int        backend_fd;
} vfs_file_t;

struct vfs_ops {
    int  (*open)(const char *path);
    u32  (*read)(int bfd, u8 *buf, u32 len);
    void (*close)(int bfd);
};

void        vfs_init(void);
void        vfs_mount(const char *prefix, vfs_ops_t *ops);
vfs_file_t *vfs_open(const char *path);
u32         vfs_read(vfs_file_t *f, u8 *buf, u32 len);
void        vfs_close(vfs_file_t *f);

#endif
