#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

int main(int argc, char **argv)
{
    char *buf;
    char *big;
    struct utsname uts;

    printf("musl_hello: hello from real musl -- argc=%d argv[0]=%s\n", argc, argv[0]);

    buf = malloc(64);
    strcpy(buf, "musl_hello: brk malloc ok");
    printf("%s\n", buf);
    free(buf);

    big = malloc(200000);
    printf("musl_hello: mmap malloc ok ptr=%p\n", (void *)big);
    free(big);

    printf("musl_hello: getpid = %d\n", getpid());
    printf("musl_hello: getuid = %d\n", getuid());

    if (uname(&uts) == 0) {
        printf("musl_hello: uname sysname = %s\n", uts.sysname);
        printf("musl_hello: uname release = %s\n", uts.release);
    }

    return 0;
}
