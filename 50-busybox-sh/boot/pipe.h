#ifndef PIPE_H
#define PIPE_H

#include "vfs.h"

void pipe_init(void);
int  pipe_create(vfs_file_t **out_read, vfs_file_t **out_write);

#endif
