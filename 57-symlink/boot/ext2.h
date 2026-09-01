#ifndef EXT2_H
#define EXT2_H

#include "console.h"

int  ext2_init(void);
int  ext2_open(const char *path, u32 flags);
u32  ext2_read(int bfd, u8 *buf, u32 len, u32 pos);
u32  ext2_write(int bfd, const u8 *buf, u32 len, u32 pos);
u32  ext2_size(int bfd);
void ext2_close(int bfd);
u32  ext2_getdents(int bfd, u8 *buf, u32 len, u32 *pos);
u32  ext2_mode(int bfd);
int  ext2_mkdir(const char *path);
int  ext2_unlink(const char *path);
int  ext2_symlink(const char *target, const char *linkpath);
int  ext2_readlink(const char *linkpath, char *buf, u32 buf_max);
int  ext2_lstat(const char *path, u32 *out_mode, u32 *out_size);

#endif
