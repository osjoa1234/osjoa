# 44 — syscall64

**목표**: `int 0x80` 옆에 x86_64의 진짜 syscall 진입 경로 — `syscall` 명령 + SYSCALL/SYSRET MSR — 을 추가한다. 지금까지 모든 syscall(`write`/`fork`/`brk`/`arch_prctl`...)은 이 커널이 정의한 i386풍 번호와 `rbx/rcx/rdx` 레지스터 관례로만 오갔다. 실제 x86_64 Linux ABI는 `int 0x80`이 아니라 `syscall` 명령을 쓰고, 번호도 인자 레지스터도 다르다. musl 같은 진짜 libc를 붙이려면(46) 이 경로가 먼저 있어야 한다.

**43에서 이어짐**: 43은 `tls.c`로 `arch_prctl`/`getpid`/`getuid`/`uname`을 `int 0x80` + i386 번호로 검증했다. 이번 단계는 그 43의 CLAUDE.md가 미뤄뒀던 질문 — "i386 관례 vs x86_64 관례, 어느 쪽에 맞출 것인가" — 에 대한 첫 답이다: **없애지 않고 나란히 추가한다.** 기존 `int 0x80` 경로(`hello`/`hello2`/`brk`/`mmap`/`tls`)는 그대로 두고, `syscall` 명령 + 진짜 x86_64 번호를 쓰는 두 번째 진입점을 만들었다. 아직 musl도, ELF의 argv/auxv 스택도 손대지 않는다 — 순수하게 "커널이 이 프로토콜을 말할 줄 아는지"만 확인한다.

## 핵심 개념

### SYSCALL/SYSRET MSR — 진입은 MSR로, 복귀는 기존 iretq로

x86_64 `syscall` 명령은 `int 0x80`과 완전히 별개의 하드웨어 경로다. `IA32_EFER`(`0xC0000080`)의 SCE 비트를 켜야 명령 자체가 동작하고, `IA32_LSTAR`(`0xC0000082`)에 진입 핸들러 주소를, `IA32_STAR`(`0xC0000081`)의 47:32비트에 커널 CS(`0x08`)를 넣는다 — SYSCALL 진입 시 CPU가 `SS = STAR[47:32]+8`을 자동으로 쓰는데, 이미 있는 커널 CS(`0x08`)/DS(`0x10`) 셀렉터와 정확히 맞아떨어져서 GDT를 새로 짤 필요가 없었다. `gdt.c`의 `gdt_enable_syscall`이 이 세 MSR을 쓴다 — `gdt_set_fs_base`와 같은 "GDT/세그먼트 관련 CPU 상태" 자리에 얹었다.

복귀는 `sysretq`를 쓰지 않는다. `sysretq`는 `STAR`의 63:48비트로 `SS=STAR[63:48]+8`, `CS=STAR[63:48]+16` 순서를 강제하는데, 지금 GDT 순서(코드 `0x18`, 데이터 `0x20`, TSS `0x28`)로는 이 공식을 만족하는 값이 없다(어떤 값을 넣어도 TSS 셀렉터와 겹치거나 존재하지 않는 디스크립터를 가리킨다). GDT를 재배열하는 대신, `syscall_entry.asm`은 진입 시 `rcx`(복귀 RIP)·`r11`(복귀 RFLAGS)·저장해둔 유저 RSP로 `interrupt_frame`과 완전히 같은 모양의 스택 프레임을 손으로 만들고 **`interrupt_common`과 동일한 GPR push/pop + `iretq`** 로 되돌아간다. 셀렉터는 기존 `enter_user_mode`가 쓰던 `0x1B`(유저 코드)/`0x23`(유저 데이터) 리터럴을 그대로 쓴다 — `sysretq`의 제약을 아예 우회하는 선택이다.

`syscall`은 스택을 자동으로 안 바꾼다(인터럽트와 달리 TSS.rsp0을 참조하지 않는다). 그래서 진입하자마자 `syscall_kernel_rsp`(전역 변수, `syscall_entry.asm`에 정의)로 커널 스택으로 직접 스위칭한다. 이 값은 `thread.c`의 `activate_thread`가 `gdt_set_kernel_stack`(TSS.rsp0)과 나란히 매 스레드 전환마다 갱신한다 — 두 값이 항상 같은 소스(`t->kstack_top`)를 따라가므로 "syscall 진입"과 "인터럽트 진입"이 항상 같은 커널 스택을 본다.

### syscall64_dispatch — 새 번호·레지스터, 같은 핸들러

새 진짜 x86_64 번호는 `syscall.h`에 `SYS64_*`로 따로 뒀다(`SYS_WRITE=4`인 기존 i386 열거형과 번호가 겹치면 안 되므로). 인자 레지스터도 다르다 — 기존은 `rbx/rcx/rdx`(i386 `int 0x80` 관례), 새 경로는 `rdi/rsi/rdx/r10/r8/r9`(진짜 x86_64 관례, 4번째 인자가 `rcx`가 아니라 `r10`인 이유는 `syscall` 명령 자체가 `rcx`를 복귀주소로 덮어써서 인자로 못 쓰기 때문). `syscall64_dispatch`(`syscall.c`)는 레지스터 추출 부분만 다르고, 실제 동작은 43이 이미 만든 `sys_write`/`sys_arch_prctl`/`sys_getpid`/`sys_getuid`/`sys_uname`/`proc_exit` static 함수를 그대로 재사용한다 — 이번 단계에서 새로 만든 핸들러는 하나도 없다, 진입·복귀 배관만 새로 놓았다.

지금 연결한 번호는 `write=1`, `arch_prctl=158`, `getpid=39`, `getuid=102`, `uname=63`, `exit=60`/`exit_group=231`뿐이다 — musl이 실제로 필요로 하는 `mmap`/`brk`/`writev`/`ioctl` 등은 아직 없다. 그건 45(argv/auxv 스택)와 46(musl-hello)에서 실제로 필요해지는 시점에 추가한다.

### user/syscall64.c — tls.c를 그대로, entry만 syscall로

43의 `tls.c`와 완전히 같은 검증(‌`arch_prctl(SET_FS)` 후 `%fs:0` 직접 읽기로 MSR이 실제로 걸렸는지 확인, `arch_prctl(GET_FS)`/`getpid`/`getuid`/`uname` 확인)을 하되, `sys_*` 래퍼 내부의 `__asm__("int $0x80")`를 `__asm__("syscall")`로, 레지스터를 `a/b/c/d`(rax/rbx/rcx/rdx)에서 `a/D/S/d`(rax/rdi/rsi/rdx)로, 번호를 i386 값에서 `SYS64_*` 값으로 바꿨을 뿐이다. 출력 메시지도 `tls:` → `syscall64:`로만 바뀌었다 — 같은 하드웨어 사실(FS.base MSR이 실제로 걸렸다)을 다른 진입 경로로 다시 확인한다는 걸 코드 구조로 보여준다. `init` 셸에서 `syscall64`라고 입력하면 실행된다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 43과 동일하게 `$` 프롬프트에서 멈춘다(`initramfs: 8 file(s) found`로 파일 수만 8→9, 새 `syscall: SYSCALL/SYSRET MSR entry ready` 줄 추가). GUI(`make run`)에서 `syscall64`를 입력하면:

```
$ syscall64
syscall64: arch_prctl(SET_FS) rc = 0x0000000000000000
syscall64: fs:0 = 0x1234567890ABCDEF
syscall64: arch_prctl(GET_FS) rc = 0x0000000000000000
syscall64: fs_base readback = 0x0000000000300520
syscall64: getpid = 0x0000000000000001
syscall64: getuid = 0x0000000000000000
syscall64: uname rc = 0x0000000000000000
syscall64: uname sysname = custom-os
syscall64: uname release = 0.43.0
process 1 exited: code=0
$
```

`fs:0`이 `tls_block`에 미리 써둔 값과 정확히 일치해야 새 경로에서도 MSR이 실제로 걸린 것이다. `hello`/`hello2`/`brk`/`mmap`/`tls`도 그대로 동작해야 한다(회귀 없음) — QEMU 모니터의 `sendkey`를 스크립트로 순서대로 넣어 확인했다.

## 이전 단계(43) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/gdt.h` | 수정 | `gdt_enable_syscall(u64 handler)` 선언 추가 |
| `boot/gdt.c` | 수정 | `IA32_EFER`(SCE)/`IA32_STAR`/`IA32_LSTAR`/`IA32_FMASK` 설정하는 `gdt_enable_syscall` 구현 |
| `boot/syscall_entry.asm` | 신규 | `syscall_kernel_rsp`/`syscall_user_rsp_scratch` 전역, `syscall_entry` — 커널 스택 스위칭 + `interrupt_frame` 호환 프레임 구성 + `syscall64_dispatch` 호출 + `iretq` 복귀 |
| `boot/syscall.h` | 수정 | `SYS64_*`(진짜 x86_64 번호: write/uname/exit/getpid/getuid/arch_prctl/exit_group) 열거형, `syscall64_dispatch`/`syscall_entry` 선언, `syscall_kernel_rsp` extern |
| `boot/syscall.c` | 수정 | `syscall64_dispatch` 추가 — 기존 `sys_write`/`sys_arch_prctl`/`sys_getpid`/`sys_getuid`/`sys_uname`/`proc_exit`을 새 레지스터 관례로 재호출할 뿐, 새 핸들러는 없음 |
| `boot/thread.c` | 수정 | `activate_thread`가 `gdt_set_kernel_stack`과 나란히 `syscall_kernel_rsp` 갱신 |
| `boot/kernel.c` | 수정 | `gdt_enable_syscall` 호출, `syscall_kernel_rsp` 초기화, readiness 로그 추가 |
| `user/syscall64.c` | 신규 | `tls.c`와 같은 검증을 `syscall` 명령 + 진짜 번호로 재현 — 검증용 유저 프로그램 |
| `Makefile` | 수정 | `syscall_entry.asm` 빌드, `user/syscall64.c` 빌드/링크, initrd에 `syscall64` 포함 |
| `initrd/.gitignore` | 수정 | 빌드 산출물 `syscall64` 패턴 추가 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `45-argv-auxv`: 아직 musl 없이, 진짜 libc가 프로세스 시작 시 기대하는 초기 유저 스택(`argc`/`argv`/`envp`/`auxv`)을 갖춘다. `syscall64.c`를 확장해서 자기가 받은 `argc`/`argv[0]`을 그대로 출력하는 식으로 검증할 것.
- `46-musl-hello`: 45까지 갖춘 뒤 진짜 musl-gcc 정적 바이너리를 붙인다. `mmap`/`brk`/`writev`/`ioctl`/`set_tid_address`/`munmap` 같이 지금은 없는 syscall들이 그때 strace로 확인되며 추가될 것이다.
