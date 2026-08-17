# 49 — fork-clone-fix

**목표**: 진짜 glibc가 쓰는 `fork()`(=`clone` syscall 기반)와 TLS를 조합했을 때만 드러나는 커널 버그 4개를 고친다. busybox 없이, 이 저장소 안에서 직접 짠 최소 재현 프로그램(`user/forkclone.c`) 하나로 전부 검증한다.

**48에서 이어짐**: `41-brk`~`46-musl-hello`에서 `arch_prctl`/TLS를, `27-proc-exec`~`31-clone-trampoline`에서 fork/clone을 각각 만들었지만, 지금까지 이 저장소의 어떤 유저 프로그램도 **"TLS를 설정해둔 상태에서 fork한다"**는 조합을 시험한 적이 없었다. 우리가 직접 짠 프로그램들은 fork를 쓰거나(`hello`/`pipe`) TLS를 쓰거나(`tls`/`musl_hello`) 둘 중 하나였지, 둘 다는 아니었다. 이 조합은 다음 단계에서 진짜 glibc 정적 바이너리(busybox)를 올리려는 시도 중에 우연히 처음 걸렸고, 그 결과 커널의 오래된 잠재 버그 네 개가 한꺼번에 드러났다. 이 단계는 그 네 개를 busybox와 분리해서 순수하게 고친다 — `[[one-concept-per-step]]` 원칙에 따라 "커널을 fork/clone/wait 관점에서 POSIX에 더 가깝게 만드는 것"과 "외부 대형 바이너리를 올리는 것"을 나눴다.

## 버그 1: `SYS_CLONE`이 glibc의 진짜 flags를 "child_stack 포인터"로 오인

`30-thread-clone`/`31-clone-trampoline`에서 만든 `SYS_CLONE` 핸들러는 이 저장소 자체 `thread_create(fn, arg)` 라이브러리(`clone.asm`) 하나만을 위한 것이었다 — `rdi`는 항상 유저가 넘긴 "child_stack 주소"라고 가정하고 그대로 `ctx.user_rsp = frame->rdi`에 꽂았다. 그런데 **glibc의 `fork()`는 `fork`(57)가 아니라 `clone`(56) syscall로 구현되어 있다** — 이때 `rdi`는 진짜 Linux clone flags 비트마스크(`CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD` = `0x1200011`)다. 이 값을 스택 주소로 오인해 유저 RSP에 그대로 꽂으면 자식이 완전히 엉뚱한 스택을 갖게 된다.

고친 방법: `frame->rdi`의 `CLONE_VM`(0x100) 비트로 분기한다(`syscall.c`). 이 비트가 있으면(이 저장소의 `clone_trampoline`처럼 진짜 스레드—주소공간 공유—를 요청하는 것) 기존 `proc_clone()`으로, 없으면(=glibc의 fork-via-clone, 독립된 주소공간을 원하는 것) `proc_fork()`로 보낸다. `proc_fork()`가 이미 페이지 테이블 복사와 `fs_base` 상속을 정확히 하고 있어서 재사용만 하면 됐다.

## 버그 2: `enter_user_mode_fork`가 유저 모드 진입 직전 FS.base를 도로 지움

버그 1을 고쳐도 자식이 TLS(`%fs:...`)를 건드리는 순간 페이지 폴트가 났다(`cr2`가 FS.base 근처 낮은 주소). 원인: 스레드 전환 시 `activate_thread`(`thread.c`)가 `gdt_set_fs_base()`로 FS_BASE MSR을 정확히 WRMSR해두는데, 바로 다음에 실행되는 `enter_user_mode_fork`(`gdt.asm`)가 링3 진입 준비로 `mov fs, ax`를 실행한다 — x86-64에서 세그먼트 셀렉터를 다시 로드하면 GDT 디스크립터의 base(=0, flat segment)로 FS.base가 리셋된다. 즉 WRMSR로 맞춰둔 값이 그 직후 조용히 지워졌다.

지금까지 안 드러난 이유: fork를 쓰는 프로그램(`hello`/`pipe`)은 TLS를 안 쓰고, TLS를 쓰는 프로그램(`musl_hello`/`tls`)은 fork를 안 써서 이 조합이 처음이었다. 프로그램이 syscall(예: `arch_prctl`)로 FS.base를 설정하고 `SYSRET`으로 돌아오는 경로는 세그먼트 레지스터를 안 건드리므로 문제가 없다 — 오직 `iretq` 기반 유저 모드 진입 경로(`enter_user_mode`/`enter_user_mode_fork`)만 이 함정에 걸린다. `enter_user_mode`는 프로세스가 막 실행을 시작하는 시점이라 FS.base=0이 맞는 값이라 문제가 없다.

고친 방법: `enter_user_mode_fork`에 두 번째 인자로 `fs_base`를 추가하고(`rsi` 레지스터로 전달, `rdi + 8`을 읽어 `rsi`를 덮어쓰기 전에 먼저 소비), `mov fs, ax` 직후 그 값으로 다시 WRMSR한다(`gdt.asm`). 호출부(`process.c`의 `fork_child_trampoline`/`clone_fork_trampoline`)는 `thread_current()->fs_base`를 넘긴다.

이 두 버그를 고치는 김에, `thread_create_with_data`가 스케줄 큐에 스레드를 연결한 **뒤에**야 호출자가 `t->pd`/`t->fs_base`를 채우던 구조도 같이 정리했다(`thread.c`/`thread.h`) — 큐 연결 순간부터 그 스레드는 스케줄될 수 있는데, `pd`/`fs_base`가 아직 기본값(0)일 때 타이머 인터럽트가 끼어들면 잘못된 값으로 활성화될 수 있는 레이스였다. `thread_create_with_data(fn, data, pd, fs_base)`로 파라미터화해서 큐에 넣기 전에 확정하도록 바꿨다.

## 버그 3: `proc_wait`가 리눅스 wait status 인코딩을 안 지킴

`proc_wait`는 자식의 종료 코드를 `*exit_code = p->exit_code`로 raw 값 그대로 유저에 돌려줬다. 리눅스 `wait4(2)` 규약은 정상 종료를 `(code & 0xff) << 8`로 인코딩해야 `WIFEXITED`가 참이 되는데, 예를 들어 raw `126`(`0x7E`)을 그대로 주면 하위 7비트가 0이 아니라서 glibc가 "시그널 126으로 죽었다"고 오판한다(`strsignal`이 실패해 `Unknown signal 126`류의 메시지가 나옴). 이 저장소의 자체 셸(`init.c`)은 `exit_code` 값을 아예 안 써서 지금까지 드러나지 않았다.

고친 방법: `proc_wait`의 두 리턴 경로(특정 pid, `pid==-1` 임의 자식) 모두 `(exit_code & 0xFFU) << 8`로 인코딩한다(`process.c`). `47-signal`이 시그널로 죽은 프로세스에 `128+signum`을 `exit_code`로 저장하는 기존 관례(bash `$?` 관례와 동일)는 그대로 두고, wait4 리턴 시에만 인코딩을 적용해 항상 "정상 종료"로 보이게 통일했다.

## 버그 4: `proc_wait(-1, ...)`가 자식이 하나도 없어도 영원히 블록

진짜 리눅스는 `wait()`/`wait4(-1, ...)` 호출 시 자식이 아예 없으면 즉시 `-1`/`ECHILD`를 리턴해야 한다. `proc_wait`의 `pid==-1` 분기는 "호출자에게 자식이 있는지" 자체를 확인하지 않고 무조건 `find_zombie_child` → 없으면 `thread_park()`를 반복했다. 자식이 아예 없으면 아무도 이 스레드를 다시 깨워줄 수 없어 영원히 잠든다 — 실전에서는 fork 자식을 정상적으로 reap한 뒤 job-control 셸이 습관적으로 한 번 더 `wait4(-1,...)`를 호출하는 경우 이 상태에 빠졌다.

고친 방법: `proc_table`을 훑어 호출자의 자식(`PROC_FREE`가 아닌 상태 + `parent_pid` 일치)이 하나라도 있는지 보는 `has_any_child()`를 추가하고, `pid==-1` 분기 맨 앞에서 없으면 즉시 `(u32)-1U`를 리턴한다(`process.c`).

## 검증: `user/forkclone.c`

네 버그를 한 번에 재현·검증하는 최소 프로그램. `tls.c`처럼 `tls_block[0]`에 매직값을 넣고 `arch_prctl(ARCH_SET_FS)`로 TLS를 설정한 뒤, `mmap.c`의 6-레지스터 인라인 asm 관례를 따라 **raw `clone`(rax=56) syscall을 glibc와 똑같은 flags(`0x1200011`, `CLONE_VM` 없음)로 직접 호출**한다 — 이 저장소의 `clone_trampoline`(`clone.asm`)을 거치지 않고 직접 호출하는 이유는, `clone_trampoline`이 애초에 "child_stack을 넘기는" 저장소 자체 관례라 glibc의 fork-via-clone 흉내를 낼 수 없기 때문이다.

자식은 `%fs:0`을 읽어 TLS가 살아있는지 확인하고 `exit(42)`한다. 부모는 `wait4(-1, &status)`로 자식을 reap해 `status`를 디코딩하고(버그 3 검증), 자식이 없는 상태에서 `wait4(-1, ...)`를 한 번 더 불러 즉시 리턴하는지 확인한다(버그 4 검증).

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

GUI(`make run`)에서:

```
$ forkclone
forkclone: parent clone rc = 0x0000000000000002
forkclone: child fs:0 = 0x1234567890ABCDEF
process 2 exited: code=42
forkclone: wait4 pid = 0x0000000000000002 status = 0x0000000000002A00 exitcode = 0x000000000000002A
forkclone: second wait4 (no children) returned = 0x00000000FFFFFFFF
process 1 exited: code=0
$
```

- `child fs:0 = 0x1234567890ABCDEF` — TLS가 fork-via-clone 자식에서도 살아있음(버그 1+2 검증).
- `status = 0x2A00`, `exitcode = 0x2A`(=42) — wait4 status가 `WIFEXITED`로 정확히 디코딩됨(버그 3 검증).
- `second wait4 ... = 0xFFFFFFFF` — 자식이 없을 때 즉시 리턴, 블록 없음(버그 4 검증).

`hello`/`hello2`/`brk`/`mmap`/`tls`/`syscall64`/`musl_hello`/`hello | pipe`/`hello | pipe | pipe`도 회귀 없이 그대로 동작한다 — QEMU HMP 모니터를 유닉스 소켓으로 열고 `sendkey`를 스크립트로 순서대로 넣어 확인했다.

## 이전 단계(48) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/thread.h` | 수정 | `thread_create_with_data(fn, data, pd, fs_base)` — 스케줄 큐 연결 전에 `pd`/`fs_base`를 확정하도록 파라미터화 |
| `boot/thread.c` | 수정 | `thread_create_with_data` 본문이 파라미터로 받은 `pd`/`fs_base`를 큐 연결 전에 채움(레이스 제거); `thread_create`는 0으로 위임 |
| `boot/gdt.asm` | 수정 | `enter_user_mode_fork`가 `mov fs,ax` 직후 두 번째 인자(`fs_base`, `rsi`)로 FS_BASE MSR을 재적용 — 세그먼트 리로드가 지우는 FS.base 복구 |
| `boot/syscall.c` | 수정 | `SYS_CLONE`이 `CLONE_VM` 비트로 `proc_clone`/`proc_fork` 분기(glibc fork-via-clone 지원) |
| `boot/process.c` | 수정 | `proc_spawn`/`proc_fork`/`proc_clone`이 `thread_create_with_data`에 `pd`/`fs_base`를 직접 전달; `enter_user_mode_fork` 호출에 `fs_base` 추가; `proc_wait`가 wait4 status를 `<<8`로 인코딩하고 `pid==-1` 대기 시 자식이 없으면 즉시 리턴하는 `has_any_child()` 추가 |
| `user/forkclone.c` | 신규 | 네 버그를 한 번에 재현·검증하는 프로그램 — TLS 설정 후 raw `clone`(glibc fork-via-clone 흉내)+`wait4` 두 번 |
| `Makefile` | 수정 | `FORKCLONEOBJ`/`USERFORKCLONE` 빌드·링크·initramfs 포함·clean 반영 |
| `initrd/.gitignore` | 수정 | `forkclone` 패턴 추가 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `50-busybox-sh`: 이 단계에서 고친 fork/clone/TLS/wait4 기반 위에서 진짜 glibc 정적 바이너리(busybox)를 올린다. 이 네 버그를 여기서 먼저 고쳐두지 않았다면 busybox의 `fork()` 호출 한 번에 커널이 죽거나 셸이 멈췄을 것이다.
- `proc_clone`(진짜 `CLONE_VM` 스레드 경로)은 지금 `fs_base`를 상속하지 않는다(항상 0으로 시작) — 이 저장소의 `thread_create`가 TLS 없는 스레드만 만들어서 지금은 문제가 안 되지만, 언젠가 TLS를 쓰는 멀티스레드 프로그램(pthread 흉내)을 올리면 같은 클래스의 버그가 재현될 수 있다.
- `has_any_child()`는 `pid==-1` 분기에서만 적용했다 — 특정 pid를 기다리는 분기(`proc_wait(pid, ...)`)는 `proc_get(pid)`가 실패하면 이미 `-1`을 리턴하므로 같은 문제가 없지만, "그 pid가 애초에 내 자식이 맞는지"는 검증하지 않는다(다른 프로세스의 자식을 기다려도 그냥 기다려짐) — 지금은 셸이 항상 자기가 막 fork한 자식만 기다려서 문제가 안 된다.