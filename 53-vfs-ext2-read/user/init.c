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
