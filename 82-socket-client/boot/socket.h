#ifndef SOCKET_H
#define SOCKET_H

#include "vfs.h"

#define AF_INET        2U
#define SOCK_STREAM    1U
#define SOCK_TYPE_MASK 0xFU
#define SOCK_CLOEXEC   0x80000U

#define SHUT_RD   0U
#define SHUT_WR   1U
#define SHUT_RDWR 2U

void socket_init(void);
int  socket_create(u32 domain, u32 type, u32 protocol, vfs_file_t **out);
int  socket_connect(vfs_file_t *f, const u8 *addr, u32 addrlen);
int  socket_shutdown(vfs_file_t *f, u32 how);
int  socket_getname(vfs_file_t *f, u8 *addr, u32 *addrlen, int peer);

#endif
