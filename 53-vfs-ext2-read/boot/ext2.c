#include "ext2.h"
#include "ata.h"

#define EXT2_MAGIC          0xEF53U
#define EXT2_ROOT_INO       2U
#define EXT2_SB_LBA         2U
#define EXT2_MAX_BLOCK_SIZE 4096U
#define EXT2_MAX_OPEN       8U
#define EXT2_DIRECT_BLOCKS  12U
#define EXT2_MAX_NAME       255U
#define EXT2_S_IFMT         0xF000U
#define EXT2_S_IFDIR        0x4000U

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

typedef struct {
    int          used;
    ext2_inode_t inode;
} ext2_ofile_t;

static u8  g_sb_buf[1024];
static u8  g_gd_buf[EXT2_MAX_BLOCK_SIZE];
static const ext2_superblock_t *g_sb;
static const ext2_group_desc_t *g_gd;
static u32 g_block_size;
static int g_ready;

static ext2_ofile_t g_ofiles[EXT2_MAX_OPEN];

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

static int ext2_read_inode(u32 inode_num, ext2_inode_t *out)
{
    static u8 inode_block[EXT2_MAX_BLOCK_SIZE];
    u32 index;
    u32 block_off;
    u32 byte_off;

    index     = (inode_num - 1U) % g_sb->s_inodes_per_group;
    block_off = (index * g_sb->s_inode_size) / g_block_size;
    byte_off  = (index * g_sb->s_inode_size) % g_block_size;

    if (ext2_read_block(g_gd->bg_inode_table + block_off, g_block_size, inode_block) != 0) {
        return -1;
    }

    *out = *(const ext2_inode_t *)(inode_block + byte_off);
    return 0;
}

static int ext2_find_in_dir_block(const u8 *block, u32 block_size, const char *name, u32 *out_inode)
{
    u32 pos = 0U;

    while (pos + sizeof(ext2_dirent_t) <= block_size) {
        const ext2_dirent_t *de = (const ext2_dirent_t *)(block + pos);
        const char *entry_name;
        u32 i;

        if (de->rec_len == 0U) {
            break;
        }

        if (de->inode != 0U) {
            entry_name = (const char *)de + sizeof(ext2_dirent_t);

            for (i = 0U; i < de->name_len && name[i] != '\0'; i++) {
                if (entry_name[i] != name[i]) break;
            }

            if (i == de->name_len && name[i] == '\0') {
                *out_inode = de->inode;
                return 0;
            }
        }

        pos += de->rec_len;
    }
    return -1;
}

static int ext2_indirect_lookup(u32 block_num, u32 index, u32 *out)
{
    static u8 buf[EXT2_MAX_BLOCK_SIZE];
    const u32 *ptrs;

    if (block_num == 0U) {
        *out = 0U;
        return 0;
    }

    if (ext2_read_block(block_num, g_block_size, buf) != 0) {
        return -1;
    }

    ptrs = (const u32 *)buf;
    *out = ptrs[index];
    return 0;
}

static int ext2_resolve_block(const ext2_inode_t *inode, u32 logical, u32 *out_phys)
{
    u32 entries    = g_block_size / 4U;
    u32 single_max = EXT2_DIRECT_BLOCKS + entries;
    u32 double_max = single_max + entries * entries;
    u32 triple_max = double_max + entries * entries * entries;

    if (logical < EXT2_DIRECT_BLOCKS) {
        *out_phys = inode->i_block[logical];
        return 0;
    }

    if (logical < single_max) {
        u32 idx = logical - EXT2_DIRECT_BLOCKS;
        return ext2_indirect_lookup(inode->i_block[12], idx, out_phys);
    }

    if (logical < double_max) {
        u32 idx   = logical - single_max;
        u32 outer = idx / entries;
        u32 inner = idx % entries;
        u32 l1;

        if (ext2_indirect_lookup(inode->i_block[13], outer, &l1) != 0) return -1;
        return ext2_indirect_lookup(l1, inner, out_phys);
    }

    if (logical < triple_max) {
        u32 idx   = logical - double_max;
        u32 outer = idx / (entries * entries);
        u32 rem   = idx % (entries * entries);
        u32 mid   = rem / entries;
        u32 inner = rem % entries;
        u32 l1;
        u32 l2;

        if (ext2_indirect_lookup(inode->i_block[14], outer, &l1) != 0) return -1;
        if (ext2_indirect_lookup(l1, mid, &l2) != 0) return -1;
        return ext2_indirect_lookup(l2, inner, out_phys);
    }

    return -1;
}

#define EXT2_TEST_TRIPLE_L1      8188U
#define EXT2_TEST_TRIPLE_DATA    8191U
#define EXT2_TEST_TRIPLE_PATTERN "ext2 deep indirect block OK"

static void ext2_self_test_triple_indirect(void)
{
    static u8    buf[EXT2_MAX_BLOCK_SIZE];
    ext2_inode_t fake = {0};
    u32          entries;
    u32          logical;
    u32          phys = 0U;
    u32          i;
    int          ok;

    fake.i_block[14] = EXT2_TEST_TRIPLE_L1;

    entries = g_block_size / 4U;
    logical = EXT2_DIRECT_BLOCKS + entries + entries * entries;

    ok = (ext2_resolve_block(&fake, logical, &phys) == 0) && (phys == EXT2_TEST_TRIPLE_DATA);

    if (ok && ext2_read_block(phys, g_block_size, buf) == 0) {
        const char *pattern = EXT2_TEST_TRIPLE_PATTERN;

        for (i = 0U; pattern[i] != '\0'; i++) {
            if (buf[i] != (u8)pattern[i]) { ok = 0; break; }
        }
    } else {
        ok = 0;
    }

    console_set_color(ok ? 0x0AU : 0x0CU);
    console_printf("ext2: triple-indirect self-test %s (logical=%u phys=%u)\n",
                   ok ? "OK" : "FAIL", logical, phys);
}

static int ext2_scan_dir(const ext2_inode_t *dir_inode, const char *name, u32 *out_inode)
{
    static u8 dir_buf[EXT2_MAX_BLOCK_SIZE];
    u32 dir_bytes_left = dir_inode->i_size;
    u32 block_index = 0U;
    u32 phys_block;

    while (dir_bytes_left > 0U) {
        if (ext2_resolve_block(dir_inode, block_index, &phys_block) != 0) break;
        if (phys_block == 0U) break;

        if (ext2_read_block(phys_block, g_block_size, dir_buf) != 0) return -1;

        if (ext2_find_in_dir_block(dir_buf, g_block_size, name, out_inode) == 0) return 0;

        dir_bytes_left -= (dir_bytes_left < g_block_size) ? dir_bytes_left : g_block_size;
        block_index++;
    }
    return -1;
}

static int ext2_resolve_path(const char *path, u32 *out_inode)
{
    ext2_inode_t cur_inode;
    u32          cur_inode_num = EXT2_ROOT_INO;
    const char  *p = path;

    if (ext2_read_inode(cur_inode_num, &cur_inode) != 0) return -1;

    while (*p) {
        char seg[EXT2_MAX_NAME + 1U];
        u32  seg_len = 0U;
        u32  next_inode_num;

        while (*p == '/') p++;
        if (*p == '\0') break;

        while (*p && *p != '/' && seg_len < EXT2_MAX_NAME) {
            seg[seg_len++] = *p++;
        }
        seg[seg_len] = '\0';

        if ((cur_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

        if (ext2_scan_dir(&cur_inode, seg, &next_inode_num) != 0) return -1;

        cur_inode_num = next_inode_num;
        if (ext2_read_inode(cur_inode_num, &cur_inode) != 0) return -1;
    }

    *out_inode = cur_inode_num;
    return 0;
}

int ext2_init(void)
{
    u32 gd_block;

    g_ready = 0;

    if (ata_read_sector(EXT2_SB_LBA, g_sb_buf) != 0 ||
        ata_read_sector(EXT2_SB_LBA + 1U, g_sb_buf + ATA_SECTOR_SIZE) != 0) {
        console_set_color(0x0CU);
        console_printf("ext2: superblock read failed\n");
        return -1;
    }

    g_sb = (const ext2_superblock_t *)g_sb_buf;

    if (g_sb->s_magic != EXT2_MAGIC) {
        console_set_color(0x0CU);
        console_printf("ext2: bad magic=0x%04X (expected 0x%04X)\n", g_sb->s_magic, EXT2_MAGIC);
        return -1;
    }

    g_block_size = 1024U << g_sb->s_log_block_size;

    if (g_block_size > EXT2_MAX_BLOCK_SIZE) {
        console_set_color(0x0CU);
        console_printf("ext2: block_size=%u exceeds max supported=%u\n", g_block_size, EXT2_MAX_BLOCK_SIZE);
        return -1;
    }

    console_set_color(0x0EU);
    console_printf("ext2: superblock magic=0x%04X rev=%u block_size=%u blocks=%u inodes=%u\n",
                   g_sb->s_magic, g_sb->s_rev_level, g_block_size, g_sb->s_blocks_count, g_sb->s_inodes_count);

    gd_block = g_sb->s_first_data_block + 1U;

    if (ext2_read_block(gd_block, g_block_size, g_gd_buf) != 0) {
        console_set_color(0x0CU);
        console_printf("ext2: group descriptor read failed\n");
        return -1;
    }

    g_gd = (const ext2_group_desc_t *)g_gd_buf;

    console_printf("ext2: group 0: inode_table=%u block_bitmap=%u inode_bitmap=%u free_blocks=%u free_inodes=%u\n",
                   g_gd->bg_inode_table, g_gd->bg_block_bitmap, g_gd->bg_inode_bitmap,
                   g_gd->bg_free_blocks_count, g_gd->bg_free_inodes_count);

    g_ready = 1;

    ext2_self_test_triple_indirect();

    return 0;
}

int ext2_open(const char *path)
{
    u32 inode_num;
    u32 i;

    if (!g_ready) return -1;
    if (ext2_resolve_path(path, &inode_num) != 0) return -1;

    for (i = 0U; i < EXT2_MAX_OPEN; i++) {
        if (!g_ofiles[i].used) {
            if (ext2_read_inode(inode_num, &g_ofiles[i].inode) != 0) return -1;
            g_ofiles[i].used = 1;
            return (int)i;
        }
    }
    return -1;
}

u32 ext2_read(int bfd, u8 *buf, u32 len, u32 pos)
{
    static u8 block_buf[EXT2_MAX_BLOCK_SIZE];
    ext2_ofile_t *f;
    u32 file_size;
    u32 total;
    u32 block_index;
    u32 block_off;
    u32 chunk;
    u32 phys_block;
    u32 i;

    if (bfd < 0 || (u32)bfd >= EXT2_MAX_OPEN || !g_ofiles[bfd].used) return 0U;

    f = &g_ofiles[bfd];
    file_size = f->inode.i_size;

    if (pos >= file_size) return 0U;
    if (len > file_size - pos) len = file_size - pos;

    total = 0U;
    while (total < len) {
        block_index = (pos + total) / g_block_size;
        block_off   = (pos + total) % g_block_size;

        if (ext2_resolve_block(&f->inode, block_index, &phys_block) != 0) break;
        if (phys_block == 0U) break;

        if (ext2_read_block(phys_block, g_block_size, block_buf) != 0) break;

        chunk = g_block_size - block_off;
        if (chunk > len - total) chunk = len - total;

        for (i = 0U; i < chunk; i++) {
            buf[total + i] = block_buf[block_off + i];
        }

        total += chunk;
    }

    return total;
}

u32 ext2_size(int bfd)
{
    if (bfd < 0 || (u32)bfd >= EXT2_MAX_OPEN || !g_ofiles[bfd].used) return 0U;
    return g_ofiles[bfd].inode.i_size;
}

void ext2_close(int bfd)
{
    if (bfd < 0 || (u32)bfd >= EXT2_MAX_OPEN) return;
    g_ofiles[bfd].used = 0;
}
