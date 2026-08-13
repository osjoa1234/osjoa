# 46 — musl-hello

**목표**: musl-gcc로 `-static` 빌드한 진짜 외부 바이너리(`musl_hello`)를 initrd에 넣어 `init` 셸에서 실행한다. 44에서 진짜 syscall 진입 경로를, 45에서 진짜 초기 스택 레이아웃을 갖췄으니, 이제 손수 만든 검증 프로그램이 아니라 musl이 스스로 만들어낸 진짜 초기화 코드를 붙여본다.

**45에서 이어짐**: 45까지는 커널이 실제 ABI를 "말할 줄 안다"는 것만 손수 만든 프로그램(`syscall64.c`)으로 확인했다. 이번 단계는 그게 **진짜 낯선 코드** 앞에서도 맞는지 확인하는 단계다 — musl_hello가 실제로 어떤 syscall을 어떤 순서로 부르는지는 추측하지 않고 관찰했다. musl 정적 바이너리는 리눅스 syscall만 쓰는 일반 ELF64라서 **호스트 리눅스 위에서 그대로 실행된다** — 크로스 에뮬레이션 없이, `ptrace(PTRACE_SYSCALL)`로 만든 20줄짜리 `mini_strace`로 직접 관찰해서 이 표를 얻었다:

| 번호 | 이름 | 언제 | 44/45에 이미 있었나 |
|------|------|------|------|
| 158 | `arch_prctl` | `__init_tp`가 TLS 포인터를 세움 | 있음 (44) |
| 218 | `set_tid_address` | 같은 초기화 경로, tid 슬롯 등록 | 없음 → 이번에 추가 |
| 12 | `brk` | 작은 `malloc` | 없음 → 이번에 추가 |
| 9 / 11 | `mmap` / `munmap` | 큰 `malloc`/`free`(mallocng의 mmap 기반 청크) | 없음 → 이번에 추가 |
| 16 | `ioctl` | `TCGETS`로 stdout이 tty인지 확인(버퍼링 모드 결정) | 없음 → 이번에 추가 |
| 20 | `writev` | `printf`/`puts`의 실제 출력 | 없음 → 이번에 추가 |
| 39 / 102 / 63 | `getpid`/`getuid`/`uname` | 유저 코드가 명시적으로 부름 | 있음 (44) |
| 231 | `exit_group` | `return`/`exit()` | 있음 (44) |

`fstat`/`mprotect` 등은 이번 프로그램에서 전혀 안 불렸다 — 필요해지면 그때 `stat_t` 레이아웃을 맞추기로 하고 지금은 손대지 않았다.

## 핵심 개념

### 44/45에서 미리 깔아둔 배관 위에 필요한 syscall만 얹는다

`syscall64_dispatch`(44에서 신설)는 이미 `write`/`arch_prctl`/`getpid`/`getuid`/`uname`/`exit_group`을 실제 번호로 연결해뒀고, `elf_setup_stack`(45에서 신설)은 이미 `argv[0]`/`auxv`를 정확히 채워준다. 이번 단계에서 새로 만든 건 `sys_writev`/`sys_ioctl`/`sys_set_tid_address`/`sys_munmap`/`sys_mmap64` 다섯 개뿐이다:

- `sys_writev` — `iovec_t{base,len}` 배열을 순회하며 기존 `sys_write`를 반복 호출.
- `sys_ioctl` — 항상 `-ENOTTY`(`-25`). 리퀘스트 코드 자체는 안 본다 — musl이 "tty 아님"으로 알고 버퍼링 모드를 결정하기만 하면 된다.
- `sys_set_tid_address` — 저장하지 않고 `pid`만 돌려준다. 단일 스레드 프로그램에는 그걸로 충분하다.
- `sys_munmap` — `proc_mmap`이 하던 것과 반대로, 페이지별로 `paging_unmap_user_page`(내부에서 `page_free`까지 함)를 돌 뿐이다. `mmap_next`는 여전히 한쪽으로만 내려가는 bump allocator라 해제된 구간을 재사용하지 않으므로 이 정도로 충분하다.
- `sys_mmap64` — 진짜 `mmap(addr,length,prot,flags,fd,offset)` 6개 인자를 받아 `addr`/`fd`/`offset`은 버리고 기존 `proc_mmap(length,prot,flags)`에 위임한다.

`brk`/`mprotect`도 새 번호(`SYS64_BRK=12`, `SYS64_MPROTECT=10`)로 다시 연결했지만 핸들러 자체(`proc_brk`, `sys_mprotect`)는 41/42에서 만든 그대로다.

### 나머지는 전부 검증이었다

`musl_hello.c`는 새 코드가 아니라 41(`brk`)·42(`mmap`)·43(`arch_prctl`/`getpid`/`getuid`/`uname`)·44(syscall 진입)·45(argv/auxv)가 실제로 다 맞는지 한 번에 확인하는 프로그램이다: `printf`(초기화+writev), 작은 `malloc`/`free`(brk), 큰 `malloc`/`free`(mmap/munmap), `getpid`/`getuid`/`uname`(진짜 syscall 번호로). 여기서 문제가 하나라도 났다면 44/45가 아니라 이 앞 단계들의 가정이 잘못됐다는 뜻이었을 텐데, 실제로는 깔끔하게 통과했다 — 44/45에서 미리 페이지 정렬·SSE·argv0 use-after-free를 잡아둔 게 여기서 값을 했다.

## 명령

```bash
# (최초 1회) musl-tools 설치 — 상위 디렉토리에서
make -C .. setup-musl

make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 45와 동일하게 `$` 프롬프트에서 멈춘다(`initramfs: 10 file(s) found`로 파일 수만 9→10). GUI(`make run`)에서 `musl_hello`를 입력하면:

```
$ musl_hello
musl_hello: hello from real musl -- argc=1 argv[0]=musl_hello
musl_hello: brk malloc ok
musl_hello: mmap malloc ok ptr=0xfc5030
musl_hello: getpid = 1
musl_hello: getuid = 0
musl_hello: uname sysname = custom-os
musl_hello: uname release = 0.43.0
process 1 exited: code=0
$
```

`hello`/`hello2`/`brk`/`mmap`/`tls`/`syscall64`도 그대로 동작해야 한다(회귀 없음) — QEMU 모니터의 `sendkey`를 스크립트로 순서대로 넣어 확인했다.

## 이전 단계(45) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/syscall.h` | 수정 | `SYS64_*`에 `MMAP`/`MPROTECT`/`MUNMAP`/`BRK`/`IOCTL`/`WRITEV`/`SET_TID_ADDRESS`/`READ`/`CLOSE` 추가 |
| `boot/syscall.c` | 수정 | `sys_writev`/`sys_ioctl`/`sys_set_tid_address`/`sys_munmap`/`sys_mmap64` 추가, `syscall64_dispatch`에 해당 케이스 연결(`brk`/`mprotect`는 기존 핸들러 재사용) |
| `user/musl_hello.c` | 신규 | musl-gcc로 빌드하는 진짜 외부 바이너리 — `printf`, 작은/큰 `malloc`/`free`, `getpid`/`getuid`/`uname` |
| `Makefile` | 수정 | `musl-gcc` 툴체인 변수, `musl_hello`를 `-static -O2 -s`로 별도 빌드해 initrd에 포함, `tools`에 `setup-musl` 추가 |
| `initrd/.gitignore` | 수정 | `musl_hello` 패턴 추가 |
| `../Makefile` | 수정 | `MUSL_PKGS`/`setup-musl` 타겟 추가 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `47-signal`: per-process 시그널 핸들러 테이블, 유저 공간 트램폴린 + `sigreturn`, Ctrl+C→SIGINT. `syscall64_dispatch`에 `rt_sigaction`/`rt_sigprocmask`(진짜 번호 13/14)가 새로 필요해질 것 — musl의 시그널 관련 초기화가 그 시점에 처음 걸린다.
- 스레드 전환 시 XMM/FPU 상태를 저장·복원하지 않는다(45에서 SSE는 켰지만 컨텍스트 스위치 시 보존은 안 함) — 지금은 SSE 쓰는 유저 프로세스가 동시에 하나만 돈다는 전제로 넘어갔다. `fork`/`clone`으로 SSE 쓰는 프로그램이 여럿 동시에 돌기 시작하면 `context_switch.asm`에 `fxsave`/`fxrstor` 영역을 추가해야 한다.
- `sys_ioctl`은 요청 코드를 안 보고 항상 `-ENOTTY`다 — busybox 등 진짜 터미널 제어가 필요한 바이너리를 붙이면(`49-busybox-sh`) `TIOCGWINSZ` 같은 요청을 구분해야 할 것.
- `stat_t`/`fstat`은 여전히 이 커널 자체 정의 레이아웃이고 `SYS64_FSTAT`을 아예 연결하지 않았다 — `musl_hello`가 안 썼기 때문. 실제로 `fstat`을 쓰는 바이너리가 오면 진짜 `struct stat`(144바이트) 레이아웃에 맞춰야 한다.
