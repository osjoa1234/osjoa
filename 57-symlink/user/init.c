static unsigned int sys_write(unsigned int fd, const char *buf, unsigned long len)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(1L), "D"(fd), "S"(buf), "d"(len) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static void sys_exit(unsigned int code)
{
    __asm__ volatile ("syscall" : : "a"(231L), "D"(code) : "rcx", "r11", "memory");
}

static unsigned int sys_read(unsigned int fd, char *buf, unsigned long len)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(0L), "D"(fd), "S"(buf), "d"(len) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static unsigned int sys_fork(void)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(57L) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static unsigned int sys_wait(unsigned int pid, unsigned int *code)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(61L), "D"(pid), "S"(code) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static void sys_exec(const char *name, char *const argv[])
{
    __asm__ volatile ("syscall" : : "a"(59L), "D"(name), "S"(argv) : "rcx", "r11", "memory");
}

static long sys_pipe(int *fds)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(22L), "D"(fds) : "rcx", "r11", "memory");
    return ret;
}

static long sys_dup2(unsigned int oldfd, unsigned int newfd)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(33L), "D"(oldfd), "S"(newfd) : "rcx", "r11", "memory");
    return ret;
}

static void sys_close(unsigned int fd)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(3L), "D"(fd) : "rcx", "r11", "memory");
    (void)ret;
}

static long sys_open(const char *path)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(2L), "D"(path), "S"(0L), "d"(0L) : "rcx", "r11", "memory");
    return ret;
}

#define O_CREAT 0x40L

static long sys_creat(const char *path)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(2L), "D"(path), "S"(O_CREAT), "d"(0644L) : "rcx", "r11", "memory");
    return ret;
}

static long sys_lseek(unsigned int fd, long offset, unsigned int whence)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(8L), "D"(fd), "S"(offset), "d"(whence) : "rcx", "r11", "memory");
    return ret;
}

static long sys_getdents64(unsigned int fd, char *buf, unsigned int count)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(217L), "D"(fd), "S"(buf), "d"(count) : "rcx", "r11", "memory");
    return ret;
}

static long sys_mkdir(const char *path)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(83L), "D"(path), "S"(0755L) : "rcx", "r11", "memory");
    return ret;
}

static long sys_unlink(const char *path)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(87L), "D"(path) : "rcx", "r11", "memory");
    return ret;
}

static long sys_symlink(const char *target, const char *linkpath)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(88L), "D"(target), "S"(linkpath) : "rcx", "r11", "memory");
    return ret;
}

static long sys_readlink(const char *path, char *buf, unsigned long bufsize)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(89L), "D"(path), "S"(buf), "d"(bufsize) : "rcx", "r11", "memory");
    return ret;
}

typedef struct {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned long st_nlink;
    unsigned int  st_mode;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned int  __pad0;
    unsigned long st_rdev;
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long __unused[3];
} stat_t;

static long sys_lstat(const char *path, stat_t *buf)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(6L), "D"(path), "S"(buf) : "rcx", "r11", "memory");
    return ret;
}

static unsigned int slen(const char *s)
{
    unsigned int n = 0U;
    while (s[n]) n++;
    return n;
}

static void writes(const char *s)
{
    sys_write(1U, s, slen(s));
}

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

#define U_SEEK_SET      0U

#define SINGLE_INDIRECT_PROBE_OFF 15460U
#define DOUBLE_INDIRECT_PROBE_OFF 286770U
#define INDIRECT_PROBE_LEN        32U

static void check_indirect_probe(const char *path, unsigned int off)
{
    char         buf[INDIRECT_PROBE_LEN];
    long         fd;
    unsigned int n;
    unsigned int i;
    unsigned int ok;

    fd = sys_open(path);
    if (fd < 0) {
        writes("shell: ext2 open ");
        writes(path);
        writes(" failed\n");
        return;
    }

    sys_lseek((unsigned int)fd, (long)off, U_SEEK_SET);
    n = sys_read((unsigned int)fd, buf, INDIRECT_PROBE_LEN);
    sys_close((unsigned int)fd);

    ok = (n == INDIRECT_PROBE_LEN) ? 1U : 0U;
    if (ok) {
        for (i = 0U; i < n; i++) {
            if ((unsigned char)buf[i] != (unsigned char)((off + i) % 256U)) { ok = 0U; break; }
        }
    }

    writes("shell: ext2 ");
    writes(path);
    writes(": ");
    writes(ok ? "content OK\n" : "content MISMATCH\n");
}

#define DUAL_OPEN_BUF_MAX 32U

static void check_dual_open(const char *path, const char *expected)
{
    char         buf1[DUAL_OPEN_BUF_MAX];
    char         buf2[DUAL_OPEN_BUF_MAX];
    long         fd1;
    long         fd2;
    unsigned int expected_len;
    unsigned int n;
    unsigned int ok;
    unsigned int i;

    expected_len = slen(expected);
    fd1 = sys_open(path);
    fd2 = sys_open(path);
    if (fd1 < 0 || fd2 < 0) {
        writes("shell: ext2 dual-open ");
        writes(path);
        writes(" failed\n");
        if (fd1 >= 0) sys_close((unsigned int)fd1);
        if (fd2 >= 0) sys_close((unsigned int)fd2);
        return;
    }

    n = sys_read((unsigned int)fd1, buf1, DUAL_OPEN_BUF_MAX);
    ok = (n == expected_len) ? 1U : 0U;

    n = sys_read((unsigned int)fd2, buf2, DUAL_OPEN_BUF_MAX);
    if (n != expected_len) ok = 0U;

    if (ok) {
        for (i = 0U; i < expected_len; i++) {
            if (buf1[i] != expected[i] || buf2[i] != expected[i]) { ok = 0U; break; }
        }
    }

    sys_close((unsigned int)fd1);

    sys_lseek((unsigned int)fd2, 0L, U_SEEK_SET);
    n = sys_read((unsigned int)fd2, buf2, DUAL_OPEN_BUF_MAX);
    if (n != expected_len) ok = 0U;
    else {
        for (i = 0U; i < expected_len; i++) {
            if (buf2[i] != expected[i]) { ok = 0U; break; }
        }
    }

    sys_close((unsigned int)fd2);

    writes("shell: ext2 dual-open ");
    writes(path);
    writes(": ");
    writes(ok ? "OK\n" : "MISMATCH\n");
}

#define GETDENTS_BUF_MAX 512U

static int dirent_has_name(const char *buf, unsigned int n, const char *want)
{
    unsigned int off = 0U;

    while (off < n) {
        unsigned short reclen = *(const unsigned short *)(buf + off + 16U);
        const char    *name   = buf + off + 19U;

        if (streq(name, want)) return 1;
        if (reclen == 0U) break;
        off += reclen;
    }
    return 0;
}

static void check_getdents(const char *path, const char *want)
{
    char buf[GETDENTS_BUF_MAX];
    long fd;
    long n;
    int  found;

    fd = sys_open(path);
    if (fd < 0) {
        writes("shell: ext2 getdents ");
        writes(path);
        writes(" open failed\n");
        return;
    }

    found = 0;
    for (;;) {
        n = sys_getdents64((unsigned int)fd, buf, GETDENTS_BUF_MAX);
        if (n <= 0) break;
        if (dirent_has_name(buf, (unsigned int)n, want)) { found = 1; break; }
    }

    sys_close((unsigned int)fd);

    writes("shell: ext2 getdents ");
    writes(path);
    writes(" ");
    writes(want);
    writes(": ");
    writes(found ? "found\n" : "MISSING\n");
}

static void check_getdents_absent(const char *path, const char *want)
{
    char buf[GETDENTS_BUF_MAX];
    long fd;
    long n;
    int  found;

    fd = sys_open(path);
    if (fd < 0) {
        writes("shell: ext2 getdents ");
        writes(path);
        writes(" open failed\n");
        return;
    }

    found = 0;
    for (;;) {
        n = sys_getdents64((unsigned int)fd, buf, GETDENTS_BUF_MAX);
        if (n <= 0) break;
        if (dirent_has_name(buf, (unsigned int)n, want)) { found = 1; break; }
    }

    sys_close((unsigned int)fd);

    writes("shell: ext2 getdents-absent ");
    writes(path);
    writes(" ");
    writes(want);
    writes(": ");
    writes(!found ? "OK\n" : "STILL PRESENT\n");
}

static void check_mkdir(const char *path)
{
    long ret = sys_mkdir(path);

    writes("shell: ext2 mkdir ");
    writes(path);
    writes(": ");
    writes(ret == 0 ? "OK\n" : "FAIL\n");
}

static void check_unlink(const char *path)
{
    long ret = sys_unlink(path);

    writes("shell: ext2 unlink ");
    writes(path);
    writes(": ");
    writes(ret == 0 ? "OK\n" : "FAIL\n");
}

static void check_symlink(const char *target, const char *linkpath)
{
    long ret = sys_symlink(target, linkpath);

    writes("shell: ext2 symlink ");
    writes(linkpath);
    writes(" -> ");
    writes(target);
    writes(": ");
    writes(ret == 0 ? "OK\n" : "FAIL\n");
}

#define READLINK_BUF_MAX 128U

static void check_readlink(const char *path, const char *expected_target)
{
    char         buf[READLINK_BUF_MAX];
    long         n;
    unsigned int explen;
    unsigned int ok;
    unsigned int i;

    n      = sys_readlink(path, buf, READLINK_BUF_MAX);
    explen = slen(expected_target);
    ok     = (n == (long)explen) ? 1U : 0U;

    if (ok) {
        for (i = 0U; i < explen; i++) {
            if (buf[i] != expected_target[i]) { ok = 0U; break; }
        }
    }

    writes("shell: ext2 readlink ");
    writes(path);
    writes(": ");
    writes(ok ? "OK\n" : "MISMATCH\n");
}

static void check_read_via_symlink(const char *path, const char *expected)
{
    char         buf[64];
    long         fd;
    unsigned int len;
    unsigned int n;
    unsigned int ok;
    unsigned int i;

    len = slen(expected);
    fd  = sys_open(path);
    if (fd < 0) {
        writes("shell: ext2 read-via-symlink ");
        writes(path);
        writes(" open failed\n");
        return;
    }

    n = sys_read((unsigned int)fd, buf, sizeof(buf) - 1U);
    sys_close((unsigned int)fd);

    ok = (n == len) ? 1U : 0U;
    if (ok) {
        for (i = 0U; i < len; i++) {
            if (buf[i] != expected[i]) { ok = 0U; break; }
        }
    }

    writes("shell: ext2 read-via-symlink ");
    writes(path);
    writes(": ");
    writes(ok ? "OK\n" : "MISMATCH\n");
}

#define S_IFMT  0xF000U
#define S_IFLNK 0xA000U

static void check_lstat_is_link(const char *path)
{
    stat_t       st;
    long         ret;
    unsigned int ok;

    ret = sys_lstat(path, &st);
    ok  = (ret == 0 && (st.st_mode & S_IFMT) == S_IFLNK) ? 1U : 0U;

    writes("shell: ext2 lstat-is-link ");
    writes(path);
    writes(": ");
    writes(ok ? "OK\n" : "FAIL\n");
}

static void check_open_fails(const char *path, const char *label)
{
    long fd = sys_open(path);

    writes("shell: ext2 ");
    writes(label);
    writes(" ");
    writes(path);
    writes(": ");
    writes(fd < 0 ? "OK\n" : "UNEXPECTED SUCCESS\n");
    if (fd >= 0) sys_close((unsigned int)fd);
}

static void check_write_create(const char *path, const char *content)
{
    char         buf[64];
    long         fd;
    unsigned int len;
    unsigned int n;
    unsigned int ok;
    unsigned int i;

    len = slen(content);

    fd = sys_creat(path);
    if (fd < 0) {
        writes("shell: ext2 write-create ");
        writes(path);
        writes(" create failed\n");
        return;
    }

    n  = sys_write((unsigned int)fd, content, len);
    sys_close((unsigned int)fd);
    ok = (n == len) ? 1U : 0U;

    fd = sys_open(path);
    if (fd < 0) {
        ok = 0U;
    } else {
        n = sys_read((unsigned int)fd, buf, sizeof(buf) - 1U);
        sys_close((unsigned int)fd);
        if (n != len) {
            ok = 0U;
        } else {
            for (i = 0U; i < len; i++) {
                if (buf[i] != content[i]) { ok = 0U; break; }
            }
        }
    }

    writes("shell: ext2 write-create ");
    writes(path);
    writes(": ");
    writes(ok ? "OK\n" : "MISMATCH\n");
}

#define LONG_SYMLINK_TARGET \
    "0123456789/0123456789/0123456789/0123456789/0123456789/0123456789/"

#define WRITE_EXTEND_LEN 2500U

static void check_write_extend(const char *path, unsigned int len)
{
    static char  wbuf[WRITE_EXTEND_LEN];
    static char  rbuf[WRITE_EXTEND_LEN];
    long         fd;
    unsigned int n;
    unsigned int ok;
    unsigned int i;

    for (i = 0U; i < len; i++) wbuf[i] = (char)(i % 256U);

    fd = sys_creat(path);
    if (fd < 0) {
        writes("shell: ext2 write-extend ");
        writes(path);
        writes(" create failed\n");
        return;
    }

    n  = sys_write((unsigned int)fd, wbuf, len);
    sys_close((unsigned int)fd);
    ok = (n == len) ? 1U : 0U;

    fd = sys_open(path);
    if (fd < 0) {
        ok = 0U;
    } else {
        n = sys_read((unsigned int)fd, rbuf, len);
        sys_close((unsigned int)fd);
        if (n != len) {
            ok = 0U;
        } else {
            for (i = 0U; i < len; i++) {
                if ((unsigned char)rbuf[i] != (unsigned char)(i % 256U)) { ok = 0U; break; }
            }
        }
    }

    writes("shell: ext2 write-extend ");
    writes(path);
    writes(": ");
    writes(ok ? "OK\n" : "MISMATCH\n");
}

static void run_argv(char *argv[])
{
    unsigned int pid;
    unsigned int exit_code;

    pid = sys_fork();
    if (pid == 0U) {
        sys_exec(argv[0], argv);
        writes("shell: not found\n");
        sys_exit(1U);
        for (;;) {}
    }

    exit_code = (unsigned int)-1U;
    sys_wait(pid, &exit_code);
}

#define SHELL_ARGV_MAX  8U
#define SHELL_STAGE_MAX 3U

static unsigned int split_argv(char *buf, char *argv[])
{
    unsigned int argc = 0U;
    char        *p    = buf;

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (argc < SHELL_ARGV_MAX) argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
    }
    argv[argc] = 0;
    return argc;
}

static unsigned int split_pipeline(char *argv[], unsigned int argc,
                                    char *stage_argv[][SHELL_ARGV_MAX + 1U])
{
    unsigned int nstages = 0U;
    unsigned int scount  = 0U;
    unsigned int i;

    for (i = 0U; i < argc; i++) {
        if (streq(argv[i], "|")) {
            if (scount == 0U || nstages + 1U >= SHELL_STAGE_MAX) return 0U;
            stage_argv[nstages][scount] = 0;
            nstages++;
            scount = 0U;
            continue;
        }
        if (scount >= SHELL_ARGV_MAX) return 0U;
        stage_argv[nstages][scount++] = argv[i];
    }
    if (scount == 0U) return 0U;
    stage_argv[nstages][scount] = 0;
    nstages++;
    return nstages;
}

void _start(void)
{
    char         buf[64];
    char         mb_buf[2600];
    char        *argv[SHELL_ARGV_MAX + 1U];
    char        *stage_argv[SHELL_STAGE_MAX][SHELL_ARGV_MAX + 1U];
    int          pipefd[SHELL_STAGE_MAX - 1U][2];
    unsigned int pid[SHELL_STAGE_MAX];
    unsigned int argc;
    unsigned int nstages;
    unsigned int n;
    unsigned int i;
    unsigned int j;
    unsigned int exit_code;
    unsigned int ok;
    long         fd;

    writes("shell: linux-abi ready\n");

    fd = sys_open("/disk/hello.txt");
    if (fd < 0) {
        writes("shell: ext2 open /disk/hello.txt failed\n");
    } else {
        n = sys_read((unsigned int)fd, buf, sizeof(buf) - 1U);
        buf[n] = '\0';
        sys_close((unsigned int)fd);
        writes("shell: ext2 /disk/hello.txt: ");
        writes(buf);
    }

    fd = sys_open("/disk/multiblock.txt");
    if (fd < 0) {
        writes("shell: ext2 open /disk/multiblock.txt failed\n");
    } else {
        n = sys_read((unsigned int)fd, mb_buf, sizeof(mb_buf));
        sys_close((unsigned int)fd);

        ok = (n == sizeof(mb_buf)) ? 1U : 0U;
        if (ok) {
            for (i = 0U; i < n; i++) {
                if ((unsigned char)mb_buf[i] != (unsigned char)('0' + (i % 10U))) { ok = 0U; break; }
            }
        }

        writes("shell: ext2 /disk/multiblock.txt: ");
        writes(ok ? "content OK\n" : "content MISMATCH\n");
    }

    check_indirect_probe("/disk/singleindirect.txt", SINGLE_INDIRECT_PROBE_OFF);
    check_indirect_probe("/disk/doubleindirect.txt", DOUBLE_INDIRECT_PROBE_OFF);

    check_dual_open("/disk/hello.txt", "hello ext2 root fs\n");

    fd = sys_open("/disk/sub/nested.txt");
    if (fd < 0) {
        writes("shell: ext2 open /disk/sub/nested.txt failed\n");
    } else {
        n = sys_read((unsigned int)fd, buf, sizeof(buf) - 1U);
        buf[n] = '\0';
        sys_close((unsigned int)fd);
        writes("shell: ext2 /disk/sub/nested.txt: ");
        writes(buf);
    }

    check_getdents("/disk/", "hello.txt");
    check_getdents("/disk/", "sub");
    check_getdents("/disk/sub", "nested.txt");

    check_write_create("/disk/written.txt", "hello ext2 write\n");
    check_getdents("/disk/", "written.txt");

    check_write_extend("/disk/multiwrite.txt", WRITE_EXTEND_LEN);
    check_getdents("/disk/", "multiwrite.txt");

    check_write_create("/disk/sub/child.txt", "nested write ok\n");
    check_getdents("/disk/sub", "child.txt");

    {
        static char argv0[] = "busybox";
        static char argv1[] = "touch";
        static char argv2[] = "/disk/touched.txt";
        char *touch_argv[4];

        touch_argv[0] = argv0;
        touch_argv[1] = argv1;
        touch_argv[2] = argv2;
        touch_argv[3] = 0;

        writes("shell: busybox touch /disk/touched.txt:\n");
        run_argv(touch_argv);
    }
    check_getdents("/disk/", "touched.txt");

    check_mkdir("/disk/newdir");
    check_getdents("/disk/", "newdir");
    check_write_create("/disk/newdir/inner.txt", "mkdir write ok\n");
    check_getdents("/disk/newdir", "inner.txt");

    check_write_create("/disk/tounlink.txt", "delete me\n");
    check_getdents("/disk/", "tounlink.txt");
    check_unlink("/disk/tounlink.txt");
    check_getdents_absent("/disk/", "tounlink.txt");

    check_symlink("hello.txt", "/disk/hello_link");
    check_getdents("/disk/", "hello_link");
    check_readlink("/disk/hello_link", "hello.txt");
    check_read_via_symlink("/disk/hello_link", "hello ext2 root fs\n");

    check_symlink("/hello.txt", "/disk/abs_link");
    check_readlink("/disk/abs_link", "/hello.txt");
    check_read_via_symlink("/disk/abs_link", "hello ext2 root fs\n");

    check_symlink("sub", "/disk/sub_link");
    check_read_via_symlink("/disk/sub_link/nested.txt", "hello nested dir\n");

    check_symlink("hello_link", "/disk/chain_link");
    check_readlink("/disk/chain_link", "hello_link");
    check_read_via_symlink("/disk/chain_link", "hello ext2 root fs\n");

    check_symlink("does_not_exist.txt", "/disk/dangling_link");
    check_readlink("/disk/dangling_link", "does_not_exist.txt");
    check_open_fails("/disk/dangling_link", "open-dangling-symlink");
    check_lstat_is_link("/disk/dangling_link");

    check_symlink("loopb", "/disk/loopa");
    check_symlink("loopa", "/disk/loopb");
    check_open_fails("/disk/loopa", "open-symlink-loop");
    check_lstat_is_link("/disk/loopa");

    check_symlink(LONG_SYMLINK_TARGET, "/disk/long_link");
    check_readlink("/disk/long_link", LONG_SYMLINK_TARGET);

    {
        static char argv0[] = "busybox";
        static char argv1[] = "mkdir";
        static char argv2[] = "/disk/bbdir";
        char *mkdir_argv[4];

        mkdir_argv[0] = argv0;
        mkdir_argv[1] = argv1;
        mkdir_argv[2] = argv2;
        mkdir_argv[3] = 0;

        writes("shell: busybox mkdir /disk/bbdir:\n");
        run_argv(mkdir_argv);
    }
    check_getdents("/disk/", "bbdir");

    {
        static char argv0[] = "busybox";
        static char argv1[] = "rm";
        static char argv2[] = "/disk/touched.txt";
        char *rm_argv[4];

        rm_argv[0] = argv0;
        rm_argv[1] = argv1;
        rm_argv[2] = argv2;
        rm_argv[3] = 0;

        writes("shell: busybox rm /disk/touched.txt:\n");
        run_argv(rm_argv);
    }
    check_getdents_absent("/disk/", "touched.txt");

    {
        static char argv0[] = "busybox";
        static char argv1[] = "ls";
        static char argv2[] = "/disk";
        char *ls_argv[4];

        ls_argv[0] = argv0;
        ls_argv[1] = argv1;
        ls_argv[2] = argv2;
        ls_argv[3] = 0;

        writes("shell: busybox ls /disk:\n");
        run_argv(ls_argv);
    }

    for (;;) {
        writes("$ ");
        n = sys_read(0U, buf, 63U);
        if (n == 0U) continue;
        buf[n] = '\0';
        if (n > 0U && buf[n - 1U] == '\n') { buf[n - 1U] = '\0'; n--; }
        if (n == 0U) continue;

        argc = split_argv(buf, argv);
        if (argc == 0U) continue;

        if (streq(argv[0], "exit")) {
            writes("shell: bye\n");
            sys_exit(0U);
            for (;;) {}
        }

        nstages = split_pipeline(argv, argc, stage_argv);
        if (nstages == 0U) {
            writes("shell: syntax error\n");
            continue;
        }

        for (i = 0U; i + 1U < nstages; i++) {
            if (sys_pipe(pipefd[i]) != 0) {
                writes("shell: pipe failed\n");
                for (j = 0U; j < i; j++) {
                    sys_close((unsigned int)pipefd[j][0]);
                    sys_close((unsigned int)pipefd[j][1]);
                }
                nstages = 0U;
                break;
            }
        }
        if (nstages == 0U) continue;

        for (i = 0U; i < nstages; i++) {
            pid[i] = sys_fork();
            if (pid[i] == 0U) {
                if (i > 0U)
                    sys_dup2((unsigned int)pipefd[i - 1U][0], 0U);
                if (i + 1U < nstages)
                    sys_dup2((unsigned int)pipefd[i][1], 1U);
                for (j = 0U; j + 1U < nstages; j++) {
                    sys_close((unsigned int)pipefd[j][0]);
                    sys_close((unsigned int)pipefd[j][1]);
                }
                sys_exec(stage_argv[i][0], stage_argv[i]);
                writes("shell: not found\n");
                sys_exit(1U);
                for (;;) {}
            }
        }

        for (i = 0U; i + 1U < nstages; i++) {
            sys_close((unsigned int)pipefd[i][0]);
            sys_close((unsigned int)pipefd[i][1]);
        }

        for (i = 0U; i < nstages; i++) {
            exit_code = (unsigned int)-1U;
            sys_wait(pid[i], &exit_code);
        }
    }
}
