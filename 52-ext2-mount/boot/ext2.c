#include "ext2.h"
#include "ata.h"

#define EXT2_MAGIC          0xEF53U
#define EXT2_ROOT_INO       2U
#define EXT2_SB_LBA         2U
#define EXT2_MAX_BLOCK_SIZE 4096U

typedef struct __attribute__((packed)) {
    u32 s_inodes_count;
    u32 s_blocks_count;
    u32 s_r_blocks_count;
    u32 s_free_blocks_count;
    u32 s_free_inodes_count;
    u32 s_first_data_block;
    u32 s_log_block_size;
    u32 s_log_frag_size;
    u32 s_blocks_per_group;
    u32 s_frags_per_group;
    u32 s_inodes_per_group;
    u32 s_mtime;
    u32 s_wtime;
    u16 s_mnt_count;
    u16 s_max_mnt_count;
    u16 s_magic;
    u16 s_state;
    u16 s_errors;
    u16 s_minor_rev_level;
    u32 s_lastcheck;
    u32 s_checkinterval;
    u32 s_creator_os;
    u32 s_rev_level;
    u16 s_def_resuid;
    u16 s_def_resgid;
    u32 s_first_ino;
    u16 s_inode_size;
    u16 s_block_group_nr;
    u32 s_feature_compat;
    u32 s_feature_incompat;
    u32 s_feature_ro_compat;
} ext2_superblock_t;

typedef struct __attribute__((packed)) {
    u32 bg_block_bitmap;
    u32 bg_inode_bitmap;
    u32 bg_inode_table;
    u16 bg_free_blocks_count;
    u16 bg_free_inodes_count;
    u16 bg_used_dirs_count;
    u16 bg_pad;
    u8  bg_reserved[12];
} ext2_group_desc_t;

typedef struct __attribute__((packed)) {
    u16 i_mode;
    u16 i_uid;
    u32 i_size;
    u32 i_atime;
    u32 i_ctime;
    u32 i_mtime;
    u32 i_dtime;
    u16 i_gid;
    u16 i_links_count;
    u32 i_blocks;
    u32 i_flags;
    u32 i_osd1;
    u32 i_block[15];
    u32 i_generation;
    u32 i_file_acl;
    u32 i_size_high;
    u32 i_faddr;
    u8  i_osd2[12];
} ext2_inode_t;

typedef struct __attribute__((packed)) {
    u32 inode;
    u16 rec_len;
    u8  name_len;
    u8  file_type;
} ext2_dirent_t;

static const char *const ext2_file_type_names[] = {
    "UNKNOWN", "REG", "DIR", "CHR", "BLK", "FIFO", "SOCK", "SYMLINK"
};

static int ext2_read_block(u32 block_num, u32 block_size, u8 *buf)
{
    u32 sectors_per_block = block_size / ATA_SECTOR_SIZE;
    u32 lba = block_num * sectors_per_block;
    u32 i;

    for (i = 0U; i < sectors_per_block; i++) {
        if (ata_read_sector(lba + i, buf + i * ATA_SECTOR_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

static int ext2_read_inode(const ext2_superblock_t *sb, const ext2_group_desc_t *gd,
                            u32 inode_num, u32 block_size, ext2_inode_t *out)
{
    static u8 inode_block[EXT2_MAX_BLOCK_SIZE];
    u32 index = (inode_num - 1U) % sb->s_inodes_per_group;
    u32 block_off = (index * sb->s_inode_size) / block_size;
    u32 byte_off = (index * sb->s_inode_size) % block_size;

    if (ext2_read_block(gd->bg_inode_table + block_off, block_size, inode_block) != 0) {
        return -1;
    }

    *out = *(const ext2_inode_t *)(inode_block + byte_off);
    return 0;
}

static void ext2_print_dir_block(const u8 *block, u32 block_size)
{
    u32 pos = 0U;

    while (pos + sizeof(ext2_dirent_t) <= block_size) {
        const ext2_dirent_t *de = (const ext2_dirent_t *)(block + pos);
        const char *type_name;
        char namebuf[256];
        u32 i;

        if (de->rec_len == 0U) {
            break;
        }

        if (de->inode != 0U) {
            for (i = 0U; i < de->name_len && i < 255U; i++) {
                namebuf[i] = ((const char *)de)[sizeof(ext2_dirent_t) + i];
            }
            namebuf[i] = '\0';

            type_name = (de->file_type < 8U) ? ext2_file_type_names[de->file_type]
                                              : ext2_file_type_names[0];

            console_printf("ext2: root dir: inode=%u type=%s name=%s\n",
                           de->inode, type_name, namebuf);
        }

        pos += de->rec_len;
    }
}

int ext2_mount(void)
{
    static u8 sb_buf[1024];
    static u8 gd_buf[EXT2_MAX_BLOCK_SIZE];
    static u8 dir_buf[EXT2_MAX_BLOCK_SIZE];
    const ext2_superblock_t *sb;
    const ext2_group_desc_t *gd;
    ext2_inode_t root_inode;
    u32 block_size;
    u32 group_count;
    u32 gd_block;
    u32 dir_bytes_left;
    u32 i;

    if (ata_read_sector(EXT2_SB_LBA, sb_buf) != 0 ||
        ata_read_sector(EXT2_SB_LBA + 1U, sb_buf + ATA_SECTOR_SIZE) != 0) {
        console_set_color(0x0CU);
        console_printf("ext2: superblock read failed\n");
        return -1;
    }

    sb = (const ext2_superblock_t *)sb_buf;

    if (sb->s_magic != EXT2_MAGIC) {
        console_set_color(0x0CU);
        console_printf("ext2: bad magic=0x%04X (expected 0x%04X)\n", sb->s_magic, EXT2_MAGIC);
        return -1;
    }

    block_size = 1024U << sb->s_log_block_size;
    group_count = (sb->s_blocks_count + sb->s_blocks_per_group - 1U) / sb->s_blocks_per_group;

    console_set_color(0x0EU);
    console_printf("ext2: superblock magic=0x%04X rev=%u block_size=%u blocks=%u inodes=%u groups=%u\n",
                   sb->s_magic, sb->s_rev_level, block_size, sb->s_blocks_count,
                   sb->s_inodes_count, group_count);

    if (block_size > EXT2_MAX_BLOCK_SIZE) {
        console_set_color(0x0CU);
        console_printf("ext2: block_size=%u exceeds max supported=%u\n", block_size, EXT2_MAX_BLOCK_SIZE);
        return -1;
    }

    gd_block = sb->s_first_data_block + 1U;

    if (ext2_read_block(gd_block, block_size, gd_buf) != 0) {
        console_set_color(0x0CU);
        console_printf("ext2: group descriptor read failed\n");
        return -1;
    }

    gd = (const ext2_group_desc_t *)gd_buf;

    console_printf("ext2: group 0: inode_table=%u block_bitmap=%u inode_bitmap=%u free_blocks=%u free_inodes=%u\n",
                   gd->bg_inode_table, gd->bg_block_bitmap, gd->bg_inode_bitmap,
                   gd->bg_free_blocks_count, gd->bg_free_inodes_count);

    if (ext2_read_inode(sb, gd, EXT2_ROOT_INO, block_size, &root_inode) != 0) {
        console_set_color(0x0CU);
        console_printf("ext2: root inode read failed\n");
        return -1;
    }

    console_printf("ext2: root inode=%u mode=0x%04X size=%u blocks=%u\n",
                   EXT2_ROOT_INO, root_inode.i_mode, root_inode.i_size, root_inode.i_blocks);

    dir_bytes_left = root_inode.i_size;

    for (i = 0U; i < 12U && dir_bytes_left > 0U; i++) {
        if (root_inode.i_block[i] == 0U) {
            break;
        }

        if (ext2_read_block(root_inode.i_block[i], block_size, dir_buf) != 0) {
            console_set_color(0x0CU);
            console_printf("ext2: root dir block read failed\n");
            return -1;
        }

        ext2_print_dir_block(dir_buf, block_size);

        dir_bytes_left -= (dir_bytes_left < block_size) ? dir_bytes_left : block_size;
    }

    console_set_color(0x0AU);
    console_printf("ext2: mount OK\n");

    return 0;
}
