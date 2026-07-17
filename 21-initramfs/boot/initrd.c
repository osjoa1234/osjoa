#include "initrd.h"
#include "console.h"

struct initrd_file {
    const char *name;
    const u8   *data;
    u32         size;
    u32         pos;
};

static struct initrd_file files[INITRD_MAX_FILES];
static u32 nfiles;

static u32 hex8(const u8 *s)
{
    u32 val = 0U;
    u32 i;

    for (i = 0U; i < 8U; i++) {
        u8 c = s[i];
        u32 d;

        if (c >= '0' && c <= '9')      d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a') + 10U;
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A') + 10U;
        else                           d = 0U;

        val = (val << 4U) | d;
    }
    return val;
}

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

void initrd_init(u32 start, u32 end)
{
    const u8 *p = (const u8 *)start;

    nfiles = 0U;

    while ((u32)p + 110U <= end) {
        u32 namesize;
        u32 filesize;
        const char *name;
        u32 data_off;
        u32 next;

        if (p[0] != '0' || p[1] != '7' || p[2] != '0' ||
            p[3] != '7' || p[4] != '0') {
            break;
        }

        namesize = hex8(p + 94U);
        filesize = hex8(p + 54U);
        name     = (const char *)(p + 110U);

        if (namesize >= 9U &&
            name[0] == 'T' && name[1] == 'R' &&
            name[2] == 'A' && name[3] == 'I' &&
            name[4] == 'L' && name[5] == 'E' &&
            name[6] == 'R') {
            break;
        }

        data_off = (110U + namesize + 3U) & ~3U;

        if (filesize > 0U && nfiles < INITRD_MAX_FILES) {
            files[nfiles].name = name;
            files[nfiles].data = p + data_off;
            files[nfiles].size = filesize;
            files[nfiles].pos  = 0U;
            nfiles++;
        }

        next = data_off + ((filesize + 3U) & ~3U);
        p += next;
    }
}

int initrd_open(const char *name)
{
    u32 i;

    for (i = 0U; i < nfiles; i++) {
        const char *n = files[i].name;

        if (n[0] == '.' && n[1] == '/') n += 2;
        if (streq(n, name)) {
            files[i].pos = 0U;
            return (int)i;
        }
    }
    return -1;
}

u32 initrd_read(int fd, u8 *buf, u32 len)
{
    u32 avail;
    u32 i;

    if (fd < 0 || (u32)fd >= nfiles) return 0U;

    avail = files[fd].size - files[fd].pos;
    if (len > avail) len = avail;

    for (i = 0U; i < len; i++) {
        buf[i] = files[fd].data[files[fd].pos + i];
    }

    files[fd].pos += len;
    return len;
}

u32 initrd_file_count(void)
{
    return nfiles;
}
