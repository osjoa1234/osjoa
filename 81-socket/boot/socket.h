#ifndef SOCKET_H
#define SOCKET_H

#include "vfs.h"

#define AF_INET        2U
#define SOCK_STREAM    1U
#define SOCK_TYPE_MASK 0xFU
#define SOCK_CLOEXEC   0x80000U

void socket_init(void);
int  socket_create(u32 domain, u32 type, u32 protocol, vfs_file_t **out);

#endif
