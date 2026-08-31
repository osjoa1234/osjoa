#ifndef ATA_H
#define ATA_H

#include "console.h"

#define ATA_SECTOR_SIZE 512U

void ata_init(void);
int  ata_read_sector(u32 lba, u8 *buf);
int  ata_write_sector(u32 lba, const u8 *buf);

#endif
