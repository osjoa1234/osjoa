# 49 — busybox-sh

**목표**: `chdir(80)`/`access(21)`/`getcwd(79)` syscall을 추가하고, Ubuntu가 배포하는 진짜 `busybox-static` 바이너리(2.1MB, glibc 정적 링크)를 initrd에 넣어 우리 셸에서 `busybox sh`로 실행한다.

**48에서 이어짐**: 지금까지 initrd에 넣은 유저 프로그램은 전부 이 저장소 안에서 우리가 직접 짠 것(`user/*.c`, 커스텀 `user.ld`)이거나 musl로 정적 링크한 `musl_hello` 하나뿐이었다. busybox는 처음으로 다루는 **외부에서 가져온, 우리가 제어하지 않는 대형 바이너리**다 — ELF 세그먼트가 10개(LOAD 4개 + NOTE/TLS/GNU_PROPERTY/GNU_STACK/GNU_RELRO)이고 glibc 정적 초기화 코드가 브레이크아웃 없이 곧장 시작한다. `41-brk`부터 `46-musl-hello`까지 쌓아온 "musl 없이 진짜 libc를 유저 스택에 태운다" 계보의 다음 시험대다.

## 진짜 버그: `phys_mem_init`이 GRUB 모듈(initrd) 영역을 몰랐다

busybox를 처음 `busybox sh`로 실행하자 커널이 `elf_load_process` 안에서 **General Protection Fault**로 즉사했다. `phdr[0]`, `phdr[1]`(1.7MB 텍스트 세그먼트)까지는 정상 출력되다가 `phdr[2]`를 읽으려는 순간 터졌다 — 그런데 `data + e_phoff + i*e_phentsize` 계산 자체는 `i=0,1`에서 이미 검증된 수식이라 `i=2`에서 갑자기 틀릴 이유가 없었다.

원인은 `phys_mem_init`(`phys_mem.c`)이 커널 이미지가 끝나는 지점(`kernel_end`)까지만 물리 페이지를 "사용 중"으로 표시하고, **GRUB이 `initramfs.cpio`를 어디에 로드했는지는 전혀 모른다**는 데 있었다. `mod[0].mod_start`~`mod_end` 범위는 free 비트맵에서 그냥 "비어 있음"으로 보였다. 그래서 `elf_load_process`가 busybox의 1.7MB 텍스트 세그먼트를 유저 페이지에 복사하려고 `page_alloc()`을 430번 가까이 호출하는 동안, **busybox 파일 자신이 아직 담겨 있는 물리 페이지를 되돌려받아 그 위에 0을 채우고 다른 내용을 덮어썼다** — `elf_load_process`가 읽고 있던 `data`(ehdr을 가리키는 포인터)가 가리키는 메모리 자체가 로딩 도중 파괴된 것이다. `musl_hello`(38KB)처럼 작은 파일에서는 `page_alloc()`이 초반 페이지만 조금 소비해 충돌 확률이 낮았을 뿐, 언제든 터질 수 있는 잠재 버그였다.

고친 방법: `phys_mem.c`에 `phys_mem_reserve(u32 start, u32 end)`를 추가해 임의 물리 범위를 사용 중으로 표시할 수 있게 하고, `kernel.c`에서 `phys_mem_init` 직후 — `kheap_init`이나 다른 어떤 `page_alloc()` 호출보다도 먼저 — `mod[0].mod_start`/`mod_end`로 이 함수를 호출해 모듈 범위를 통째로 예약한다. 페이지 얼라인은 `phys_mem_reserve` 내부에서 `start`를 내림, `end`를 올림해서 처리한다.

## 새 syscall: `access`/`chdir`/`getcwd`

- **`sys_access(21)`**: `vfs_open()`으로 열어봐서 성공하면 0, 실패하면 -1. 권한 비트(`mode` 인자)는 애초에 `fstat`도 항상 `0666`을 리턴하는 이 코드베이스에 의미가 없어 무시한다 — 존재 여부만 진짜로 확인한다.
- **`sys_chdir(80)`**: 항상 `0`(성공)을 리턴하는 무조건 성공 스텁이다. `sys_mprotect`가 이미 같은 패턴(파라미터 무시, 항상 성공)이다 — 이 vfs는 애초에 디렉토리 개념이 없는 flat namespace라(`initrd_open`이 이름을 정확히 매치할 뿐 경로 성분을 파싱하지 않는다) "경로가 진짜 디렉토리인지" 검증할 방법이 없다. `cd` builtin이 실패하면 오히려 셸 사용성이 떨어지므로 무조건 성공을 택했다.
- **`sys_getcwd(79)`**: 버퍼에 `"/"`를 써넣고 길이 2를 리턴하는 고정 스텁이다. `chdir`이 아무것도 기록하지 않으므로 이 값은 항상 `"/"`다 — ash의 `pwd`/프롬프트가 빈 문자열 대신 뭔가 의미 있는 값을 보게 하려고 최소한으로 넣었다.

## 진단 도구: `syscall_dispatch` default case에 로그 추가

이 커널에는 `strace` 같은 게 없다. busybox처럼 우리가 소스를 안 짠 대형 바이너리를 처음 올릴 때 정확히 어떤 syscall이 몇 번 빠졌는지 알 방법이 없어서, `syscall.c`의 `syscall_dispatch` default case에 `console_printf("syscall: unimplemented rax=%u\n", ...)`를 영구적으로 추가했다. `busybox sh` 최초 실행 시 이 로그로 확인된 미구현 syscall 목록:

| rax | syscall | 처리 |
|-----|---------|------|
| 79  | getcwd | 이번에 구현 |
| 110 | getppid | 미구현(-1), glibc가 실패를 감내 |
| 157 | prctl | 미구현(-1) |
| 228 | clock_gettime | 미구현(-1), glibc 초기화 중 2회 호출되지만 치명적이지 않음 |
| 267 | readlinkat | 미구현(-1), `/proc/self/exe` 조회 실패로 추정 — `/proc`이 아예 없음 |
| 273 | set_robust_list | 미구현(-1), glibc 스레딩 초기화 상수 호출, 실패해도 무시됨 |
| 302 | prlimit64 | 미구현(-1) |
| 318 | getrandom | 미구현(-1), 스택 카나리/ASLR 시드용 — 실패 시 glibc가 폴백 |
| 334 | rseq | 미구현(-1), glibc 2.35+가 등록 시도하지만 ENOSYS면 그냥 비활성화 |

전부 "실패해도 glibc/ash가 graceful하게 넘어가는" 종류라 `busybox sh`가 정상 기동해 `echo`/`pwd`/`cd`/`exit` 같은 builtin을 문제없이 처리한다.

## 범위 밖: `ls`는 커널 전체를 멈춘다 (getdents 없음)

`busybox sh`에서 `ls`를 실행하면 유저 모드에서 페이지 폴트(`cr2` 근처 NULL 역참조)가 나고 **커널 전체가 halt** 된다. 원인은 두 가지가 겹친 것이다:

1. 이 vfs에는 디렉토리 나열(`getdents`) 자체가 없다 — `51-vfs-ext`에서 다룰 예정.
2. **유저 모드 폴트가 나면 프로세스만 죽는 게 아니라 커널 전체가 죽는다.** `interrupts.c`의 `handle_exception`은 vector가 3(`int3`)이나 4(`overflow`)가 아니면 무조건 `halt_after_exception()`으로 시스템을 정지시킨다 — 폴트를 일으킨 게 커널 코드인지 링3 유저 프로세스인지 구분하지 않는다. 이건 busybox 특유의 문제가 아니라 **이 프로젝트 전체가 처음부터 안고 있던 한계**다(지금까지 모든 유저 프로그램이 우리가 짠 것이라 애초에 폴트를 일으키지 않았을 뿐). 그래서 이번 완료 기준에서 `ls`는 의도적으로 제외했다.

## 명령

```bash
make            # build/os.iso 생성 (busybox-static이 /usr/bin/busybox에 설치돼 있어야 함: `make -C .. setup-busybox`)
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 이전과 동일하게 `$` 프롬프트에서 멈춘다(`initramfs: N file(s) found`가 11→12로 늘어난다, `busybox` 바이너리 추가). GUI(`make run`)에서:

```
$ busybox sh
syscall: unimplemented rax=273
syscall: unimplemented rax=334
syscall: unimplemented rax=302
syscall: unimplemented rax=267
syscall: unimplemented rax=318
syscall: unimplemented rax=228
syscall: unimplemented rax=228
syscall: unimplemented rax=157
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

`hello`/`hello2`/`brk`/`mmap`/`tls`/`syscall64`/`musl_hello`/`hello | pipe`/`hello | pipe | pipe`/`signal`(`Ctrl+C` 3회)도 회귀 없이 그대로 동작한다 — QEMU HMP 모니터를 유닉스 소켓으로 열고 `sendkey`를 스크립트로 순서대로 넣어, `-debugcon file:...`로 받은 로그(포트 `0xE9`는 `console.c`가 VGA 출력과 함께 항상 미러링한다)로 확인했다.

## 이전 단계(48) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/phys_mem.h` | 수정 | `phys_mem_reserve()` 선언 추가 |
| `boot/phys_mem.c` | 수정 | `phys_mem_reserve(start, end)` 구현 — 임의 물리 범위를 사용 중으로 표시 |
| `boot/kernel.c` | 수정 | `phys_mem_init` 직후 GRUB 모듈(`mod[0].mod_start`~`mod_end`) 범위를 `phys_mem_reserve`로 예약 — initrd 로딩 중 자기 자신의 물리 페이지가 재할당되는 버그 수정 |
| `boot/syscall.h` | 수정 | `SYS_ACCESS`(21)/`SYS_CHDIR`(80)/`SYS_GETCWD`(79) 추가 |
| `boot/syscall.c` | 수정 | `sys_access`/`sys_chdir`/`sys_getcwd` 구현과 디스패치 케이스; default case에 미구현 syscall 진단 로그 추가 |
| `Makefile` | 수정 | `BUSYBOXSRC`(`/usr/bin/busybox`, 호스트 apt 설치 바이너리)를 `initrd/busybox`로 복사해 initramfs에 포함; `tools`가 `setup-busybox`도 호출 |
| `initrd/.gitignore` | 수정 | `busybox` 패턴 추가 |
| `../Makefile` | 수정 | `setup-busybox` 타깃(`busybox-static` apt 설치) 추가, `setup`이 이를 포함 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `ls`, `cat 없는파일`, 디렉토리 관련 busybox applet은 전부 `getdents` 부재로 실패한다 — `51-vfs-ext`에서 `getdents`/`mkdir`/`unlink`를 붙일 때 같이 검증.
- **유저 모드 폴트가 커널 전체를 halt시키는 문제**가 이번에 `ls` 크래시로 다시 드러났다 — `interrupts.c`의 `handle_exception`이 폴트 발생 시 CS 셀렉터(또는 별도 플래그)로 "링3에서 왔는지"를 판별해, 유저 모드 폴트는 `SIGSEGV`를 현재 프로세스에 전달하고 `proc_exit`으로 그 프로세스만 죽이도록 바꿔야 한다. `47-signal`의 시그널 인프라가 이미 있으니 재사용 가능 — 별도 단계로 분리할 가치가 있다.
- `getppid`/`prctl`/`clock_gettime`/`readlinkat`/`getrandom`/`set_robust_list`/`prlimit64`/`rseq`는 지금 전부 `-1` 스텁이다. busybox의 다른 applet(예: `date`, `sleep`)을 시험해보려면 `clock_gettime`/`nanosleep` 계열부터 채워야 할 것이다.
- `busybox`가 만드는 자식 프로세스(예: 외부 명령 실행)는 PATH에 아무것도 없어 전부 "not found"가 난다 — initrd가 flat namespace이고 `$PATH` 개념도 없기 때문. 여러 유틸리티를 실제로 실행하려면 `/bin/busybox` 같은 경로 규칙과 심볼릭 링크(busybox의 applet 멀티콜 관행)를 initrd에 흉내 내야 한다.
