# 50 — busybox-sh

**목표**: `access(21)`/`chdir(80)`/`getcwd(79)` syscall을 추가하고, [busybox.net이 배포하는 musl 정적 바이너리](https://www.busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox)(1.1MB, musl 1.2.2 정적 링크)를 initrd에 넣어 우리 셸에서 `busybox sh`로 실행한다.

**49에서 이어짐**: 지금까지 initrd에 넣은 유저 프로그램은 전부 이 저장소 안에서 우리가 직접 짠 것(`user/*.c`, 커스텀 `user.ld`)이거나 musl-gcc로 정적 링크한 `musl_hello`뿐이었다. busybox는 처음으로 다루는 **외부에서 가져온, 우리가 제어하지 않는 대형 바이너리**다 — ELF 세그먼트가 여러 개(LOAD 다수 + NOTE/GNU_STACK 등)이고 musl 정적 초기화 코드가 브레이크아웃 없이 곧장 시작한다. `41-brk`부터 `46-musl-hello`까지 쌓아온 "musl 없이 진짜 libc가 기대하는 유저 스택을 태운다" 계보, 그리고 `49-fork-clone-fix`에서 미리 고쳐둔 "musl이 실제로 쓰는 `fork()`/TLS/`wait4`" 기반 위에서 처음으로 완전한 외부 바이너리를 시험하는 자리다.

## glibc가 아니라 musl을 쓰는 이유

Ubuntu apt의 `busybox-static` 패키지는 glibc 정적 링크다 — 처음 이 실습을 시도했을 때 그걸 썼더니 `49`에서 고친 버그들이 "glibc의 fork-via-clone" 관점으로 잘못 서술됐다. 하지만 그 관점에서 필요했던 `SYS_CLONE` 플래그 오인 수정은 **musl에는 적용되지 않는다** — musl은 raw `fork`(57)를 그대로 쓰기 때문이다(`49-fork-clone-fix/CLAUDE.md` 참고). 이 학습 계보(41~46)가 애초에 "musl이 기대하는 ABI"를 목표로 쌓아온 것과 일관되게, 여기서도 musl 정적 바이너리를 쓴다. apt에는 musl 빌드 busybox 패키지가 없어서 [busybox.net 공식 바이너리 배포](https://www.busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/)에서 직접 받는다.

`Makefile`의 `BUSYBOXSRC`는 `~/.cache/custom-os/busybox-musl`을 가리키고, 최상위 `Makefile`의 `setup-busybox`가 `curl`로 그 경로에 받아둔다(`sudo apt install`이 아니라 `curl` 한 번이다 — 실행 권한만 있으면 되는 static 바이너리라 apt/sudo가 필요 없다):

```bash
make -C .. setup-busybox   # 또는 make tools
```

## 진짜 버그: `phys_mem_init`이 GRUB 모듈(initrd) 영역을 몰랐다

busybox를 처음 `busybox sh`로 실행하자 `rip`/`cr2`가 전혀 말이 안 되는 값(`0xE80050B7E4000014`처럼 물리 주소 범위를 한참 벗어난 값)으로 페이지 폴트가 나며 즉사했다. 원인은 `phys_mem_init`(`phys_mem.c`)이 커널 이미지가 끝나는 지점(`kernel_end`)까지만 물리 페이지를 "사용 중"으로 표시하고, **GRUB이 `initramfs.cpio`를 어디에 로드했는지는 전혀 모른다**는 데 있었다. `mod[0].mod_start`~`mod_end` 범위는 free 비트맵에서 그냥 "비어 있음"으로 보였다. 그래서 `elf_load_process`가 busybox의 큰 텍스트 세그먼트를 유저 페이지에 복사하려고 `page_alloc()`을 수백 번 호출하는 동안, **busybox 파일 자신이 아직 담겨 있는 물리 페이지를 되돌려받아 그 위에 0을 채우고 다른 내용을 덮어썼다** — `elf_load_process`가 읽고 있던 `data`(ehdr을 가리키는 포인터)가 가리키는 메모리 자체가 로딩 도중 파괴된 것이다. `musl_hello`(38KB)처럼 작은 파일에서는 `page_alloc()`이 초반 페이지만 조금 소비해 충돌 확률이 낮았을 뿐, 언제든 터질 수 있는 잠재 버그였다 — **이건 musl이든 glibc든 상관없는, 큰 외부 바이너리를 처음 로드할 때만 드러나는 버그다.**

고친 방법: `phys_mem.c`에 `phys_mem_reserve(u32 start, u32 end)`를 추가해 임의 물리 범위를 사용 중으로 표시할 수 있게 하고, `kernel.c`에서 `phys_mem_init` 직후 — `kheap_init`이나 다른 어떤 `page_alloc()` 호출보다도 먼저 — `mod[0].mod_start`/`mod_end`로 이 함수를 호출해 모듈 범위를 통째로 예약한다. 페이지 얼라인은 `phys_mem_reserve` 내부에서 `start`를 내림, `end`를 올림해서 처리한다.

## 새 syscall: `access`/`chdir`/`getcwd`

- **`sys_access(21)`**: `vfs_open()`으로 열어봐서 성공하면 0, 실패하면 -1. 권한 비트(`mode` 인자)는 애초에 `fstat`도 항상 고정값을 리턴하는 이 코드베이스에 의미가 없어 무시한다 — 존재 여부만 진짜로 확인한다.
- **`sys_chdir(80)`**: 항상 `0`(성공)을 리턴하는 무조건 성공 스텁이다. `sys_mprotect`가 이미 같은 패턴(파라미터 무시, 항상 성공)이다 — 이 vfs는 애초에 디렉토리 개념이 없는 flat namespace라(`initrd_open`이 이름을 정확히 매치할 뿐 경로 성분을 파싱하지 않는다) "경로가 진짜 디렉토리인지" 검증할 방법이 없다. `cd` builtin이 실패하면 오히려 셸 사용성이 떨어지므로 무조건 성공을 택했다.
- **`sys_getcwd(79)`**: 버퍼에 `"/"`를 써넣고 길이 2를 리턴하는 고정 스텁이다. `chdir`이 아무것도 기록하지 않으므로 이 값은 항상 `"/"`다 — ash의 `pwd`/프롬프트가 빈 문자열 대신 뭔가 의미 있는 값을 보게 하려고 최소한으로 넣었다.

## 진단 도구: `syscall_dispatch` default case에 로그 추가

이 커널에는 `strace` 같은 게 없다. busybox처럼 우리가 소스를 안 짠 대형 바이너리를 처음 올릴 때 정확히 어떤 syscall이 몇 번 빠졌는지 알 방법이 없어서, `syscall.c`의 `syscall_dispatch` default case에 `console_printf("syscall: unimplemented rax=%u\n", ...)`를 영구적으로 추가했다. `busybox sh` 최초 실행 시 이 로그로 확인된 미구현 syscall:

| rax | syscall | 언제 | 처리 |
|-----|---------|------|------|
| 110 | getppid | `sh` 시작 직후 | 미구현(-1), musl이 실패를 감내 |
| 186 | gettid | 외부 명령을 `fork`+`exec`할 때 | 미구현(-1), musl 내부 캐시 갱신 시도지만 실패해도 무시됨 |

glibc 정적 바이너리 때 나타났던 `prctl`/`clock_gettime`/`readlinkat`/`set_robust_list`/`prlimit64`/`getrandom`/`rseq` 같은 긴 목록은 musl에서는 안 나온다 — musl의 초기화 경로가 glibc보다 훨씬 가볍기 때문이다. 두 syscall 다 "실패해도 musl/ash가 graceful하게 넘어가는" 종류라 `busybox sh`가 정상 기동해 `echo`/`pwd`/`cd`/`exit` 같은 builtin을 문제없이 처리한다.

## `./hello`: exec 실패는 정상적으로 처리되지만 에러 메시지는 틀렸다

`busybox sh` 프롬프트에서 `./hello`처럼 **외부 명령을 실행**시키면(=ash가 실제로 `fork()`+`execve()`를 호출하면) `49-fork-clone-fix`에서 고친 fork/TLS/wait4 경로를 실제로 타게 된다. 그 기반이 없었다면 이 한 줄만으로 커널이 죽거나(page fault) 셸이 영원히 멈췄을 것이다 — 지금은 다음처럼 정상적으로(그러나 명령을 못 찾은 채) 넘어간다:

```
$ busybox sh
...
./hello
sh: ./hello: Operation not permitted
process 2 exited: code=126
```

크래시도, 행(hang)도 없다. 다만 `Operation not permitted`(EPERM) 자체는 틀린 메시지다 — `proc_exec`의 `SYS_EXECVE` 핸들러가 실패 사유를 구분 안 하고 항상 raw `-1`을 리턴해서, musl이 `-errno` 관례로 해석할 때 무조건 `errno=1`(EPERM)이 된다. 진짜 원인은 `initrd_open`이 `"./hello"`를 못 찾은 것(`ENOENT`가 맞는 값)이다 — `initrd_open`이 저장된 cpio 이름의 `"./"` 접두어만 벗기고 질의(query) 쪽의 `"./"`는 벗기지 않아서, `hello`는 찾아도 `./hello`는 못 찾는다. 다음 단계 힌트로 남긴다.

## 범위 밖: `ls`는 커널 전체를 멈춘다 (getdents 없음)

`busybox sh`에서 `ls`를 실행하면 유저 모드에서 페이지 폴트가 나고 **커널 전체가 halt** 된다. 원인은 두 가지가 겹친 것이다:

1. 이 vfs에는 디렉토리 나열(`getdents`) 자체가 없다 — `52-vfs-ext`에서 다룰 예정.
2. **유저 모드 폴트가 나면 프로세스만 죽는 게 아니라 커널 전체가 죽는다.** `interrupts.c`의 `handle_exception`은 vector가 3(`int3`)이나 4(`overflow`)가 아니면 무조건 `halt_after_exception()`으로 시스템을 정지시킨다 — 폴트를 일으킨 게 커널 코드인지 링3 유저 프로세스인지 구분하지 않는다. 이건 busybox 특유의 문제가 아니라 **이 프로젝트 전체가 처음부터 안고 있던 한계**다. 이번 완료 기준에서 `ls`는 의도적으로 제외했다.

## 명령

```bash
make            # build/os.iso 생성 (musl busybox가 ~/.cache/custom-os/busybox-musl에 있어야 함: `make -C .. setup-busybox`)
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 이전과 동일하게 `$` 프롬프트에서 멈춘다(`initramfs: N file(s) found`가 12→13으로 늘어난다, `busybox` 바이너리 추가). GUI(`make run`)에서:

```
$ busybox sh
syscall: unimplemented rax=110
echo hello from busybox
hello from busybox
pwd
/
cd /
exit
process 1 exited: code=0
$
```

## 이전 단계(49) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/phys_mem.h` | 수정 | `phys_mem_reserve(u32 start, u32 end)` 선언 추가 |
| `boot/phys_mem.c` | 수정 | `phys_mem_reserve` 구현 — 임의 물리 범위를 페이지 정렬 후 사용 중으로 표시 |
| `boot/kernel.c` | 수정 | `phys_mem_init` 직후, `kheap_init` 이전에 `phys_mem_reserve(mod0[0].mod_start, mod0[0].mod_end)` 호출 — GRUB 모듈 영역 예약 |
| `boot/syscall.h` | 수정 | `SYS_ACCESS`(21)/`SYS_GETCWD`(79)/`SYS_CHDIR`(80) 추가 |
| `boot/syscall.c` | 수정 | `sys_access`/`sys_chdir`/`sys_getcwd` 구현과 디스패치 케이스 추가; default case에 미구현 syscall 진단 로그 추가 |
| `Makefile` | 수정 | `BUSYBOXSRC`(`~/.cache/custom-os/busybox-musl`) initramfs 포함·clean 반영; `tools`가 `setup-busybox` 의존 |
| `initrd/.gitignore` | 수정 | `busybox` 패턴 추가 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `initrd_open`이 `"./이름"` 형태의 질의를 처리하지 못한다(`"./"` 접두어를 벗기지 않음) — `execve`가 항상 `ENOENT`를 `EPERM`으로 오보하는 문제와 함께 다음 어딘가에서 고칠 만하다.
- `ls`/`getdents`는 `52-vfs-ext`에서 다룬다. 그때는 유저 모드 폴트가 프로세스 하나만 죽이도록(`interrupts.c`의 `handle_exception`이 `cs`로 커널/유저를 구분하도록) 손보는 것도 같이 고려할 만하다 — `ls` 자체가 그 한계에 정확히 걸리는 사례라서.
- `SYS_CLONE`(56)의 glibc fork-via-clone 오인 버그는 여전히 고쳐지지 않았다(`49-fork-clone-fix` 참고) — musl busybox로는 절대 안 걸리지만, 언젠가 glibc 바이너리나 `clone`을 직접 쓰는 프로그램을 올리면 그때 다시 마주친다.
