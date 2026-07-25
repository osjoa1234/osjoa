#ifndef INITRD_H
#define INITRD_H

#include "console.h"

#define INITRD_MAX_FILES 16

void       initrd_init(u32 start, u32 end);
int        initrd_open(const char *name);
u32        initrd_read(int fd, u8 *buf, u32 len);
u32        initrd_file_count(void);
const u8  *initrd_data(int fd);
u32        initrd_size(int fd);

#endif
