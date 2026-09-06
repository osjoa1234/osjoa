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

#define O_CREAT 0x40U

struct vfs_ops {
    int  (*open)(const char *path, u32 flags);
    u32  (*read)(int bfd, u8 *buf, u32 len, u32 pos);
    u32  (*write)(int bfd, const u8 *buf, u32 len, u32 pos);
    u32  (*size)(int bfd);
    void (*close)(int bfd);
    void (*dup)(int bfd);
    u32  (*getdents)(int bfd, u8 *buf, u32 len, u32 *pos);
    u32  (*mode)(int bfd);
    int  (*mkdir)(const char *path);
    int  (*unlink)(const char *path);
    int  (*symlink)(const char *target, const char *linkpath);
    int  (*readlink)(const char *linkpath, char *buf, u32 buf_max);
    int  (*lstat)(const char *path, u32 *out_mode, u32 *out_size);
};

void        vfs_init(void);
void        vfs_mount(const char *prefix, vfs_ops_t *ops);
vfs_file_t *vfs_open(const char *path, u32 flags);
vfs_file_t *vfs_dup(vfs_file_t *f);
u32         vfs_read(vfs_file_t *f, u8 *buf, u32 len);
u32         vfs_write(vfs_file_t *f, const u8 *buf, u32 len);
u32         vfs_seek(vfs_file_t *f, int offset, u32 whence);
void        vfs_close(vfs_file_t *f);
u32         vfs_getdents(vfs_file_t *f, u8 *buf, u32 len);
u32         vfs_mode(vfs_file_t *f);
u32         vfs_size(vfs_file_t *f);
int         vfs_mkdir(const char *path);
int         vfs_unlink(const char *path);
int         vfs_symlink(const char *target, const char *linkpath);
int         vfs_readlink(const char *linkpath, char *buf, u32 buf_max);
int         vfs_lstat(const char *path, u32 *out_mode, u32 *out_size);

#endif
