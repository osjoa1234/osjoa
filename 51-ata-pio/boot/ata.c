#include "ata.h"

#define ATA_PRIMARY_DATA      0x1F0U
#define ATA_PRIMARY_SECCOUNT  0x1F2U
#define ATA_PRIMARY_LBA_LO    0x1F3U
#define ATA_PRIMARY_LBA_MID   0x1F4U
#define ATA_PRIMARY_LBA_HI    0x1F5U
#define ATA_PRIMARY_DRIVE     0x1F6U
#define ATA_PRIMARY_STATUS    0x1F7U
#define ATA_PRIMARY_COMMAND   0x1F7U
#define ATA_PRIMARY_CTRL      0x3F6U

#define ATA_SR_ERR  0x01U
#define ATA_SR_DRQ  0x08U
#define ATA_SR_DF   0x20U
#define ATA_SR_BSY  0x80U

#define ATA_CMD_READ_SECTORS  0x20U
#define ATA_CMD_WRITE_SECTORS 0x30U
#define ATA_CMD_CACHE_FLUSH   0xE7U

#define ATA_WAIT_SPINS 1000000U

static u8 inb(u16 port)
{
    u8 value;

    __asm__ volatile ("inb %1, %0" : "=a" (value) : "Nd" (port));
    return value;
}

static void outb(u16 port, u8 value)
{
    __asm__ volatile ("outb %0, %1" : : "a" (value), "Nd" (port));
}

static u16 inw(u16 port)
{
    u16 value;

    __asm__ volatile ("inw %1, %0" : "=a" (value) : "Nd" (port));
    return value;
}

static void outw(u16 port, u16 value)
{
    __asm__ volatile ("outw %0, %1" : : "a" (value), "Nd" (port));
}

static void ata_io_wait(void)
{
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
}

static int ata_wait_ready(void)
{
    u32 spins;

    spins = 0U;
    while (inb(ATA_PRIMARY_STATUS) & ATA_SR_BSY) {
        spins++;
        if (spins > ATA_WAIT_SPINS) {
            return -1;
        }
    }
    return 0;
}

static int ata_wait_drq(void)
{
    u8 status;
    u32 spins;

    spins = 0U;
    while (1) {
        status = inb(ATA_PRIMARY_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
        if (status & ATA_SR_DRQ) {
            return 0;
        }
        spins++;
        if (spins > ATA_WAIT_SPINS) {
            return -1;
        }
    }
}

static void ata_select_lba(u32 lba)
{
    outb(ATA_PRIMARY_DRIVE, 0xE0U | (u8)((lba >> 24U) & 0x0FU));
    ata_io_wait();
    outb(ATA_PRIMARY_SECCOUNT, 1U);
    outb(ATA_PRIMARY_LBA_LO, (u8)(lba & 0xFFU));
    outb(ATA_PRIMARY_LBA_MID, (u8)((lba >> 8U) & 0xFFU));
    outb(ATA_PRIMARY_LBA_HI, (u8)((lba >> 16U) & 0xFFU));
}

void ata_init(void)
{
    outb(ATA_PRIMARY_DRIVE, 0xE0U);
    ata_io_wait();
    console_set_color(0x0EU);
    console_printf("ata: primary master ready (0x1F0-0x1F7, ctrl=0x3F6)\n");
}

int ata_read_sector(u32 lba, u8 *buf)
{
    u32 i;

    if (ata_wait_ready() != 0) {
        return -1;
    }

    ata_select_lba(lba);
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_READ_SECTORS);

    if (ata_wait_ready() != 0 || ata_wait_drq() != 0) {
        return -1;
    }

    for (i = 0U; i < ATA_SECTOR_SIZE / 2U; i++) {
        u16 word = inw(ATA_PRIMARY_DATA);

        buf[i * 2U] = (u8)(word & 0xFFU);
        buf[i * 2U + 1U] = (u8)((word >> 8U) & 0xFFU);
    }

    return 0;
}

int ata_write_sector(u32 lba, const u8 *buf)
{
    u32 i;

    if (ata_wait_ready() != 0) {
        return -1;
    }

    ata_select_lba(lba);
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_WRITE_SECTORS);

    if (ata_wait_ready() != 0 || ata_wait_drq() != 0) {
        return -1;
    }

    for (i = 0U; i < ATA_SECTOR_SIZE / 2U; i++) {
        u16 word = (u16)buf[i * 2U] | ((u16)buf[i * 2U + 1U] << 8U);

        outw(ATA_PRIMARY_DATA, word);
    }

    outb(ATA_PRIMARY_COMMAND, ATA_CMD_CACHE_FLUSH);

    return ata_wait_ready();
}
