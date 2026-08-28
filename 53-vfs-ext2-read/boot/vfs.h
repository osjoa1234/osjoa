#ifndef VFS_H
#define VFS_H

#include "console.h"

#define VFS_MOUNT_MAX 4

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct vfs_ops vfs_ops_t;

typedef struct {
    vfs_ops_t *ops;
    int        backend_fd;
    u32        pos;
} vfs_file_t;

struct vfs_ops {
    int  (*open)(const char *path);
    u32  (*read)(int bfd, u8 *buf, u32 len, u32 pos);
    u32  (*write)(int bfd, const u8 *buf, u32 len, u32 pos);
    u32  (*size)(int bfd);
    void (*close)(int bfd);
    void (*dup)(int bfd);
};

void        vfs_init(void);
void        vfs_mount(const char *prefix, vfs_ops_t *ops);
vfs_file_t *vfs_open(const char *path);
vfs_file_t *vfs_dup(vfs_file_t *f);
u32         vfs_read(vfs_file_t *f, u8 *buf, u32 len);
u32         vfs_write(vfs_file_t *f, const u8 *buf, u32 len);
u32         vfs_seek(vfs_file_t *f, int offset, u32 whence);
void        vfs_close(vfs_file_t *f);

#endif
