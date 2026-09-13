# 62 — fcntl

**목표**: `fcntl` syscall(`F_DUPFD`/`F_DUPFD_CLOEXEC`/`F_GETFD`/`F_SETFD`)을 구현하고, `proc_exec`의 exec-time fd 정리를 "3번 이상은 무조건 닫는다"는 하드코딩에서 실제 리눅스처럼 **`FD_CLOEXEC` 플래그를 보는 방식**으로 바꾼다.

**60/61에서 이어짐, 원래는 "redirect-in"이었다**: 이 단계는 원래 셸에 `<`(입력 리다이렉션)을 추가하는 `62-redirect-in`으로 시작했다. `user/init.c`의 `split_pipeline`에 `<` 토큰 파싱을 추가하고 fork 자식에서 `sys_open`+`dup2(fd,0)`을 꽂는 작업 자체는 기존 `redirect_out`/`redirect_append`를 그대로 복사하는 수준이라 커널에 새로 구현할 게 없었다(이 부분 코드는 그대로 남아있다 — 아래 "1) 커스텀 셸의 `<`" 참고). 그런데 "우리 셸이 아니라 `busybox sh`(ash)도 `<`를 지원하는가"를 검증하는 과정에서 `fcntl` 미구현이 드러났고, 이게 이 단계에서 실제로 새로 배울 개념이라 판단해 단계 자체를 **fcntl 구현**으로 다시 잡았다. 커스텀 셸의 `<` 배선은 버리지 않고 그대로 유지한 채, 그 위에 fcntl을 얹은 형태다.

## 1) 커스텀 셸의 `<` — 새 커널 개념 없음

`user/init.c`의 `split_pipeline`이 `>`/`>>` 토큰을 만나면 `redirect_out[nstages]`를 채우던 것과 완전히 같은 자리에 `<` 토큰 처리를 추가했다:

```c
if (streq(argv[i], "<")) {
    if (scount == 0U || i + 1U >= argc || redirect_in[nstages]) return 0U;
    redirect_in[nstages] = argv[i + 1U];
    i++;
    continue;
}
```

`run_line`의 fork 자식 쪽 적용 순서는 **파이프 dup2 → `redirect_in` dup2 → `redirect_out` dup2**다(bash와 동일하게, 같은 스테이지에 파이프 입력과 `<`가 동시에 있으면 `<`가 이긴다):

```c
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
```

`sys_open`은 이미 `flags=0`(`O_RDONLY`)으로 여는 형태라 새 syscall 래퍼가 필요 없었다. 검증(`run_line("cat < /disk/hello.txt")` 등)은 아래 "검증" 절에 있다.

## 2) busybox ash의 `<`는 실패했다 — `fcntl` 미구현

부팅 시퀀스에 `sh -c 'cat < /disk/hello.txt'`(busybox ash가 자기 방식대로 `<`를 처리)를 추가해 실행해보니:

```
shell: sh -c 'cat < /disk/hello.txt' (busybox ash < check):
syscall: unimplemented rax=110
syscall: unimplemented rax=72
process 1 exited: code=1
```

`rax=72`는 `fcntl`. 미구현 syscall은 `boot/syscall.c`의 `default` 케이스에서 `frame->rax = (u64)-1`을 그대로 돌려주는데, 이 raw `-1`을 libc/ash는 `errno=EPERM`(1)으로 해석한다. ash는 리다이렉션을 걸 때 "원래 fd를 안전한 번호로 옮겨뒀다가 나중에 복원"하는 절차에서 `fcntl(fd, F_DUPFD_CLOEXEC, N)`을 쓰는데, 이게 진짜 에러로 보여서 리다이렉션 설정 자체가 깨졌다(`rax=110`인 `getppid`도 미구현이지만 ash가 그 실패는 무시하고 넘어가서 문제가 안 됐다 — 리다이렉션 없는 `sh -c 'cat ...'`은 그래서 이미 정상 동작했다).

## 3) `fcntl` 구현 — `F_DUPFD`/`F_DUPFD_CLOEXEC`/`F_GETFD`/`F_SETFD`

`fcntl`(72번, `boot/syscall.h`에 `SYS_FCNTL` 추가)은 이미 연 fd의 속성을 조회/변경하는 다목적 syscall이다. `dup2`처럼 "정확히 이 번호로 복제해줘"가 아니라 커맨드(`cmd`)로 동작이 갈린다. 이번에 구현한 네 개:

| 커맨드 | 값 | 동작 |
|--------|-----|------|
| `F_DUPFD` | 0 | `fd`를 복제해 `arg` 이상인 첫 빈 번호에 배치, cloexec 플래그 없음 |
| `F_GETFD` | 1 | `fd`의 `FD_CLOEXEC` 플래그(0 또는 1) 조회 |
| `F_SETFD` | 2 | `fd`의 `FD_CLOEXEC` 플래그를 `arg`로 설정 |
| `F_DUPFD_CLOEXEC` | 1030 | `F_DUPFD`와 같지만 새 fd에 `FD_CLOEXEC`를 켠 채로 반환 |

```c
static u32 sys_fcntl(u32 fd, u32 cmd, u64 arg)
{
    process_t *p = (process_t *)thread_current()->user_data;
    u32        minfd;
    u32        newfd;

    if (fd >= PROC_FD_MAX || !p->fds[fd]) return (u32)-1U;

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        minfd = (u32)arg;
        for (newfd = minfd; newfd < PROC_FD_MAX; newfd++) {
            if (!p->fds[newfd]) {
                p->fds[newfd] = vfs_dup(p->fds[fd]);
                if (cmd == F_DUPFD_CLOEXEC) p->fd_cloexec |= (1U << newfd);
                else p->fd_cloexec &= ~(1U << newfd);
                return newfd;
            }
        }
        return (u32)-1U;
    case F_GETFD:
        return (p->fd_cloexec & (1U << fd)) ? FD_CLOEXEC : 0U;
    case F_SETFD:
        if (arg & FD_CLOEXEC) p->fd_cloexec |= (1U << fd);
        else p->fd_cloexec &= ~(1U << fd);
        return 0U;
    default:
        return (u32)-1U;
    }
}
```

`FD_CLOEXEC` 플래그는 `process_t`에 새로 추가한 `u32 fd_cloexec` 비트마스크(비트 `i` = fd `i`가 cloexec)에 저장한다. `PROC_FD_MAX`(16, 아래 참고)가 32를 넘지 않으니 `u32` 하나로 충분하다.

fd 슬롯이 닫히거나 재사용될 때(`sys_close`, `dup2`가 기존 `newfd`를 덮어쓸 때) 그 비트를 같이 지워야 한다 — 안 지우면 예전에 cloexec였던 fd 번호를 나중에 `open`이 재사용했을 때 엉뚱하게 cloexec가 켜진 채로 남는다. `sys_close`/`sys_dup2` 양쪽에 `p->fd_cloexec &= ~(1U << fd)`를 추가했다. `proc_fork`는 fd 배열과 함께 `fd_cloexec`도 그대로 복사한다(자식이 부모의 cloexec 설정을 물려받는 건 실제 리눅스 `fork()`와 동일).

## 4) `proc_exec`의 exec-time fd 정리 — 하드코딩에서 `FD_CLOEXEC` 기반으로

기존 `proc_exec`는 fd 0/1/2는 항상 남기고 3번 이상은 무조건 다 닫는 식이었다:

```c
for (j = 3U; j < PROC_FD_MAX; j++) {
    if (p->fds[j]) { vfs_close(p->fds[j]); p->fds[j] = 0; }
}
```

이건 실제 리눅스와 반대다 — 리눅스는 **기본적으로 fd가 exec 후에도 살아남고**, `FD_CLOEXEC`가 켜진 fd만 닫힌다. 지금까지는 셸이 exec 직전에 파이프/리다이렉션용 fd를 항상 수동으로 정리해놨기 때문에(0/1/2만 남기고) 이 하드코딩과 실제 동작이 우연히 같았을 뿐이다. 이제 `fcntl`로 진짜 `FD_CLOEXEC`를 다루게 됐으니, `proc_exec`도 그 플래그를 실제로 보게 고쳤다:

```c
for (j = 0U; j < PROC_FD_MAX; j++) {
    if (p->fds[j] && (p->fd_cloexec & (1U << j))) {
        vfs_close(p->fds[j]);
        p->fds[j] = 0;
        p->fd_cloexec &= ~(1U << j);
    }
}
```

범위를 `j=3`이 아니라 `j=0`부터로 바꿨는데도 기존 동작이 깨지지 않는 이유: 지금까지 아무도 `FD_CLOEXEC`를 설정한 적이 없어서(`fd_cloexec`는 항상 0) 이 검사를 fd 0/1/2에 적용해도 애초에 닫힐 일이 없다. ash가 fd 0을 `F_DUPFD_CLOEXEC`로 fd 10에 저장해두는 것도 이 로직이 있어야 "저장해둔 원래 stdin이 exec 후에 새 프로그램 손에 넘어가지 않고 알아서 닫힌다"는 ash의 의도대로 동작한다.

## 5) `PROC_FD_MAX`를 8에서 16으로

fcntl을 붙이고 첫 실행에서 여전히 `sh -c 'cat < ...'`가 `code=1`로 실패했다 — `fcntl(fd=0, F_DUPFD_CLOEXEC, arg=10)`을 로그로 찍어보니 ash가 저장용 fd로 **10번 이상**을 요구하는데, 당시 `PROC_FD_MAX`가 8이라 `for (newfd = minfd; newfd < PROC_FD_MAX; newfd++)` 루프가 `10 < 8`이 거짓이라 아예 안 돌고 실패였다. `PROC_FD_MAX`를 16으로 올리자 통과했다. 8은 `32-vfs-open`부터 쓰던 임의의 작은 값이었고, `fd_cloexec`가 `u32` 비트마스크라 32까지는 자료구조 변경 없이 올릴 수 있어 16을 골랐다(0~9는 일반 fd, 10~15는 ash 같은 프로그램이 "안전지대"로 쓰는 fd에 여유를 둔 값).

## 검증

`user/init.c`의 `init_main`에 다음을 추가했다(커스텀 셸 `<` 셋 + busybox ash `<`/`>`/`>>` 각각):

```c
run_line("cat < /disk/hello.txt");
run_line("cat < /disk/hello.txt | cat");
run_line("cat < /disk/does_not_exist.txt");
...
sh_argv = { "sh", "-c", "cat < /disk/hello.txt" };
run_argv(sh_argv);   // busybox ash가 직접 `<`를 파싱하는 경로
...
sh_argv = { "sh", "-c", "echo ff > /disk/redir3.txt" };
run_argv(sh_argv);   // busybox ash가 직접 `>`를 파싱하는 경로
run_argv({ "cat", "/disk/redir3.txt" });
sh_argv = { "sh", "-c", "echo gg >> /disk/redir3.txt" };
run_argv(sh_argv);   // busybox ash가 직접 `>>`를 파싱하는 경로
run_argv({ "cat", "/disk/redir3.txt" });
```

`>`/`>>` 두 개는 처음엔 없었다 — `<`만 ash 경로로 검증하고 넘어갔는데, "`echo`로 `>`/`>>`/`<`를 전부 busybox `sh` 안에서" 검증됐는지 질문을 받고서야 `>`/`>>`는 지금까지 커스텀 셸(`run_argv_redirect`/`run_line`)로만 검증했고 ash 자신의 리다이렉션 파서를 통과한 적이 없다는 게 드러나 추가했다.

`make clean && make run-nogui` 결과(관련 구간만):

```
shell: cat < /disk/hello.txt:
hello ext2 root fs
process 1 exited: code=0
shell: cat < /disk/hello.txt | cat:
process 1 exited: code=0
hello ext2 root fs
process 2 exited: code=0
shell: cat < /disk/does_not_exist.txt (missing file check):
shell: cannot open file
process 1 exited: code=1
shell: sh -c 'cat < /disk/hello.txt' (busybox ash < check):
hello ext2 root fs
process 1 exited: code=0
shell: sh -c 'echo ff > /disk/redir3.txt' (busybox ash > check):
process 1 exited: code=0
shell: cat /disk/redir3.txt:
ff
process 1 exited: code=0
shell: sh -c 'echo gg >> /disk/redir3.txt' (busybox ash >> check):
process 1 exited: code=0
shell: cat /disk/redir3.txt:
ff
gg
process 1 exited: code=0
```

여섯 경우 모두 기대대로다: 커스텀 셸의 단독 `<`, 파이프와 결합, 존재하지 않는 파일 에러 처리, 그리고 **busybox ash가 자기 방식대로(`fcntl` 기반 fd 셔플) 처리한 `<`/`>`/`>>`** 전부 통과. `>`는 새 파일 생성, `>>`는 기존 내용(`ff`) 뒤에 `gg`를 이어붙인 것까지 확인됐다.

**회귀**: 51~61의 모든 ext2 읽기/쓰기/`getdents`/심링크/PATH exec/`>` 리다이렉션/파이프/동시성 검증이 이전과 동일하게 통과했고, `e2fsck -f -n build/disk.img`도 에러 없이 통과했다.

## 완료 기준

`make clean && make run-nogui`에서 61까지의 모든 출력 뒤에 다음이 이어져야 한다:

```
shell: cat < /disk/hello.txt:
hello ext2 root fs
process 1 exited: code=0
shell: cat < /disk/hello.txt | cat:
process 1 exited: code=0
hello ext2 root fs
process 2 exited: code=0
shell: cat < /disk/does_not_exist.txt (missing file check):
shell: cannot open file
process 1 exited: code=1
shell: sh -c 'cat < /disk/hello.txt' (busybox ash < check):
hello ext2 root fs
process 1 exited: code=0
shell: sh -c 'echo ff > /disk/redir3.txt' (busybox ash > check):
process 1 exited: code=0
shell: cat /disk/redir3.txt:
ff
process 1 exited: code=0
shell: sh -c 'echo gg >> /disk/redir3.txt' (busybox ash >> check):
process 1 exited: code=0
shell: cat /disk/redir3.txt:
ff
gg
process 1 exited: code=0
```

`e2fsck -f -n build/disk.img`가 에러 없이 통과해야 한다.

## 이전 단계(61) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/syscall.h` | 수정 | `SYS_FCNTL = 72` 추가 |
| `boot/syscall.c` | 수정 | `sys_fcntl`(`F_DUPFD`/`F_DUPFD_CLOEXEC`/`F_GETFD`/`F_SETFD`) 신규 + 디스패치 케이스 추가; `sys_close`/`sys_dup2`에서 재사용되는 fd 슬롯의 `fd_cloexec` 비트 정리 추가 |
| `boot/process.h` | 수정 | `process_t`에 `u32 fd_cloexec` 비트마스크 추가; `PROC_FD_MAX`를 8→16으로 |
| `boot/process.c` | 수정 | `proc_alloc`에서 `fd_cloexec` 초기화; `proc_fork`에서 `fd_cloexec` 복사; `proc_exec`의 exec-time fd 정리를 "fd≥3 무조건 닫기"에서 "`FD_CLOEXEC` 플래그가 켜진 fd만 닫기"로 교체 |
| `user/init.c` | 수정 | `split_pipeline`에 `redirect_in[SHELL_STAGE_MAX]` 파라미터와 `<` 토큰 파싱 추가; `run_line`에 `redirect_in` 배열 선언 + fork 자식에서 파이프 dup2 다음·`redirect_out` dup2 이전에 `sys_open`+`dup2(fd,0)` 삽입; 부팅 시퀀스에 커스텀 셸 `<` 검증 3종 + busybox ash `sh -c 'cat < ...'`/`sh -c 'echo ff > ...'`/`sh -c 'echo gg >> ...'` 검증 3종 추가 |
| 나머지 전부 | 변경 없음 | 61의 파일 그대로 |

## 다음 단계 힌트

- **`fcntl`의 나머지 커맨드는 미구현**: `F_GETFL`/`F_SETFL`(예: `O_NONBLOCK` 조회/변경), `F_GETLK`/`F_SETLK`(파일 잠금) 등은 구현하지 않았다. 지금 당장 이걸 쓰는 유저 프로그램이 없어서 미룬다 — 필요해지면(예: 네트워크 소켓을 넌블로킹으로 돌려야 하는 `71-socket-syscall` 즈음) 그때 추가.
- **`O_TRUNC`의 캐시 일관성 문제는 여전히 남아있다**: `60-redirect`가 지적하고 `61-io-lock`도 재확인한 항목 — 이미 열려 `ofiles[i].inode`에 캐시된 파일을 다른 프로세스가 `O_TRUNC`로 열어도 앞의 캐시가 갱신되지 않는다. 이번 단계는 관여하지 않았다.
- **`ext2_lock`은 여전히 파일시스템 전체를 잠그는 굵은 락**: `61-io-lock`이 남긴 대로, inode 단위 락/rwlock 세분화는 필요해지면 재검토.
- **PCI 버스 스캔(`63-pci-enum`)으로 이동**: 셸의 리다이렉션(`>`/`>>`/`<`)과 파이프, 그리고 그걸 실제로 뒷받침하는 `fcntl`(`FD_CLOEXEC`)까지 갖춰졌으니, 로드맵상 다음은 하드웨어 확장(PCI → AHCI → APIC)이다.
