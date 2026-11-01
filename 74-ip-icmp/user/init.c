static unsigned int sys_write(unsigned int fd, const char *buf, unsigned long len)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(1L), "D"(fd), "S"(buf), "d"(len) : "rcx", "r11", "memory");
    return (unsigned int)ret;
}

static void sys_exit(unsigned int code)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(231L), "D"(code) : "rcx", "r11", "memory");
    (void)ret;
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

static void sys_exec(const char *name, char *const argv[], char *const envp[])
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(59L), "D"(name), "S"(argv), "d"(envp) : "rcx", "r11", "memory");
    (void)ret;
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

#define O_CREAT  0x40L
#define O_TRUNC  0x200L
#define O_APPEND 0x400L

static long sys_creat_trunc(const char *path)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(2L), "D"(path), "S"(O_CREAT | O_TRUNC), "d"(0644L) : "rcx", "r11", "memory");
    return ret;
}

static long sys_creat_append(const char *path)
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(2L), "D"(path), "S"(O_CREAT | O_APPEND), "d"(0644L) : "rcx", "r11", "memory");
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

static char **environ;

static char *getenv(const char *name)
{
    unsigned int nlen = slen(name);
    unsigned int i;

    for (i = 0U; environ[i]; i++) {
        unsigned int j = 0U;
        while (j < nlen && environ[i][j] == name[j]) j++;
        if (j == nlen && environ[i][j] == '=') return environ[i] + j + 1U;
    }
    return 0;
}

static void check_getenv(const char *name, const char *expected)
{
    char        *v  = getenv(name);
    unsigned int ok = (v && streq(v, expected)) ? 1U : 0U;

    writes("shell: getenv ");
    writes(name);
    writes(": ");
    writes(ok ? "OK\n" : "FAIL\n");
}

#define PATH_CAND_MAX 128U

static void exec_path(char *const argv[], char *const envp[])
{
    const char  *name = argv[0];
    unsigned int i;
    char        *path;
    const char  *p;
    char         cand[PATH_CAND_MAX];
    unsigned int ci;
    unsigned int j;

    for (i = 0U; name[i]; i++) {
        if (name[i] == '/') { sys_exec(name, argv, envp); return; }
    }

    path = getenv("PATH");
    if (!path) { sys_exec(name, argv, envp); return; }

    p = path;
    while (*p) {
        ci = 0U;
        while (*p && *p != ':' && ci < PATH_CAND_MAX - 1U) cand[ci++] = *p++;
        while (*p && *p != ':') p++;

        if (ci > 0U && cand[ci - 1U] != '/' && ci < PATH_CAND_MAX - 1U) cand[ci++] = '/';
        for (j = 0U; name[j] && ci < PATH_CAND_MAX - 1U; j++) cand[ci++] = name[j];
        cand[ci] = '\0';

        sys_exec(cand, argv, envp);

        if (*p == ':') p++;
    }
}

static void run_argv(char *argv[])
{
    unsigned int pid;
    unsigned int exit_code;

    pid = sys_fork();
    if (pid == 0U) {
        exec_path(argv, environ);
        writes("shell: not found\n");
        sys_exit(1U);
        for (;;) {}
    }

    exit_code = (unsigned int)-1U;
    sys_wait(pid, &exit_code);
}

static void run_argv_redirect(char *argv[], const char *redirect_path, unsigned int append)
{
    unsigned int pid;
    unsigned int exit_code;
    long         rfd;

    pid = sys_fork();
    if (pid == 0U) {
        rfd = append ? sys_creat_append(redirect_path) : sys_creat_trunc(redirect_path);
        if (rfd < 0) {
            writes("shell: cannot create file\n");
            sys_exit(1U);
            for (;;) {}
        }
        sys_dup2((unsigned int)rfd, 1U);
        sys_close((unsigned int)rfd);
        exec_path(argv, environ);
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
                                    char *stage_argv[][SHELL_ARGV_MAX + 1U],
                                    char *redirect_out[SHELL_STAGE_MAX],
                                    unsigned int redirect_append[SHELL_STAGE_MAX],
                                    char *redirect_in[SHELL_STAGE_MAX])
{
    unsigned int nstages = 0U;
    unsigned int scount  = 0U;
    unsigned int i;

    redirect_out[0]    = 0;
    redirect_append[0] = 0U;
    redirect_in[0]     = 0;

    for (i = 0U; i < argc; i++) {
        if (streq(argv[i], "|")) {
            if (scount == 0U || nstages + 1U >= SHELL_STAGE_MAX) return 0U;
            stage_argv[nstages][scount] = 0;
            nstages++;
            scount = 0U;
            redirect_out[nstages]    = 0;
            redirect_append[nstages] = 0U;
            redirect_in[nstages]     = 0;
            continue;
        }
        if (streq(argv[i], ">") || streq(argv[i], ">>")) {
            if (scount == 0U || i + 1U >= argc || redirect_out[nstages]) return 0U;
            redirect_out[nstages]    = argv[i + 1U];
            redirect_append[nstages] = streq(argv[i], ">>") ? 1U : 0U;
            i++;
            continue;
        }
        if (streq(argv[i], "<")) {
            if (scount == 0U || i + 1U >= argc || redirect_in[nstages]) return 0U;
            redirect_in[nstages] = argv[i + 1U];
            i++;
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

static void run_line(char *buf)
{
    char        *argv[SHELL_ARGV_MAX + 1U];
    char        *stage_argv[SHELL_STAGE_MAX][SHELL_ARGV_MAX + 1U];
    char        *redirect_out[SHELL_STAGE_MAX];
    unsigned int redirect_append[SHELL_STAGE_MAX];
    char        *redirect_in[SHELL_STAGE_MAX];
    int          pipefd[SHELL_STAGE_MAX - 1U][2];
    unsigned int pid[SHELL_STAGE_MAX];
    unsigned int argc;
    unsigned int nstages;
    unsigned int i;
    unsigned int j;
    unsigned int exit_code;

    argc = split_argv(buf, argv);
    if (argc == 0U) return;

    if (streq(argv[0], "exit")) {
        writes("shell: bye\n");
        sys_exit(0U);
        for (;;) {}
    }

    nstages = split_pipeline(argv, argc, stage_argv, redirect_out, redirect_append, redirect_in);
    if (nstages == 0U) {
        writes("shell: syntax error\n");
        return;
    }

    for (i = 0U; i + 1U < nstages; i++) {
        if (sys_pipe(pipefd[i]) != 0) {
            writes("shell: pipe failed\n");
            for (j = 0U; j < i; j++) {
                sys_close((unsigned int)pipefd[j][0]);
                sys_close((unsigned int)pipefd[j][1]);
            }
            return;
        }
    }

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
            if (redirect_in[i]) {
                long ifd = sys_open(redirect_in[i]);
                if (ifd < 0) {
                    writes("shell: cannot open file\n");
                    sys_exit(1U);
                    for (;;) {}
                }
                sys_dup2((unsigned int)ifd, 0U);
                sys_close((unsigned int)ifd);
            }
            if (redirect_out[i]) {
                long rfd = redirect_append[i] ? sys_creat_append(redirect_out[i]) : sys_creat_trunc(redirect_out[i]);
                if (rfd < 0) {
                    writes("shell: cannot create file\n");
                    sys_exit(1U);
                    for (;;) {}
                }
                sys_dup2((unsigned int)rfd, 1U);
                sys_close((unsigned int)rfd);
            }
            exec_path(stage_argv[i], environ);
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

__asm__(
    ".global _start\n"
    "_start:\n"
    "    mov %rsp, %rdi\n"
    "    and $-16, %rsp\n"
    "    call init_main\n"
);

void init_main(unsigned long *stack)
{
    long          stack_argc = (long)stack[0];
    char        **stack_argv = (char **)(stack + 1);
    char         buf[64];
    unsigned int n;
    long         fd;

    environ = stack_argv + stack_argc + 1;

    writes("shell: linux-abi ready\n");

    check_getenv("PATH", "/disk/bin:/");

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

    for (;;) {
        writes("$ ");
        n = sys_read(0U, buf, 63U);
        if (n == 0U) continue;
        buf[n] = '\0';
        if (n > 0U && buf[n - 1U] == '\n') { buf[n - 1U] = '\0'; n--; }
        if (n == 0U) continue;

        run_line(buf);
    }
}
