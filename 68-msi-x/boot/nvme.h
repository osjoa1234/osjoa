#ifndef NVME_H
#define NVME_H

#include "console.h"

#define NVME_SECTOR_SIZE 512U

void nvme_init(void);
int  nvme_read_sector(u32 lba, u8 *buf);
int  nvme_write_sector(u32 lba, const u8 *buf);

#endif
