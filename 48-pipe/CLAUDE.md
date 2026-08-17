# 48 — pipe

**목표**: `pipe(22)`/`dup2(33)` syscall과 커널 파이프 백엔드를 추가하고, 셸이 `cmd1 | cmd2 | ...` 파이프라인을 파싱해 자식들을 fork+dup2+exec로 연결하도록 확장한다.

**47에서 이어짐**: 지금까지 프로세스 간 통신 수단은 전무했다 — fd 0/1/2는 항상 콘솔이었고, fork가 fd 테이블을 복제해줘도 그 fd들은 여전히 전부 콘솔을 가리켰다. 파이프는 커널에 두 번째 `vfs_ops_t` 백엔드(콘솔·initrd에 이은 세 번째)를 추가해 프로세스 간에 바이트 스트림을 흘려보내는 첫 사례다.

## 범위: `>` 파일 리다이렉션은 포함하지 않는다

이 OS에는 아직 쓰기 가능한 파일시스템이 없다 — 마운트된 건 `initrd` 하나뿐이고 그마저도 `vfs_ops.write`가 NULL, 콘솔은 fd 번호로 직접 열릴 뿐 마운트 테이블에 없다(`vfs_mount`로 경로 매핑된 적이 없음). 그래서 `open("파일", O_WRONLY|...)`가 성공할 대상이 아예 없다. `>` 리다이렉션을 진짜로 동작시키려면 최소 램 기반 tmpfs 같은 쓰기 가능한 백엔드가 별도로 필요하고, 이건 pipe와는 다른 개념이라 `[[one-concept-per-step]]` 원칙상 여기 넣지 않았다 — 나중 단계(예정된 `51-disk-fs` 근처)로 미룬다. 48은 `cmd1 | cmd2 | ...` 파이프라인만 다룬다.

## 핵심 개념

### `vfs_ops_t`에 `dup` 훅 추가

기존 `vfs_dup`(`vfs.c`)은 `backend_fd`를 그대로 복사한 새 `vfs_file_t`를 만들 뿐, 백엔드에 "복제됐다"는 사실을 알리지 않았다 — 콘솔·initrd는 `backend_fd`가 사실상 의미 없어서(콘솔은 항상 0, initrd는 읽기 전용 독립 파일이라 위치 상태가 파일별로 안 겹침) 문제가 없었다. 파이프는 다르다: 같은 파이프의 read/write 끝을 가리키는 `vfs_file_t`가 fork·`dup2`를 거치며 여러 개로 늘어나는데, 그중 몇 개가 "아직 열려 있는지"를 세어야 EOF/broken-pipe 시점을 판단할 수 있다. 그래서 `vfs_ops_t`에 `void (*dup)(int bfd)`를 추가하고, `vfs_dup`이 복제 직후 `ops->dup`이 있으면 호출하도록 했다(`vfs.c`). 콘솔·initrd는 훅을 NULL로 둬서 기존 동작 그대로다.

### `boot/pipe.c`: 링버퍼 + refcount, 두 개의 `vfs_ops_t`

`pipe_t`는 고정 크기 배열(`pipe_t pipes[PIPE_MAX=4]`, 슬롯당 1KB 링버퍼) — `proc_table`/`mounts[]`와 같은 이 프로젝트의 정적 할당 관례를 따른다. `pipe_create()`가 슬롯 하나를 찾아 `read_refs=1`/`write_refs=1`로 초기화하고, `pipe_read_ops`/`pipe_write_ops` 두 개의 `vfs_ops_t`로 각각 `vfs_file_t`를 하나씩 만들어 반환한다. **read 끝의 `write` 훅과 write 끝의 `read` 훅은 둘 다 NULL이다** — 실제 파이프처럼 반대 방향 접근을 막는다. `vfs_write`는 이미 `write==NULL`을 가드하지만(`vfs.c`), **`vfs_read`는 원래 `read==NULL`을 가드하지 않았다** — 이번에 파이프를 붙이면서 처음으로 read가 NULL일 수 있는 백엔드가 생겼는데도 가드를 추가하지 않았다(다음 단계 힌트 참고). 대신 커널 쪽에서 read 끝에는 `read`만, write 끝에는 `write`만 유효하게 배선해 애초에 잘못된 방향으로 호출될 일이 없도록 했다.

읽기/쓰기는 `proc_wait`(`process.c`)와 동일한 관용구를 그대로 쓴다 — `wq_add(wait_queue, thread_current()); thread_park();`를 조건이 만족될 때까지 반복. 별도의 `interrupts_disable` 래핑 없이 기존 코드와 같은 수준의(작지만 존재하는) 경쟁 조건을 그대로 안고 간다 — 일관성을 위한 의도적 선택.

- **읽기**(`pipe_read`): `count>0`이면 있는 만큼 꺼내고 `wait_write`를 깨운다. `count==0`이고 `write_refs==0`(쓰기 쪽이 전부 닫힘)이면 EOF로 `0`을 반환한다.
- **쓰기**(`pipe_write`): 버�터가 꽉 찼는데 `read_refs==0`(읽기 쪽이 전부 닫힘)이면 그 시점까지 쓴 바이트 수를 반환하고 멈춘다 — SIGPIPE 같은 시그널은 보내지 않는다(다음 단계 힌트).

`close`/`dup` 훅은 read/write 끝마다 따로 있고(`pipe_read_close`/`pipe_write_close`/`pipe_read_dup`/`pipe_write_dup`), 카운트가 0이 될 때 반대편 대기열을 깨우며, 양쪽 refcount가 모두 0이 되면 슬롯을 반납한다(`pipe_maybe_free`).

### `sys_pipe`/`sys_dup2`: 리눅스 x86_64 ABI 번호 그대로

`pipe=22`, `dup2=33` — `44-syscall64`에서 세운 "syscall 번호는 진짜 x86_64 관례를 따른다" 원칙을 유지했다. (최상위 `CLAUDE.md` 로드맵 표에 적혀 있던 `pipe(42)`/`dup2(63)`는 오타다 — 63은 이미 이 코드베이스에서 `SYS_UNAME`으로 쓰이고 있어서 명백히 잘못된 번호였다. 로드맵 표도 이번에 22/33으로 고쳤다.)

`sys_pipe`는 `pipe_create`로 얻은 두 `vfs_file_t`를 프로세스의 fd 테이블에서 빈 슬롯 두 개(먼저 read, 다음 write)에 꽂고 `fds[0]=read_fd, fds[1]=write_fd`로 유저에 돌려준다 — 리눅스 `pipe(2)`와 동일한 순서. `sys_dup2`는 POSIX `dup2`처럼 `newfd`가 이미 열려 있으면 먼저 닫고, `oldfd==newfd`면 아무것도 안 하고 `newfd`를 반환하고, 아니면 `vfs_dup`으로 복제해 꽂는다.

### 셸: `fork` → 자식이 `dup2` → `exec`가 나머지를 정리해준다

`user/init.c`의 `_start`는 입력 줄을 먼저 공백 기준으로 전부 토큰화(`split_argv`, 기존 그대로)한 뒤, `|` 토큰을 경계로 스테이지별 `argv`로 다시 묶는다(`split_pipeline`, 신규) — **`|`는 양옆에 공백이 있어야 별도 토큰으로 인식된다** (`cmd1|cmd2`처럼 붙여 쓰면 한 단어로 취급돼 "not found"가 난다). 스테이지 수는 `SHELL_STAGE_MAX=3`으로 제한했다 — `PROC_FD_MAX=8`인데 콘솔 3개(fd 0/1/2) + 파이프 `N-1`개(fd 2개씩)를 셸이 fork 시점까지 동시에 들고 있어야 해서, 4단계 이상이면 fd 테이블이 넘친다.

각 스테이지를 `sys_fork()`로 자식을 만들고, **자식만** 자기 번호(`i`)에 따라 `dup2`한다 — 첫 스테이지가 아니면 이전 파이프의 read 끝을 fd 0으로, 마지막 스테이지가 아니면 자기 파이프의 write 끝을 fd 1로. 그 다음 자식은 원본 파이프 fd들을 전부 닫고(자기가 쓸 fd 0/1은 이미 `dup2`로 별도 복제됐으므로 안전) `exec`한다. **`proc_exec`이 fd 3번 이상을 자동으로 닫아주므로**(`process.c`, 46 이전부터 있던 동작) 자식이 명시적으로 정리하지 않아도 실행 이미지가 바뀌는 순간 남은 파이프 fd가 새는 일이 없다 — 이번 파이프라인 배선에서 별도 코드를 더 쓰지 않은 이유.

부모(셸)는 모든 자식을 fork한 뒤에야 자기가 들고 있던 원본 파이프 fd들을 닫는다 — 그래야 마지막 스테이지가 EOF를 보려 할 때 "쓰기 쪽 refcount"가 정말 0이 된다. 부모가 이걸 먼저 안 닫으면 모든 자식이 끝나도 부모의 사본 때문에 `write_refs`가 절대 0이 안 되고, 마지막 리더가 영원히 블록된다.

### 실전에서 만난 버그: `syscall` 인라인 asm의 암묵적 `rax` 클로버 누락

구현 중 `hello | pipe`를 돌리면 커널이 `rip=0x0000000000000000`으로 페이지 폴트를 내며 즉사했다. 원인은 `sys_close`의 인라인 asm:

```c
__asm__ volatile ("syscall" : : "a"(3L), "D"(fd) : "rcx", "r11", "memory");
```

`"a"(3L)`은 입력일 뿐 출력이 없어서, GCC는 `syscall` 명령이 `rax`를 바꾼다는 걸 모른다. 셸이 `sys_close(pipefd[i][0]); sys_close(pipefd[i][1]);`처럼 **같은 리터럴로 두 번 연속** 호출하면, GCC -O2가 두 번째 `mov eax,3`을 "이미 3이다"라며 지워버렸다 — 그런데 실제로는 첫 번째 `close` syscall이 커널에서 `frame->rax=0`으로 리턴하며 `rax`를 실제로 0으로 바꿔놓은 뒤였다. 그래서 두 번째 호출이 `rax=0`(=`SYS_READ`) + `rdi=4`(파이프 write 끝)로 실행돼 `sys_read`가 `ops->read==NULL`인 write 끝을 그대로 호출 — 커널 모드에서 NULL 함수 포인터를 콜해 크래시.

고친 방법은 `sys_write`/`sys_read`처럼 `"=a"(ret)` 출력을 추가해 `rax`가 바뀐다는 걸 GCC에 알리는 것(`sys_close`, `user/init.c`). **`sys_exit`/`sys_exec`도 같은 패턴(출력 없는 `"a"(N)`)을 쓰지만, 각각 프로세스 종료 직전 또는 실패 시 단 한 번만 호출되고 그 뒤로 같은 함수를 다시 호출하지 않아 지금까지 드러나지 않았다** — 잠재적으로 같은 문제를 안고 있으니 향후 이 함수들을 반복 호출하는 코드를 추가하면 동일하게 고쳐야 한다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 이전과 동일하게 `$` 프롬프트에서 멈춘다(`initramfs: N file(s) found`의 파일 수가 10→11로 늘어난다, `pipe` 검증 프로그램 추가). GUI(`make run`)에서:

```
$ hello | pipe
process 1 exited: code=0
pipe: reading from stdin
pipe: got: hello: Hello from hello!
pipe: EOF, exiting
process 2 exited: code=0
```

3단계 파이프라인(`hello | pipe | pipe`)도 데이터가 두 파이프를 순서대로 통과하며 각 스테이지가 정상 종료한다. 같은 파이프라인을 반복 실행해도(fd/파이프 슬롯 재사용) 문제없다. `hello`/`hello2`/`brk`/`mmap`/`tls`/`syscall64`/`musl_hello`/`signal`(`Ctrl+C` 3회)도 회귀 없이 그대로 동작한다 — QEMU 모니터의 `sendkey`(HMP)를 스크립트로 순서대로 넣어 확인했다.

## 이전 단계(47) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/pipe.h` | 신규 | `pipe_init()`, `pipe_create()` 선언 |
| `boot/pipe.c` | 신규 | 고정 슬롯 `pipe_t[PIPE_MAX]`(1KB 링버퍼 + refcount + wait queue), read/write 전용 `vfs_ops_t` 두 개, `pipe_create` |
| `boot/vfs.h` | 수정 | `vfs_ops_t`에 `dup` 훅 추가 |
| `boot/vfs.c` | 수정 | `vfs_dup`이 복제 후 `ops->dup` 호출 |
| `boot/console_dev.c` | 수정 | `console_ops` 초기화에 `dup=0` 추가(구조체 필드 증가분) |
| `boot/kernel.c` | 수정 | `initrd_ops` 초기화에 `dup=0` 추가; `pipe_init()` 호출 추가 |
| `boot/syscall.h` | 수정 | `SYS_PIPE`(22)/`SYS_DUP2`(33) 추가 |
| `boot/syscall.c` | 수정 | `#include "pipe.h"`; `sys_pipe`/`sys_dup2` 구현과 디스패치 케이스 추가 |
| `user/pipe.c` | 신규 | fd 0에서 EOF까지 읽어 `pipe: got: ` 접두사를 붙여 fd 1로 되쏘는 검증 프로그램 |
| `user/init.c` | 수정 | `sys_pipe`/`sys_dup2`/`sys_close` syscall 래퍼 추가; `split_pipeline`로 `\|` 파싱; `_start`가 다단 파이프라인을 fork+dup2+exec로 배선 |
| `Makefile` | 수정 | `PIPEOBJ`(`boot/pipe.o`) 빌드·링크, 관련 룰에 `boot/pipe.h` 의존성 반영, `USERPIPE`(`user/pipe.c`→`build/pipe`) 빌드·링크·initramfs 포함·clean 반영 |
| `initrd/.gitignore` | 수정 | `pipe` 패턴 추가 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `vfs_read`가 여전히 `ops->read==NULL`을 가드하지 않는다 — 이번엔 커널이 read/write 끝을 절대 혼동하지 않도록 배선해서 피해갔지만, 원칙적으로는 `vfs_write`처럼 `if (!f->ops->read) return 0U;`를 넣는 게 안전하다.
- 파이프 읽기도 47에서 남긴 "진짜 블로킹 syscall 도중의 `Ctrl+C`" 힌트와 같은 한계를 그대로 안고 있다 — `pipe_read`/`pipe_write`의 `thread_park()` 루프는 `sig_pending`을 확인하지 않는다. `con_read`와 함께 나중에 EINTR 조기 리턴을 넣을 때 같이 고칠 수 있다.
- 쓰기 쪽이 막힌 채로(`read_refs==0`) 계속 쓰면 지금은 조용히 짧은 반환만 한다 — 진짜 유닉스처럼 `SIGPIPE`를 보내려면 `pipe_write`가 `signal_raise_current(SIGPIPE)`를 호출하도록 확장해야 한다(`SIGPIPE` 상수 자체가 아직 없다).
- `>` 파일 리다이렉션은 쓰기 가능한 파일시스템이 생겨야 의미가 있다 — tmpfs든 `51-disk-fs`든, 그 단계에서 셸의 `split_pipeline`과 비슷한 자리에 `>` 토큰 처리를 추가하면 된다.
- `sys_exit`/`sys_exec`(`user/init.c`, 그리고 다른 `user/*.c`의 동일 패턴)는 이번에 고친 `sys_close`와 같은 잠재적 `rax` 클로버 누락을 안고 있다 — 지금은 반복 호출되지 않아 드러나지 않을 뿐이다.
