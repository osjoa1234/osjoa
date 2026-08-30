#include "ext2.h"
#include "ata.h"
#include "vfs.h"

#define EXT2_MAGIC          0xEF53U
#define EXT2_ROOT_INO       2U
#define EXT2_SB_LBA         2U
#define EXT2_MAX_BLOCK_SIZE 4096U
#define EXT2_MAX_OPEN       8U
#define EXT2_DIRECT_BLOCKS  12U
#define EXT2_MAX_NAME       255U
#define EXT2_MAX_PATH       128U
#define EXT2_S_IFMT         0xF000U
#define EXT2_S_IFDIR        0x4000U
#define EXT2_S_IFREG        0x8000U
#define EXT2_DEFAULT_FMODE  0644U

#define EXT2_FT_REG_FILE    1U
#define EXT2_FT_DIR         2U
#define EXT2_FT_SYMLINK     7U

#define DT_UNKNOWN          0U
#define DT_DIR              4U
#define DT_REG              8U
#define DT_LNK              10U

#define DIRENT64_HDR_LEN    19U

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
    u32          inode_num;
    u32          refcount;
    ext2_inode_t inode;
} ext2_ofile_t;

typedef struct {
    ext2_superblock_t sb;
    ext2_group_desc_t gd;
    u32               block_size;
    int               ready;
    ext2_ofile_t      ofiles[EXT2_MAX_OPEN];
} ext2_fs_t;

static ext2_fs_t g_fs;

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

static int ext2_write_block(u32 block_num, u32 block_size, const u8 *buf)
{
    u32 sectors_per_block = block_size / ATA_SECTOR_SIZE;
    u32 lba = block_num * sectors_per_block;
    u32 i;

    for (i = 0U; i < sectors_per_block; i++) {
        if (ata_write_sector(lba + i, buf + i * ATA_SECTOR_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

static int ext2_zero_block(const ext2_fs_t *fs, u32 block_num)
{
    static u8 zero_buf[EXT2_MAX_BLOCK_SIZE];
    u32 i;

    for (i = 0U; i < fs->block_size; i++) zero_buf[i] = 0U;
    return ext2_write_block(block_num, fs->block_size, zero_buf);
}

static int ext2_read_inode(const ext2_fs_t *fs, u32 inode_num, ext2_inode_t *out)
{
    static u8 inode_block[EXT2_MAX_BLOCK_SIZE];
    u32 index;
    u32 block_off;
    u32 byte_off;

    index     = (inode_num - 1U) % fs->sb.s_inodes_per_group;
    block_off = (index * fs->sb.s_inode_size) / fs->block_size;
    byte_off  = (index * fs->sb.s_inode_size) % fs->block_size;

    if (ext2_read_block(fs->gd.bg_inode_table + block_off, fs->block_size, inode_block) != 0) {
        return -1;
    }

    *out = *(const ext2_inode_t *)(inode_block + byte_off);
    return 0;
}

static int ext2_write_inode(const ext2_fs_t *fs, u32 inode_num, const ext2_inode_t *in)
{
    static u8 inode_block[EXT2_MAX_BLOCK_SIZE];
    u32 index;
    u32 block_off;
    u32 byte_off;

    index     = (inode_num - 1U) % fs->sb.s_inodes_per_group;
    block_off = (index * fs->sb.s_inode_size) / fs->block_size;
    byte_off  = (index * fs->sb.s_inode_size) % fs->block_size;

    if (ext2_read_block(fs->gd.bg_inode_table + block_off, fs->block_size, inode_block) != 0) {
        return -1;
    }

    *(ext2_inode_t *)(inode_block + byte_off) = *in;

    return ext2_write_block(fs->gd.bg_inode_table + block_off, fs->block_size, inode_block);
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

static int ext2_indirect_lookup(const ext2_fs_t *fs, u32 block_num, u32 index, u32 *out)
{
    static u8 buf[EXT2_MAX_BLOCK_SIZE];
    const u32 *ptrs;

    if (block_num == 0U) {
        *out = 0U;
        return 0;
    }

    if (ext2_read_block(block_num, fs->block_size, buf) != 0) {
        return -1;
    }

    ptrs = (const u32 *)buf;
    *out = ptrs[index];
    return 0;
}

static int ext2_resolve_block(const ext2_fs_t *fs, const ext2_inode_t *inode, u32 logical, u32 *out_phys)
{
    u32 entries    = fs->block_size / 4U;
    u32 single_max = EXT2_DIRECT_BLOCKS + entries;
    u32 double_max = single_max + entries * entries;
    u32 triple_max = double_max + entries * entries * entries;

    if (logical < EXT2_DIRECT_BLOCKS) {
        *out_phys = inode->i_block[logical];
        return 0;
    }

    if (logical < single_max) {
        u32 idx = logical - EXT2_DIRECT_BLOCKS;
        return ext2_indirect_lookup(fs, inode->i_block[12], idx, out_phys);
    }

    if (logical < double_max) {
        u32 idx   = logical - single_max;
        u32 outer = idx / entries;
        u32 inner = idx % entries;
        u32 l1;

        if (ext2_indirect_lookup(fs, inode->i_block[13], outer, &l1) != 0) return -1;
        return ext2_indirect_lookup(fs, l1, inner, out_phys);
    }

    if (logical < triple_max) {
        u32 idx   = logical - double_max;
        u32 outer = idx / (entries * entries);
        u32 rem   = idx % (entries * entries);
        u32 mid   = rem / entries;
        u32 inner = rem % entries;
        u32 l1;
        u32 l2;

        if (ext2_indirect_lookup(fs, inode->i_block[14], outer, &l1) != 0) return -1;
        if (ext2_indirect_lookup(fs, l1, mid, &l2) != 0) return -1;
        return ext2_indirect_lookup(fs, l2, inner, out_phys);
    }

    return -1;
}

static int ext2_scan_dir(const ext2_fs_t *fs, const ext2_inode_t *dir_inode, const char *name, u32 *out_inode)
{
    static u8 dir_buf[EXT2_MAX_BLOCK_SIZE];
    u32 dir_bytes_left = dir_inode->i_size;
    u32 block_index = 0U;
    u32 phys_block;

    while (dir_bytes_left > 0U) {
        if (ext2_resolve_block(fs, dir_inode, block_index, &phys_block) != 0) break;
        if (phys_block == 0U) break;

        if (ext2_read_block(phys_block, fs->block_size, dir_buf) != 0) return -1;

        if (ext2_find_in_dir_block(dir_buf, fs->block_size, name, out_inode) == 0) return 0;

        dir_bytes_left -= (dir_bytes_left < fs->block_size) ? dir_bytes_left : fs->block_size;
        block_index++;
    }
    return -1;
}

static int ext2_resolve_path(const ext2_fs_t *fs, const char *path, u32 *out_inode)
{
    ext2_inode_t cur_inode;
    u32          cur_inode_num = EXT2_ROOT_INO;
    const char  *p = path;

    if (ext2_read_inode(fs, cur_inode_num, &cur_inode) != 0) return -1;

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

        if (ext2_scan_dir(fs, &cur_inode, seg, &next_inode_num) != 0) return -1;

        cur_inode_num = next_inode_num;
        if (ext2_read_inode(fs, cur_inode_num, &cur_inode) != 0) return -1;
    }

    *out_inode = cur_inode_num;
    return 0;
}

static int ext2_flush_sb(ext2_fs_t *fs)
{
    static u8 sb_buf[1024];

    *(ext2_superblock_t *)sb_buf = fs->sb;

    if (ata_write_sector(EXT2_SB_LBA, sb_buf) != 0) return -1;
    return ata_write_sector(EXT2_SB_LBA + 1U, sb_buf + ATA_SECTOR_SIZE);
}

static int ext2_flush_gd(ext2_fs_t *fs)
{
    static u8 gd_buf[EXT2_MAX_BLOCK_SIZE];
    u32 gd_block = fs->sb.s_first_data_block + 1U;

    if (ext2_read_block(gd_block, fs->block_size, gd_buf) != 0) return -1;
    *(ext2_group_desc_t *)gd_buf = fs->gd;
    return ext2_write_block(gd_block, fs->block_size, gd_buf);
}

static u32 ext2_bitmap_alloc(const ext2_fs_t *fs, u32 bitmap_block, u32 max_bits)
{
    static u8 buf[EXT2_MAX_BLOCK_SIZE];
    u32 i;

    if (ext2_read_block(bitmap_block, fs->block_size, buf) != 0) return 0U;

    for (i = 0U; i < max_bits; i++) {
        u32 byte_idx = i / 8U;
        u32 bit_idx  = i % 8U;

        if (!(buf[byte_idx] & (u8)(1U << bit_idx))) {
            buf[byte_idx] |= (u8)(1U << bit_idx);
            if (ext2_write_block(bitmap_block, fs->block_size, buf) != 0) return 0U;
            return i + 1U;
        }
    }
    return 0U;
}

static u32 ext2_alloc_block(ext2_fs_t *fs)
{
    u32 bit;
    u32 phys;

    if (fs->gd.bg_free_blocks_count == 0U) return 0U;

    bit = ext2_bitmap_alloc(fs, fs->gd.bg_block_bitmap, fs->sb.s_blocks_per_group);
    if (bit == 0U) return 0U;

    phys = fs->sb.s_first_data_block + (bit - 1U);

    fs->gd.bg_free_blocks_count--;
    fs->sb.s_free_blocks_count--;

    if (ext2_flush_gd(fs) != 0 || ext2_flush_sb(fs) != 0) return 0U;
    if (ext2_zero_block(fs, phys) != 0) return 0U;

    return phys;
}

static u32 ext2_alloc_inode(ext2_fs_t *fs)
{
    u32 inode_num;

    if (fs->gd.bg_free_inodes_count == 0U) return 0U;

    inode_num = ext2_bitmap_alloc(fs, fs->gd.bg_inode_bitmap, fs->sb.s_inodes_per_group);
    if (inode_num == 0U) return 0U;

    fs->gd.bg_free_inodes_count--;
    fs->sb.s_free_inodes_count--;

    if (ext2_flush_gd(fs) != 0 || ext2_flush_sb(fs) != 0) return 0U;

    return inode_num;
}

static int ext2_get_or_alloc_block(ext2_fs_t *fs, ext2_inode_t *inode, u32 logical, u32 *out_phys)
{
    u32 phys;

    if (logical >= EXT2_DIRECT_BLOCKS) return -1;

    if (inode->i_block[logical] != 0U) {
        *out_phys = inode->i_block[logical];
        return 0;
    }

    phys = ext2_alloc_block(fs);
    if (phys == 0U) return -1;

    inode->i_block[logical] = phys;
    inode->i_blocks += fs->block_size / ATA_SECTOR_SIZE;
    *out_phys = phys;
    return 0;
}

#define EXT2_DIRENT_HDR 8U

static u32 ext2_dirent_used(const ext2_dirent_t *de)
{
    return (EXT2_DIRENT_HDR + de->name_len + 3U) & ~3U;
}

static int ext2_insert_dirent_in_block(u8 *block, u32 block_size, const char *name, u32 name_len,
                                        u32 inode_num, u8 file_type)
{
    u32 needed = (EXT2_DIRENT_HDR + name_len + 3U) & ~3U;
    u32 pos = 0U;

    while (pos + EXT2_DIRENT_HDR <= block_size) {
        ext2_dirent_t *de = (ext2_dirent_t *)(block + pos);
        u32 used;
        u32 avail;

        if (de->rec_len == 0U) break;

        used  = (de->inode != 0U) ? ext2_dirent_used(de) : 0U;
        avail = de->rec_len - used;

        if (avail >= needed) {
            ext2_dirent_t *nde;
            char *nname;
            u32 i;

            if (used > 0U) {
                u32 new_rec_len = de->rec_len - used;
                de->rec_len = (u16)used;
                nde = (ext2_dirent_t *)(block + pos + used);
                nde->rec_len = (u16)new_rec_len;
            } else {
                nde = de;
            }

            nde->inode     = inode_num;
            nde->name_len  = (u8)name_len;
            nde->file_type = file_type;

            nname = (char *)nde + sizeof(ext2_dirent_t);
            for (i = 0U; i < name_len; i++) nname[i] = name[i];

            return 0;
        }

        pos += de->rec_len;
    }
    return -1;
}

static int ext2_dir_insert(ext2_fs_t *fs, ext2_inode_t *dir_inode, u32 dir_inode_num,
                            const char *name, u32 name_len, u32 inode_num, u8 file_type)
{
    static u8 dir_buf[EXT2_MAX_BLOCK_SIZE];
    u32 nblocks = (dir_inode->i_size + fs->block_size - 1U) / fs->block_size;
    u32 block_index;
    u32 phys_block;

    for (block_index = 0U; block_index < nblocks; block_index++) {
        if (ext2_resolve_block(fs, dir_inode, block_index, &phys_block) != 0) return -1;
        if (phys_block == 0U) continue;
        if (ext2_read_block(phys_block, fs->block_size, dir_buf) != 0) return -1;

        if (ext2_insert_dirent_in_block(dir_buf, fs->block_size, name, name_len, inode_num, file_type) == 0) {
            if (ext2_write_block(phys_block, fs->block_size, dir_buf) != 0) return -1;
            return ext2_write_inode(fs, dir_inode_num, dir_inode);
        }
    }

    if (ext2_get_or_alloc_block(fs, dir_inode, nblocks, &phys_block) != 0) return -1;
    dir_inode->i_size += fs->block_size;

    {
        ext2_dirent_t *root;
        u32 i;

        for (i = 0U; i < fs->block_size; i++) dir_buf[i] = 0U;
        root = (ext2_dirent_t *)dir_buf;
        root->inode     = 0U;
        root->rec_len   = (u16)fs->block_size;
        root->name_len  = 0U;
        root->file_type = 0U;

        if (ext2_insert_dirent_in_block(dir_buf, fs->block_size, name, name_len, inode_num, file_type) != 0) return -1;
        if (ext2_write_block(phys_block, fs->block_size, dir_buf) != 0) return -1;
    }

    return ext2_write_inode(fs, dir_inode_num, dir_inode);
}

static void ext2_split_parent(const char *path, char *dir_buf, u32 dir_buf_max, char *name_buf, u32 name_buf_max)
{
    u32 len = 0U;
    int last_slash = -1;
    u32 i;
    u32 j;

    while (path[len]) len++;
    for (i = 0U; i < len; i++) if (path[i] == '/') last_slash = (int)i;

    if (last_slash <= 0) {
        dir_buf[0] = '/';
        dir_buf[1] = '\0';
    } else {
        for (j = 0U; j < (u32)last_slash && j + 1U < dir_buf_max; j++) dir_buf[j] = path[j];
        dir_buf[j] = '\0';
    }

    {
        u32 name_start = (last_slash < 0) ? 0U : (u32)(last_slash + 1);
        u32 nlen = len - name_start;

        for (j = 0U; j < nlen && j + 1U < name_buf_max; j++) name_buf[j] = path[name_start + j];
        name_buf[j] = '\0';
    }
}

static u32 ext2_create_file(ext2_fs_t *fs, const char *path)
{
    char dir_path[EXT2_MAX_PATH];
    char name[EXT2_MAX_NAME + 1U];
    u32  name_len;
    u32  parent_inode_num;
    ext2_inode_t parent_inode;
    ext2_inode_t new_inode;
    u32  new_inode_num;
    u32  i;

    ext2_split_parent(path, dir_path, sizeof(dir_path), name, sizeof(name));

    name_len = 0U;
    while (name[name_len]) name_len++;
    if (name_len == 0U || name_len > EXT2_MAX_NAME) return 0U;

    if (ext2_resolve_path(fs, dir_path, &parent_inode_num) != 0) return 0U;
    if (ext2_read_inode(fs, parent_inode_num, &parent_inode) != 0) return 0U;
    if ((parent_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return 0U;

    new_inode_num = ext2_alloc_inode(fs);
    if (new_inode_num == 0U) return 0U;

    {
        u8 *raw = (u8 *)&new_inode;
        for (i = 0U; i < sizeof(new_inode); i++) raw[i] = 0U;
    }
    new_inode.i_mode        = (u16)(EXT2_S_IFREG | EXT2_DEFAULT_FMODE);
    new_inode.i_links_count = 1U;

    if (ext2_write_inode(fs, new_inode_num, &new_inode) != 0) return 0U;

    if (ext2_dir_insert(fs, &parent_inode, parent_inode_num, name, name_len,
                         new_inode_num, EXT2_FT_REG_FILE) != 0) {
        return 0U;
    }

    return new_inode_num;
}

int ext2_init(void)
{
    static u8   sb_buf[1024];
    static u8   gd_buf[EXT2_MAX_BLOCK_SIZE];
    ext2_fs_t  *fs = &g_fs;
    u32         gd_block;

    fs->ready = 0;

    if (ata_read_sector(EXT2_SB_LBA, sb_buf) != 0 ||
        ata_read_sector(EXT2_SB_LBA + 1U, sb_buf + ATA_SECTOR_SIZE) != 0) {
        console_set_color(0x0CU);
        console_printf("ext2: superblock read failed\n");
        return -1;
    }

    fs->sb = *(const ext2_superblock_t *)sb_buf;

    if (fs->sb.s_magic != EXT2_MAGIC) {
        console_set_color(0x0CU);
        console_printf("ext2: bad magic=0x%04X (expected 0x%04X)\n", fs->sb.s_magic, EXT2_MAGIC);
        return -1;
    }

    fs->block_size = 1024U << fs->sb.s_log_block_size;

    if (fs->block_size > EXT2_MAX_BLOCK_SIZE) {
        console_set_color(0x0CU);
        console_printf("ext2: block_size=%u exceeds max supported=%u\n", fs->block_size, EXT2_MAX_BLOCK_SIZE);
        return -1;
    }

    console_set_color(0x0EU);
    console_printf("ext2: superblock magic=0x%04X rev=%u block_size=%u blocks=%u inodes=%u\n",
                   fs->sb.s_magic, fs->sb.s_rev_level, fs->block_size, fs->sb.s_blocks_count, fs->sb.s_inodes_count);

    gd_block = fs->sb.s_first_data_block + 1U;

    if (ext2_read_block(gd_block, fs->block_size, gd_buf) != 0) {
        console_set_color(0x0CU);
        console_printf("ext2: group descriptor read failed\n");
        return -1;
    }

    fs->gd = *(const ext2_group_desc_t *)gd_buf;

    console_printf("ext2: group 0: inode_table=%u block_bitmap=%u inode_bitmap=%u free_blocks=%u free_inodes=%u\n",
                   fs->gd.bg_inode_table, fs->gd.bg_block_bitmap, fs->gd.bg_inode_bitmap,
                   fs->gd.bg_free_blocks_count, fs->gd.bg_free_inodes_count);

    fs->ready = 1;

    return 0;
}

int ext2_open(const char *path, u32 flags)
{
    ext2_fs_t *fs = &g_fs;
    u32        inode_num;
    u32        i;

    if (!fs->ready) return -1;

    if (ext2_resolve_path(fs, path, &inode_num) != 0) {
        if (!(flags & O_CREAT)) return -1;
        inode_num = ext2_create_file(fs, path);
        if (inode_num == 0U) return -1;
    }

    for (i = 0U; i < EXT2_MAX_OPEN; i++) {
        if (fs->ofiles[i].used && fs->ofiles[i].inode_num == inode_num) {
            fs->ofiles[i].refcount++;
            return (int)i;
        }
    }

    for (i = 0U; i < EXT2_MAX_OPEN; i++) {
        if (!fs->ofiles[i].used) {
            if (ext2_read_inode(fs, inode_num, &fs->ofiles[i].inode) != 0) return -1;
            fs->ofiles[i].used      = 1;
            fs->ofiles[i].inode_num = inode_num;
            fs->ofiles[i].refcount  = 1U;
            return (int)i;
        }
    }
    return -1;
}

u32 ext2_read(int bfd, u8 *buf, u32 len, u32 pos)
{
    static u8     block_buf[EXT2_MAX_BLOCK_SIZE];
    ext2_fs_t    *fs = &g_fs;
    ext2_ofile_t *f;
    u32 file_size;
    u32 total;
    u32 block_index;
    u32 block_off;
    u32 chunk;
    u32 phys_block;
    u32 i;

    if (bfd < 0 || (u32)bfd >= EXT2_MAX_OPEN || !fs->ofiles[bfd].used) return 0U;

    f = &fs->ofiles[bfd];
    file_size = f->inode.i_size;

    if (pos >= file_size) return 0U;
    if (len > file_size - pos) len = file_size - pos;

    total = 0U;
    while (total < len) {
        block_index = (pos + total) / fs->block_size;
        block_off   = (pos + total) % fs->block_size;

        if (ext2_resolve_block(fs, &f->inode, block_index, &phys_block) != 0) break;
        if (phys_block == 0U) break;

        if (ext2_read_block(phys_block, fs->block_size, block_buf) != 0) break;

        chunk = fs->block_size - block_off;
        if (chunk > len - total) chunk = len - total;

        for (i = 0U; i < chunk; i++) {
            buf[total + i] = block_buf[block_off + i];
        }

        total += chunk;
    }

    return total;
}

u32 ext2_write(int bfd, const u8 *buf, u32 len, u32 pos)
{
    static u8     block_buf[EXT2_MAX_BLOCK_SIZE];
    ext2_fs_t    *fs = &g_fs;
    ext2_ofile_t *f;
    u32 total;
    u32 block_index;
    u32 block_off;
    u32 chunk;
    u32 phys_block;
    u32 i;

    if (bfd < 0 || (u32)bfd >= EXT2_MAX_OPEN || !fs->ofiles[bfd].used) return 0U;

    f = &fs->ofiles[bfd];
    total = 0U;

    while (total < len) {
        block_index = (pos + total) / fs->block_size;
        block_off   = (pos + total) % fs->block_size;

        if (ext2_get_or_alloc_block(fs, &f->inode, block_index, &phys_block) != 0) break;
        if (ext2_read_block(phys_block, fs->block_size, block_buf) != 0) break;

        chunk = fs->block_size - block_off;
        if (chunk > len - total) chunk = len - total;

        for (i = 0U; i < chunk; i++) {
            block_buf[block_off + i] = buf[total + i];
        }

        if (ext2_write_block(phys_block, fs->block_size, block_buf) != 0) break;

        total += chunk;
    }

    if (pos + total > f->inode.i_size) f->inode.i_size = pos + total;

    ext2_write_inode(fs, f->inode_num, &f->inode);

    return total;
}

u32 ext2_size(int bfd)
{
    ext2_fs_t *fs = &g_fs;

    if (bfd < 0 || (u32)bfd >= EXT2_MAX_OPEN || !fs->ofiles[bfd].used) return 0U;
    return fs->ofiles[bfd].inode.i_size;
}

void ext2_close(int bfd)
{
    ext2_fs_t *fs = &g_fs;

    if (bfd < 0 || (u32)bfd >= EXT2_MAX_OPEN || !fs->ofiles[bfd].used) return;

    fs->ofiles[bfd].refcount--;
    if (fs->ofiles[bfd].refcount == 0U) fs->ofiles[bfd].used = 0;
}

static u8 ext2_dtype(u8 file_type)
{
    switch (file_type) {
    case EXT2_FT_REG_FILE: return DT_REG;
    case EXT2_FT_DIR:      return DT_DIR;
    case EXT2_FT_SYMLINK:  return DT_LNK;
    default:                return DT_UNKNOWN;
    }
}

u32 ext2_getdents(int bfd, u8 *buf, u32 len, u32 *pos)
{
    static u8     dir_buf[EXT2_MAX_BLOCK_SIZE];
    ext2_fs_t    *fs = &g_fs;
    ext2_ofile_t *f;
    u32           cur;
    u32           total = 0U;

    if (bfd < 0 || (u32)bfd >= EXT2_MAX_OPEN || !fs->ofiles[bfd].used) return 0U;

    f   = &fs->ofiles[bfd];
    cur = *pos;

    while (cur < f->inode.i_size) {
        u32 block_index = cur / fs->block_size;
        u32 block_off   = cur % fs->block_size;
        u32 phys_block;
        const ext2_dirent_t *de;
        const char *entry_name;
        u32 name_len;
        u32 reclen_out;
        u8 *out;
        u32 i;

        if (ext2_resolve_block(fs, &f->inode, block_index, &phys_block) != 0) break;
        if (phys_block == 0U) break;
        if (ext2_read_block(phys_block, fs->block_size, dir_buf) != 0) break;

        de = (const ext2_dirent_t *)(dir_buf + block_off);
        if (de->rec_len == 0U) break;

        if (de->inode == 0U) {
            cur += de->rec_len;
            continue;
        }

        name_len   = de->name_len;
        reclen_out = (DIRENT64_HDR_LEN + name_len + 1U + 7U) & ~7U;

        if (total + reclen_out > len) break;

        out = buf + total;
        *(u64 *)(out + 0)  = (u64)de->inode;
        *(u64 *)(out + 8)  = (u64)(cur + de->rec_len);
        *(u16 *)(out + 16) = (u16)reclen_out;
        out[18] = ext2_dtype(de->file_type);

        entry_name = (const char *)de + sizeof(ext2_dirent_t);
        for (i = 0U; i < name_len; i++) out[DIRENT64_HDR_LEN + i] = entry_name[i];
        out[DIRENT64_HDR_LEN + name_len] = 0U;

        total += reclen_out;
        cur   += de->rec_len;
    }

    *pos = cur;
    return total;
}

u32 ext2_mode(int bfd)
{
    ext2_fs_t *fs = &g_fs;

    if (bfd < 0 || (u32)bfd >= EXT2_MAX_OPEN || !fs->ofiles[bfd].used) return 0U;
    return fs->ofiles[bfd].inode.i_mode;
}
