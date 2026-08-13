# 44 — syscall64

**목표**: x86_64의 진짜 syscall 진입 경로 — `syscall` 명령 + SYSCALL/SYSRET MSR — 를 도입하고, 지금까지 `int 0x80` + i386 번호로 짜여 있던 모든 것(`init`/`hello`/`hello2`/`brk`/`mmap`/`tls`, 커널의 `syscall_dispatch`, `clone_trampoline`)을 이 경로 하나로 완전히 옮긴다. `int 0x80`은 남기지 않는다 — IDT의 `0x80` 게이트, i386 레지스터 관례(`rbx/rcx/rdx`), i386 syscall 번호 전부 삭제했다. 실제 x86_64 Linux ABI는 애초에 `int 0x80`을 안 쓰고 `syscall` 명령 하나만 쓴다. musl 같은 진짜 libc를 붙이려면(46) 이 경로가 유일한 진입점이어야 한다.

**43에서 이어짐**: 43은 `tls.c`로 `arch_prctl`/`getpid`/`getuid`/`uname`을 `int 0x80` + i386 번호로 검증했다. 처음엔 `syscall` 경로를 `int 0x80` 옆에 "추가"하는 걸로 시작했지만(별도 `SYS64_*` 번호, 별도 `syscall64_dispatch`), 유저 프로그램을 하나씩 옮기다 보니 두 번호 체계·두 dispatch 함수를 나란히 유지할 이유가 없었다 — 최종적으로 `syscall.h`의 enum은 하나(`SYS_*`, 전부 진짜 x86_64 번호), dispatch 함수도 하나(`syscall_dispatch`)로 합쳤다.

## 핵심 개념

### SYSCALL/SYSRET MSR — 진입은 MSR로, 복귀는 기존 iretq로

x86_64 `syscall` 명령은 소프트웨어 인터럽트와 완전히 별개의 하드웨어 경로다. `IA32_EFER`(`0xC0000080`)의 SCE 비트를 켜야 명령 자체가 동작하고, `IA32_LSTAR`(`0xC0000082`)에 진입 핸들러 주소를, `IA32_STAR`(`0xC0000081`)의 47:32비트에 커널 CS(`0x08`)를 넣는다 — SYSCALL 진입 시 CPU가 `SS = STAR[47:32]+8`을 자동으로 쓰는데, 이미 있는 커널 CS(`0x08`)/DS(`0x10`) 셀렉터와 정확히 맞아떨어져서 GDT를 새로 짤 필요가 없었다. `gdt.c`의 `gdt_enable_syscall`이 이 세 MSR을 쓴다 — `gdt_set_fs_base`와 같은 "GDT/세그먼트 관련 CPU 상태" 자리에 얹었다.

복귀는 `sysretq`를 쓰지 않는다. `sysretq`는 `STAR`의 63:48비트로 `SS=STAR[63:48]+8`, `CS=STAR[63:48]+16` 순서를 강제하는데, 지금 GDT 순서(코드 `0x18`, 데이터 `0x20`, TSS `0x28`)로는 이 공식을 만족하는 값이 없다(어떤 값을 넣어도 TSS 셀렉터와 겹치거나 존재하지 않는 디스크립터를 가리킨다). GDT를 재배열하는 대신, `syscall_entry.asm`은 진입 시 `rcx`(복귀 RIP)·`r11`(복귀 RFLAGS)·저장해둔 유저 RSP로 `interrupt_frame`과 완전히 같은 모양의 스택 프레임을 손으로 만들고 **`interrupt_common`과 동일한 GPR push/pop + `iretq`** 로 되돌아간다. 셀렉터는 기존 `enter_user_mode`가 쓰던 `0x1B`(유저 코드)/`0x23`(유저 데이터) 리터럴을 그대로 쓴다 — `sysretq`의 제약을 아예 우회하는 선택이다.

`syscall`은 스택을 자동으로 안 바꾼다(인터럽트와 달리 TSS.rsp0을 참조하지 않는다). 그래서 진입하자마자 `syscall_kernel_rsp`(전역 변수, `syscall_entry.asm`에 정의)로 커널 스택으로 직접 스위칭한다. 이 값은 `thread.c`의 `activate_thread`가 `gdt_set_kernel_stack`(TSS.rsp0)과 나란히 매 스레드 전환마다 갱신한다.

### IA32_FMASK와 `sti`/`cli` — blocking read가 멈췄던 이유

`gdt_enable_syscall`은 `IA32_FMASK`에 `0x200`(RFLAGS의 IF 비트)을 넣는다. `syscall` 진입 시 CPU가 `RFLAGS &= ~FMASK`를 자동으로 해서 IF를 끈다 — 예전 `int 0x80`(인터럽트 게이트)과 같은 효과다. `init` 셸의 blocking `read`(`keyboard_getchar`의 `hlt` 루프)는 키보드 IRQ1이 들어와야 깨어나는데, IF가 꺼진 채로 `syscall_dispatch` 안에서 이 루프에 들어가면 IRQ가 영원히 안 들어와 멈춘다. 그래서 `syscall_entry.asm`은 `call syscall_dispatch` 앞뒤를 `sti`/`cli`로 감싼다 — 예전 `interrupt_dispatch`가 `int 0x80`(vector `0x80`)에서 하던 것과 정확히 같은 처리를, 이제는 그 vector 분기 자체가 없어졌으니 `syscall_entry.asm` 쪽으로 옮겨온 것이다.

### syscall_dispatch — 단일 enum, 단일 진입점, 기존 핸들러 재사용

`syscall.h`의 `SYS_*` enum은 전부 진짜 x86_64 Linux 번호다: `read=0`, `write=1`, `open=2`, `close=3`, `fstat=5`, `lseek=8`, `mmap=9`, `mprotect=10`, `brk=12`, `getpid=39`, `clone=56`, `fork=57`, `execve=59`, `exit=60`, `wait4=61`, `uname=63`, `getuid=102`, `arch_prctl=158`, `exit_group=231`. 인자 레지스터는 `rdi/rsi/rdx/r10/r8/r9`(4번째 인자가 `rcx`가 아니라 `r10`인 이유는 `syscall` 명령 자체가 `rcx`를 복귀주소로 덮어써서 인자로 못 쓰기 때문). `mmap`(9)은 진짜 리눅스 6-인자 시그니처(`addr, length, prot, flags, fd, offset`)를 받지만 커널의 `proc_mmap`은 `length`/`prot`/`flags`만 쓰므로 `addr`/`fd`/`offset`은 유저 쪽에서 넘겨도 커널에서 무시한다. `r10`/`r8` 인자는 GCC 인라인 어셈블리 제약 문자(`b`/`c`/`d`/`S`/`D`)로 직접 못 넣으므로 `register unsigned long r10 __asm__("r10") = flags;` 형태의 명시적 레지스터 변수로 강제 배치했다(`-std=c11`이라 `asm` 키워드가 아니라 `__asm__`을 써야 컴파일된다).

`exit`(60)과 `exit_group`(231)은 진짜 리눅스처럼 의미가 다르다 — `exit`는 **현재 스레드만** 끝내고(다른 스레드가 남아 있으면 프로세스는 안 죽는다), `exit_group`은 **프로세스 전체**를 강제 종료한다. `proc_thread_exit()`(마지막 스레드면 알아서 `proc_exit()`로 넘어가고, 아니면 이 스레드만 정리)가 정확히 `exit`의 의미와 같고 `proc_exit()`가 `exit_group`과 같아서, 예전에 따로 있던 `SYS_THREAD_EXIT`(201, 커스텀 번호) 같은 enum 항목이 필요 없어졌다 — `clone_trampoline`(스레드 진입점)이 스레드 종료 시 부르던 번호도 `exit`(60)로 바뀌었다. `clone`(56)도 마찬가지로 커스텀 번호(120) 대신 진짜 번호를 그대로 재사용한다 — 이 커널의 `clone` 인자 관례(새 스레드의 유저 스택 포인터 하나만 받음)는 진짜 리눅스 `clone(2)`와 다르지만, 번호 자체는 다른 syscall과 안 겹치기만 하면 되므로 굳이 새 번호를 만들지 않았다.

`SYS_SPAWN`(옛 200번, 커스텀)은 아예 없앴다 — 어떤 유저 프로그램도 부르지 않는 죽은 코드였다(`init`은 셸에서 부팅 시 커널이 `proc_spawn("init")`을 직접 호출할 뿐, 유저 코드가 syscall로 부르는 경로가 없었다). `proc_spawn` 함수 자체는 `kernel.c`가 여전히 직접 호출하므로 그대로 있다.

### interrupts.c — `int 0x80` 벽 전체 삭제

`interrupts_init()`에서 `idt_set_entry(0x80U, ...)` 줄을 지웠다 — IDT에 `0x80` 게이트가 아예 없다. `interrupt_dispatch()`의 `if (frame->vector == 0x80U) { sti; syscall_dispatch(frame); cli; }` 분기도 지웠다 — `syscall_dispatch`는 이제 인터럽트 벡터가 아니라 `syscall_entry.asm`(MSR 경로)에서만 불린다. 지금 유저 코드가 `int $0x80`을 실행하면 IDT에 게이트가 없으므로 일반 보호 예외(`#GP`)가 뜬다.

### 유저 프로그램 — 전부 syscall 경로로

`init`/`hello`/`hello2`/`brk`/`mmap`/`tls` 여섯 프로그램 전부 같은 패턴: `__asm__("int $0x80" ...)` → `__asm__("syscall" ...)`, 레지스터 제약을 `a/b/c/d`(rax/rbx/rcx/rdx)에서 `a/D/S/d`(+필요하면 `r10`/`r8`)로, syscall 번호를 i386 값에서 통합된 `SYS_*`(실제 리눅스 번호) 값으로. `sys_exit`는 `exit`(60)이 아니라 `exit_group`(231)을 쓴다 — 이 프로그램들은 전부 단일 스레드라 둘 다 결과는 같지만, 진짜 libc의 `exit()`가 내부적으로 `exit_group`을 쓰는 것과 같은 이유다. 클로버 목록에 `"rcx", "r11", "memory"`가 항상 들어간다 — `syscall` 명령 자체가 `rcx`(복귀 RIP)와 `r11`(복귀 RFLAGS)을 덮어쓰기 때문에 컴파일러에게 알려야 한다. 함수 이름·구조·출력 메시지·프로그램 이름은 전혀 안 바뀌었다 — 바뀐 건 시스템 콜을 요청하는 명령어와 번호뿐이다.

`tls.c`와 `user/syscall64.c`는 이제 진입 메커니즘이 완전히 같다(둘 다 `syscall` + 같은 번호). 원래 `syscall64.c`는 "새 경로가 int 0x80과 별개로 동작하는지" 비교하려고 만든 프로그램이었는데, `int 0x80`이 사라진 지금은 두 파일이 사실상 중복이다(메시지 접두어만 `tls:` vs `syscall64:`로 다름) — 지우지 않고 남겨뒀지만, 다음에 손댈 때 정리 대상이다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 43과 동일하게 `$` 프롬프트에서 멈춘다(`initramfs: 8 file(s) found`로 파일 수만 8→9, `IDT ready` 줄에서 `syscall=0x80(DPL=3)` 표기가 사라짐, 새 `syscall: SYSCALL/SYSRET MSR entry ready` 줄 추가). GUI(`make run`)에서 순서대로 입력하면:

```
$ hello
hello: Hello from hello!
process 1 exited: code=0
$ hello2
hello2: Hello from hello2!
process 1 exited: code=0
$ brk
brk: heap start = 0x0000000000301000
brk: heap end   = 0x0000000000303000
process 1 exited: code=0
$ mmap
mmap: addr = 0x00000000003FD000
mmap: mprotect rc = 0x0000000000000000
mmap: fstat rc = 0x0000000000000000
mmap: fstat st_mode = 0x00000000000021B6
process 1 exited: code=0
$ tls
tls: fs:0 = 0x1234567890ABCDEF
...
process 1 exited: code=0
$ syscall64
syscall64: fs:0 = 0x1234567890ABCDEF
...
process 1 exited: code=0
$ exit
shell: bye
process 0 exited: code=0
processes: init exited code=0
```

`init`/`hello`/`hello2`/`brk`/`mmap`/`tls`/`syscall64` 전부 `syscall` 명령 하나만으로 동작해야 한다 — `int 0x80`을 쓰는 코드는 이 저장소에 더 이상 없다. QEMU 모니터의 `sendkey`를 스크립트로 순서대로 넣어 확인했다(디스플레이 없이도 QMP `send-key`로 PS/2 이벤트가 정상 전달됨).

## 이전 단계(43) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/gdt.h` | 수정 | `gdt_enable_syscall(u64 handler)` 선언 추가 |
| `boot/gdt.c` | 수정 | `IA32_EFER`(SCE)/`IA32_STAR`/`IA32_LSTAR`/`IA32_FMASK` 설정하는 `gdt_enable_syscall` 구현 |
| `boot/syscall_entry.asm` | 신규 | `syscall_kernel_rsp`/`syscall_user_rsp_scratch` 전역, `syscall_entry` — 커널 스택 스위칭 + `interrupt_frame` 호환 프레임 구성 + `sti`/`syscall_dispatch` 호출/`cli` + `iretq` 복귀 |
| `boot/syscall.h` | 수정 | i386용/x86_64용으로 나뉘어 있던 enum 두 개를 진짜 x86_64 번호 하나(`SYS_*`, 19개)로 통합, `syscall_dispatch` 선언 하나만 남김, `syscall_kernel_rsp` extern |
| `boot/syscall.c` | 수정 | i386용 `syscall_dispatch`와 x86_64용 `syscall64_dispatch`를 `syscall_dispatch` 하나로 병합 — `exit`→`proc_thread_exit`/`exit_group`→`proc_exit`로 의미 분리, `clone`의 새 스택 인자를 `rbx`(i386)에서 `rdi`(x86_64 arg1)로, 죽은 코드였던 `SYS_SPAWN` 케이스 제거 |
| `boot/interrupts.c` | 수정 | IDT `0x80` 게이트 등록 제거, `interrupt_dispatch`의 vector `0x80` 분기(`sti`/`syscall_dispatch`/`cli`) 제거 |
| `boot/thread.c` | 수정 | `activate_thread`가 `gdt_set_kernel_stack`과 나란히 `syscall_kernel_rsp` 갱신 |
| `boot/kernel.c` | 수정 | `gdt_enable_syscall` 호출, `syscall_kernel_rsp` 초기화, readiness 로그 추가, `IDT ready` 로그에서 `syscall=0x80` 표기 제거 |
| `user/init.c` | 수정 | `int 0x80`→`syscall`, 번호를 i386 값에서 통합 `SYS_*` 값으로(`read`/`write`/`fork`/`wait4`/`execve`/`exit_group`) |
| `user/hello.c` | 수정 | `int 0x80`→`syscall`(`write`/`exit_group`) |
| `user/hello2.c` | 수정 | `int 0x80`→`syscall`(`write`/`exit_group`) |
| `user/brk.c` | 수정 | `int 0x80`→`syscall`(`write`/`exit_group`/`brk`) |
| `user/mmap.c` | 수정 | `int 0x80`→`syscall`(`write`/`exit_group`/`mmap`/`mprotect`/`fstat`), `r10`/`r8` 인자를 명시적 레지스터 변수로 전달 |
| `user/tls.c` | 수정 | `int 0x80`→`syscall`(`write`/`exit_group`/`arch_prctl`/`getpid`/`getuid`/`uname`) |
| `user/clone.asm` | 수정 | `clone_trampoline`의 `int 0x80`(번호 120/201)을 `syscall`(번호 56/60)로 |
| `user/syscall64.c` | 신규 | `tls.c`와 같은 검증 — 지금은 `tls.c`와 진입 메커니즘이 동일해져 중복이지만 유지 |
| `Makefile` | 수정 | `syscall_entry.asm` 빌드, `user/syscall64.c` 빌드/링크, initrd에 `syscall64` 포함 |
| `initrd/.gitignore` | 수정 | 빌드 산출물 `syscall64` 패턴 추가 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `45-argv-auxv`: 아직 musl 없이, 진짜 libc가 프로세스 시작 시 기대하는 초기 유저 스택(`argc`/`argv`/`envp`/`auxv`)을 갖춘다. `init`/`syscall64.c`를 확장해서 자기가 받은 `argc`/`argv[0]`을 그대로 출력하는 식으로 검증할 것.
- `46-musl-hello`: 45까지 갖춘 뒤 진짜 musl-gcc 정적 바이너리를 붙인다. `writev`/`ioctl`/`set_tid_address`/`munmap` 같이 지금은 없는 syscall들이 그때 strace로 확인되며 추가될 것이다.
