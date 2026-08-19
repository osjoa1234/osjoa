# 49 — fork-clone-fix

**목표**: 진짜 musl 정적 바이너리가 쓰는 `fork()`(raw `fork`(57) syscall)와 TLS를 조합했을 때만 드러나는 커널 버그 3개를 고친다. busybox 없이, 이 저장소 안에서 직접 짠 최소 재현 프로그램(`user/forkclone.c`) 하나로 전부 검증한다.

**48에서 이어짐**: `41-brk`~`46-musl-hello`에서 `arch_prctl`/TLS를, `27-proc-exec`~`31-clone-trampoline`에서 fork/clone을 각각 만들었지만, 지금까지 이 저장소의 어떤 유저 프로그램도 **"TLS를 설정해둔 상태에서 fork한다"**는 조합을 시험한 적이 없었다. 우리가 직접 짠 프로그램들은 fork를 쓰거나(`hello`/`pipe`) TLS를 쓰거나(`tls`/`musl_hello`) 둘 중 하나였지, 둘 다는 아니었다. 이 조합은 다음 단계에서 musl 정적 바이너리(busybox)를 처음 올려보려는 시도 중에 실제로 걸렸고, 그 결과 커널의 오래된 잠재 버그가 한꺼번에 드러났다. 이 단계는 그것들을 busybox와 분리해서 순수하게 고친다.

## musl은 `clone` flags를 오인하는 버그를 만나지 않는다

이 계보를 처음 시도했을 때(별도 실험) glibc 정적 바이너리로 같은 조합을 걸었더니 **`SYS_CLONE`(56) 핸들러가 glibc의 fork-via-clone 플래그(`0x1200011`)를 자식 스택 주소로 오인하는** 버그가 있었다. musl은 다르다 — 실제 musl 소스(`src/process/fork.c`)를 확인하면 `fork()`는 내부적으로 `_Fork()`를 호출하고, x86_64에는 `__NR_fork`(57)가 그대로 존재해서 musl은 **raw `fork`(57) syscall**을 쓴다. glibc처럼 `clone`(56)에 `CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD` 플래그를 실어 fork를 흉내내는 건 glibc NPTL 특유의 구현이지, POSIX `fork()`의 필수 조건이 아니다. 이 커널은 `27-proc-exec`/`28-proc-fork`에서 이미 `SYS_FORK`(57) 전용 핸들러를 갖고 있었고 그 핸들러는 처음부터 옳았으므로, musl이 `fork()`를 불러도 `SYS_CLONE` 쪽 코드는 아예 실행되지 않는다 — **이번 단계는 `SYS_CLONE`을 건드리지 않는다.** (이 오인 버그 자체는 여전히 커널에 잠재해 있다 — 언젠가 glibc 프로그램이나 `clone` syscall을 직접 쓰는 코드가 올라오면 그때 다시 다뤄야 한다.)

## 버그 1: `enter_user_mode_fork`가 유저 모드 진입 직전 FS.base를 도로 지움

`forkclone`(TLS 설정 후 raw `fork`(57))을 처음 돌리자 자식이 `%fs:0`을 읽는 순간 페이지 폴트가 났다(`cr2`가 낮은 주소, FS.base가 0으로 리셋된 것과 일치). 원인: 스레드 전환 시 `activate_thread`(`thread.c`)가 `gdt_set_fs_base()`로 FS_BASE MSR을 정확히 WRMSR해두는데, 바로 다음에 실행되는 `enter_user_mode_fork`(`gdt.asm`)가 링3 진입 준비로 `mov fs, ax`를 실행한다 — x86-64에서 세그먼트 셀렉터를 다시 로드하면 GDT 디스크립터의 base(=0, flat segment)로 FS.base가 리셋된다. 즉 WRMSR로 맞춰둔 값이 그 직후 조용히 지워졌다.

지금까지 안 드러난 이유: fork를 쓰는 프로그램(`hello`/`pipe`)은 TLS를 안 쓰고, TLS를 쓰는 프로그램(`musl_hello`/`tls`)은 fork를 안 써서 이 조합이 처음이었다. 프로그램이 syscall(예: `arch_prctl`)로 FS.base를 설정하고 `SYSRET`으로 돌아오는 경로는 세그먼트 레지스터를 안 건드리므로 문제가 없다 — 오직 `iretq` 기반 유저 모드 진입 경로(`enter_user_mode`/`enter_user_mode_fork`)만 이 함정에 걸린다. `enter_user_mode`는 프로세스가 막 실행을 시작하는 시점이라 FS.base=0이 맞는 값이라 문제가 없다.

처음 고친 방법(폐기): `enter_user_mode_fork`에 두 번째 인자로 `fs_base`를 추가해 `mov fs, ax` 직후 WRMSR로 다시 적용했다. 동작은 했지만 진짜 리눅스가 하는 방식과는 달랐다 — 리눅스/glibc/musl은 애초에 **FS 셀렉터에 0(널)을 넣고, base는 오직 `FS_BASE` MSR(`arch_prctl`/`wrfsbase`)로만 관리**한다. `iretq`는 64비트 모드에서 DS/ES/FS/GS를 아예 복원하지 않으므로(SDM의 롱모드 인터럽트 프레임에 그 필드 자체가 없음), 링3 진입 시 세그먼트 레지스터를 결국 만지는 건 커널이 명시적으로 `mov fs, ax` 같은 코드를 실행할 때뿐이다.

이 방식에 맞춰 다시 고쳤다: `enter_user_mode_fork`의 `mov fs, ax`(원래 0x23, 링3용 flat 유저 데이터 셀렉터)를 **아예 삭제**했다(`gdt.asm`) — `activate_thread`가 이 함수 실행 직전에 이미 올바른 `fs_base`를 WRMSR해뒀으므로, 그 뒤로 FS 셀렉터를 다시 로드하는 명령을 아예 안 만나면 그 값이 그대로 살아남는다. 처음에는 `mov fs, ax`를 지우는 대신 `ax=0`(널 셀렉터)으로 바꿔보기도 했는데 — "널 셀렉터 로드는 base를 안 건드린다"는 실제 리눅스/glibc 관례와 달리, **`mov fs, sel` 자체는 값이 0이든 뭐든 매번 hidden base를 셀렉터로부터 새로 파생시킨다**(널이면 0으로). 즉 `mov fs, 0`도 여전히 `fs_base`를 지워서 `forkclone`이 그대로 페이지 폴트 났다 — 실측으로 확인. 진짜 리눅스가 이 문제를 안 만나는 이유는 "널을 로드해서"가 아니라, **프로세스가 맨 처음 시작할 때 딱 한 번만 FS=0을 로드해두고 그 뒤로 fork든 컨텍스트 스위치든 다시는 FS 셀렉터를 로드하지 않기 때문**이다 — base 변경은 항상 WRMSR/`wrfsbase`만으로 이뤄진다.

`enter_user_mode`(프로세스 최초 시작점)는 `fs_base=0`이 정확히 맞는 값이라 원래도 문제가 없었지만, 리눅스 관례와 일관되게 `mov fs, ax`의 `ax`를 0x23(ds/es/gs와 공유하던 값)이 아니라 0(널)으로 바꿔뒀다 — 여기는 유일하게 FS 셀렉터를 로드하는 지점으로 남기고, 그 이후 어떤 경로(fork, exec, context switch)도 FS 셀렉터를 다시 건드리지 않는다. `enter_user_mode_fork`에 추가했던 `fs_base` 인자(`process.c`의 `fork_child_trampoline`/`clone_fork_trampoline` 호출부)는 더 이상 필요 없어 제거했다.

이 버그를 고치는 김에, `thread_create_with_data`가 스케줄 큐에 스레드를 연결한 **뒤에**야 호출자가 `t->pml4_phys`/`t->fs_base`를 채우던 구조도 같이 정리했다(`thread.c`/`thread.h`) — 큐 연결 순간부터 그 스레드는 스케줄될 수 있는데, `pml4_phys`/`fs_base`가 아직 기본값(0)일 때 타이머 인터럽트가 끼어들면 잘못된 값으로 활성화될 수 있는 레이스였다. `thread_create_with_data(fn, data, pml4_phys, fs_base)`로 파라미터화해서 큐에 넣기 전에 확정하도록 바꿨다. 필드 이름은 원래 32비트 2단계 페이징 시절의 `pd`(page directory)를 그대로 쓰고 있었는데, `39-paging-thread-64`에서 4단계 페이징으로 전환한 뒤 `paging.c`/`paging.h`는 전부 `pml4_phys`로 불렀지만 `thread.h`/`thread.c`만 리네임이 안 된 채 남아 있었다 — 이번에 `pml4_phys`로 맞췄다.

## 버그 2: `proc_wait`가 리눅스 wait status 인코딩을 안 지킴

버그 1을 고쳐도 `forkclone`의 `wait4` 결과가 이상했다. `proc_wait`는 자식의 종료 코드를 `*exit_code = p->exit_code`로 raw 값 그대로 유저에 돌려줬다. 리눅스 `wait4(2)` 규약은 정상 종료를 `(code & 0xff) << 8`로 인코딩해야 `WIFEXITED`가 참이 되는데, raw `42`(`0x2A`)를 그대로 주면 하위 7비트가 0이 아니라서 libc가 "시그널 42로 죽었다"고 오판한다. 이 저장소의 자체 셸(`init.c`)은 `exit_code` 값을 아예 안 써서 지금까지 드러나지 않았다.

고친 방법: `proc_wait`의 두 리턴 경로(특정 pid, `pid==-1` 임의 자식) 모두 `(exit_code & 0xFFU) << 8`로 인코딩한다(`process.c`). `47-signal`이 시그널로 죽은 프로세스에 `128+signum`을 `exit_code`로 저장하는 기존 관례(bash `$?` 관례와 동일)는 그대로 두고, wait4 리턴 시에만 인코딩을 적용해 항상 "정상 종료"로 보이게 통일했다.

## 버그 3: `proc_wait(-1, ...)`가 자식이 하나도 없어도 영원히 블록

진짜 리눅스는 `wait()`/`wait4(-1, ...)` 호출 시 자식이 아예 없으면 즉시 `-1`/`ECHILD`를 리턴해야 한다. `proc_wait`의 `pid==-1` 분기는 "호출자에게 자식이 있는지" 자체를 확인하지 않고 무조건 `find_zombie_child` → 없으면 `thread_park()`를 반복했다. 자식이 아예 없으면 아무도 이 스레드를 다시 깨워줄 수 없어 영원히 잠든다.

고친 방법: `proc_table`을 훑어 호출자의 자식(`PROC_FREE`가 아닌 상태 + `parent_pid` 일치)이 하나라도 있는지 보는 `has_any_child()`를 추가하고, `pid==-1` 분기 맨 앞에서 없으면 즉시 `(u32)-1U`를 리턴한다(`process.c`).

## 검증: `user/forkclone.c`

세 버그를 한 번에 재현·검증하는 최소 프로그램. `tls.c`처럼 `tls_block[0]`에 매직값을 넣고 `arch_prctl(ARCH_SET_FS)`로 TLS를 설정한 뒤, **musl이 실제로 부르는 것과 똑같이 raw `fork`(57) syscall**을 직접 호출한다 — `clone`(56)이 아니다. 이 저장소의 `clone_trampoline`(`clone.asm`)을 거치지 않고 raw syscall을 직접 호출하는 이유는, `clone_trampoline`이 애초에 "child_stack을 넘기는" 저장소 자체 관례(`CLONE_VM` 스레드용)라 fork 흉내를 낼 수 없기 때문이다.

자식은 `%fs:0`을 읽어 TLS가 살아있는지 확인하고 `exit(42)`한다. 부모는 `wait4(-1, &status)`로 자식을 reap해 `status`를 디코딩하고(버그 2 검증), 자식이 없는 상태에서 `wait4(-1, ...)`를 한 번 더 불러 즉시 리턴하는지 확인한다(버그 3 검증).

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
forkclone: fork rc = 0x0000000000000002
forkclone: fork rc = 0x0000000000000000
forkclone: child fs:0 = 0x1234567890ABCDEF
process 2 exited: code=42
forkclone: wait4 pid = 0x0000000000000002 status = 0x0000000000002A00 exitcode = 0x000000000000002A
forkclone: second wait4 (no children) returned = 0x00000000FFFFFFFF
process 1 exited: code=0
```

- `fork rc`가 두 번 찍힌다 — 부모(`0x2`, 자식 pid)와 자식(`0x0`) 둘 다 `fork()` 직후의 같은 코드를 지나가기 때문에 정상이다.
- `child fs:0 = 0x1234567890ABCDEF` — TLS가 fork 자식에서도 살아있음(버그 1 검증).
- `status = 0x2A00`, `exitcode = 0x2A`(=42) — wait4 status가 `WIFEXITED`로 정확히 디코딩됨(버그 2 검증).
- `second wait4 ... = 0xFFFFFFFF` — 자식이 없을 때 즉시 리턴, 블록 없음(버그 3 검증).

`hello`/`hello2`/`brk`/`mmap`/`tls`/`syscall64`/`musl_hello`/`hello | pipe`/`hello | pipe | pipe`/`signal`(`Ctrl+C` 3회)도 회귀 없이 그대로 동작한다 — QEMU HMP 모니터를 유닉스 소켓으로 열고 `sendkey`를 스크립트로 순서대로 넣어 확인했다.

## 이전 단계(48) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/thread.h` | 수정 | `thread_create_with_data(fn, data, pml4_phys, fs_base)` — 스케줄 큐 연결 전에 `pml4_phys`/`fs_base`를 확정하도록 파라미터화; `thread_t.pd` 필드를 `pml4_phys`로 리네임(`paging.c`의 4단계 페이징 네이밍과 통일) |
| `boot/thread.c` | 수정 | `thread_create_with_data` 본문이 파라미터로 받은 `pml4_phys`/`fs_base`를 큐 연결 전에 채움(레이스 제거); `thread_create`는 0으로 위임 |
| `boot/gdt.asm` | 수정 | `enter_user_mode_fork`의 `mov fs,ax` 삭제(FS 셀렉터를 아예 안 건드려 `activate_thread`가 WRMSR해둔 `fs_base`를 보존); `enter_user_mode`는 `mov fs,ax`의 `ax`를 0x23 대신 0(널)으로 — 리눅스 관례와 일관되게 FS 셀렉터를 로드하는 유일한 지점으로 남김 |
| `boot/process.c` | 수정 | `enter_user_mode_fork` 시그니처에서 `fs_base` 인자 제거(더 이상 불필요); `proc_spawn`/`proc_fork`/`proc_clone`이 `thread_create_with_data`에 `pml4_phys`/`fs_base`를 직접 전달; `proc_wait`가 wait4 status를 `<<8`로 인코딩하고 `pid==-1` 대기 시 자식이 없으면 즉시 리턴하는 `has_any_child()` 추가 |
| `user/forkclone.c` | 신규 | 세 버그를 한 번에 재현·검증하는 프로그램 — TLS 설정 후 raw `fork`(57, musl과 동일한 경로)+`wait4` 두 번 |
| `Makefile` | 수정 | `FORKCLONEOBJ`/`USERFORKCLONE` 빌드·링크·initramfs 포함·clean 반영 |
| `initrd/.gitignore` | 수정 | `forkclone` 패턴 추가 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `50-busybox-sh`: 이 단계에서 고친 fork/TLS/wait4 기반 위에서 진짜 musl 정적 바이너리(busybox)를 올린다. 이 세 버그를 여기서 먼저 고쳐두지 않았다면 busybox의 `fork()` 호출 한 번에 커널이 죽거나 셸이 멈췄을 것이다.
- `SYS_CLONE`(56)이 glibc 스타일 fork-via-clone 플래그를 자식 스택 주소로 오인하는 버그는 이번에 다루지 않았다 — musl은 그 경로를 안 타서 필요가 없었다. glibc 정적 바이너리나 `clone`을 직접 쓰는 코드를 올리게 되면 그때 다시 마주친다.
- `proc_clone`(진짜 `CLONE_VM` 스레드 경로)은 지금 `fs_base`를 상속하지 않는다(항상 0으로 시작) — 이 저장소의 `thread_create`가 TLS 없는 스레드만 만들어서 지금은 문제가 안 되지만, 언젠가 TLS를 쓰는 멀티스레드 프로그램(pthread 흉내)을 올리면 같은 클래스의 버그가 재현될 수 있다.
- `has_any_child()`는 `pid==-1` 분기에서만 적용했다 — 특정 pid를 기다리는 분기(`proc_wait(pid, ...)`)는 `proc_get(pid)`가 실패하면 이미 `-1`을 리턴하므로 같은 문제가 없지만, "그 pid가 애초에 내 자식이 맞는지"는 검증하지 않는다(다른 프로세스의 자식을 기다려도 그냥 기다려짐) — 지금은 셸이 항상 자기가 막 fork한 자식만 기다려서 문제가 안 된다.
