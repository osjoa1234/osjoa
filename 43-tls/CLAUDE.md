# 43 — tls

**목표**: `arch_prctl(172)`의 `ARCH_SET_FS`/`ARCH_GET_FS`로 FS.base MSR을 실제로 설정하고, `getpid(20)`/`getuid(24)`/`uname(122)` stub을 추가한다. musl은 프로그램 시작 시 `__init_tp`에서 `arch_prctl(ARCH_SET_FS, tls_block)`을 호출해 스레드 포인터를 세우므로, 이 호출이 크래시 없이 성공하고 `%fs` 상대 주소가 실제로 그 블록을 가리켜야 musl 초기화가 이어질 수 있다.

**42에서 이어짐**: 42는 `brk`/`mmap`으로 musl mallocng가 메모리를 얻어오는 두 경로를 갖췄다. musl은 메모리를 얻기 전에 먼저 TLS(스레드 로컬 스토리지) 포인터를 세우는데, x86_64에서 이 포인터는 세그먼트 레지스터의 값이 아니라 `FS.base` MSR(`0xC0000100`)이 담고 있는 주소다. 이번 단계는 그 MSR을 유저 프로그램이 요청한 주소로 실제로 바꿔주는 `arch_prctl`을 구현한다. `getpid`/`getuid`/`uname`은 TLS 자체와는 무관하지만 musl 초기화 경로와 흔한 libc 호출이 곧바로 뒤따르므로 "크래시 없이 그럴듯한 값"을 돌려주는 수준으로 같이 채운다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### FS.base는 세그먼트 셀렉터가 아니라 MSR

보호모드/32비트에서는 세그먼트 디스크립터의 base 필드가 `%fs`가 가리키는 주소였지만, long mode에서는 코드/데이터 세그먼트 디스크립터의 base가 무시되고 대신 `IA32_FS_BASE`(`0xC0000100`)·`IA32_GS_BASE`(`0xC0000101`) MSR이 `%fs`/`%gs` 상대 주소의 기준을 정한다. `entry.asm`이 이미 `EFER.LME`를 켜는 자리에서 `wrmsr`를 한 번 썼던 것과 같은 명령을 재사용해, `gdt.c`에 `gdt_set_fs_base(u64 base)`를 추가했다 — `ecx`에 MSR 번호, `edx:eax`에 상위/하위 32비트를 실어 `wrmsr` 한 번으로 끝난다. GDT/TSS를 다루는 파일에 얹은 이유는 "세그먼트 관련 CPU 상태"라는 성격이 `gdt_set_kernel_stack`(TSS.rsp0)과 같기 때문이다.

### fs_base는 프로세스가 아니라 스레드에 귀속

`heap_end`/`mmap_next`는 주소공간을 공유하는 프로세스 전체가 공유해야 하므로 `process_t`에 있지만, TLS 포인터는 스레드마다 달라야 하는 값이라 `thread_t`에 `fs_base`(u64) 필드로 추가했다. 스케줄러가 스레드를 깨워 넣는 지점인 `activate_thread`(`thread.c`)에서 `paging_set_dir`(CR3 전환)와 나란히 `gdt_set_fs_base(t->fs_base)`를 호출한다 — CR3를 스레드가 아니라 프로세스(`t->pd`) 단위로 두면서도 매 스위치마다 다시 쓰는 것과 같은 이유로, FS.base도 "현재 스레드가 무엇이든 컨텍스트 스위치 후에는 그 값이어야" 하므로 매번 다시 쓴다. 새로 만들어지는 스레드는 `fs_base = 0`으로 시작한다(아직 아무도 TLS를 세팅하지 않은 상태를 표현).

`proc_fork`는 자식 스레드를 새로 만들면서 `t->fs_base = thread_current()->fs_base`로 부모의 값을 그대로 물려준다 — `fork()`는 주소공간을 통째로 복제하므로 TLS 블록도 같은 가상주소에 그대로 살아있고, 부모가 이미 `arch_prctl`을 불렀다면 자식도 같은 FS.base를 가져야 맞다. `heap_end`/`mmap_next`를 복사하던 것과 같은 자리에 한 줄을 더한 것뿐이다. `proc_clone`(같은 프로세스 안의 추가 스레드)은 이 단계에서 손대지 않았다 — 새 스레드가 스스로 다른 TLS 블록으로 `arch_prctl`을 부르는 것이 정상 동작이고, `SYS_CLONE`이 아직 `CLONE_SETTLS` 같은 인자를 받지 않으므로 지금 건드리면 검증되지 않은 채로 남는다.

### sys_arch_prctl — SET_FS는 즉시 반영, GET_FS는 Linux처럼 포인터에 씀

```
ARCH_SET_GS = 0x1001   (미지원 — default case로 떨어져 -1 반환)
ARCH_SET_FS = 0x1002   (thread_t.fs_base 갱신 + wrmsr로 즉시 반영)
ARCH_GET_FS = 0x1003   (*addr = thread_t.fs_base, 반환값 자체는 0)
ARCH_GET_GS = 0x1004   (미지원)
```

레지스터 관례는 기존 syscall들과 동일하게 `rbx`=arg1(code), `rcx`=arg2(addr)다(i386 `int 0x80` 순서를 그대로 씀 — `mmap2`/`mprotect`와 같은 관례). `ARCH_GET_FS`는 반환값에 주소를 직접 담지 않고 Linux 실제 동작처럼 `addr` 포인터가 가리키는 유저 메모리에 `u64`로 써준다 — 반환값은 성공/실패(`0`/`-1`)만 나타낸다. 커널이 유저 포인터에 바로 쓰는 것은 `sys_fstat`이 `stat_t *buf`에 쓰던 것과 같은 패턴이다(현재 페이지 테이블이 호출한 프로세스의 것이므로 그냥 역참조하면 된다).

### getpid/getuid/uname — musl 초기화가 곧바로 건드리는 세 가지

- `getpid`(20): `process_t.pid`를 그대로 반환. 이미 있는 정보라 진짜 값을 준다.
- `getuid`(24): 사용자 개념이 없으므로 항상 `0`(root)을 반환하는 stub.
- `uname`(122): `struct utsname`(`sysname/nodename/release/version/machine/domainname` 각 65바이트)을 채워 반환 — `sysname="custom-os"`, `release="0.43.0"`, `version="#43"`, `machine="x86_64"`, `domainname`은 빈 문자열로 둔다. 이 구조체도 42의 `stat_t`처럼 이 커널 자체 정의이고 실제 musl이 필드 크기/순서를 어떻게 기대하는지는 44에서 다시 맞출 수 있다.

### user/tls.c — FS.base가 실제로 걸렸는지 %fs 상대 읽기로 검증

`user/mmap.c`와 같은 구조. 전역 배열 `tls_block[8]`에 알아볼 수 있는 값(`0x1234567890ABCDEF`)을 미리 써두고, `arch_prctl(ARCH_SET_FS, &tls_block)`을 호출한 다음 `mov %fs:0, reg`로 직접 읽어 같은 값이 나오는지 확인한다 — 이 두 값이 일치해야 `wrmsr`가 하드웨어 레벨에서 실제로 먹혔다는 뜻이다(단순히 반환값만 보면 커널이 필드만 갱신하고 MSR을 안 써도 성공처럼 보일 수 있다). 이어서 `ARCH_GET_FS`로 되읽은 주소가 `tls_block`의 주소와 같은지, `getpid`/`getuid`/`uname`이 그럴듯한 값을 주는지 출력한다. `init` 셸에서 `tls`라고 입력하면 실행된다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 42와 동일하게 `$` 프롬프트에서 멈춘다(`initramfs: 8 file(s) found`로 파일 수만 7→8). GUI(`make run`)에서 `tls`를 입력하면:

```
$ tls
tls: arch_prctl(SET_FS) rc = 0x0000000000000000
tls: fs:0 = 0x1234567890ABCDEF
tls: arch_prctl(GET_FS) rc = 0x0000000000000000
tls: fs_base readback = 0x00000000003004C0
tls: getpid = 0x0000000000000001
tls: getuid = 0x0000000000000000
tls: uname rc = 0x0000000000000000
tls: uname sysname = custom-os
tls: uname release = 0.43.0
process 1 exited: code=0
$
```

`fs:0`이 `tls_block`에 미리 써둔 값과 정확히 일치해야 FS.base가 실제로 걸린 것이다. `brk`/`mmap`/`hello`/`hello2`도 그대로 동작해야 한다(회귀 없음).

## 이전 단계(42) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/gdt.h` | 수정 | `gdt_set_fs_base(u64 base)` 선언 추가 |
| `boot/gdt.c` | 수정 | `IA32_FS_BASE`(0xC0000100) `wrmsr`로 FS.base를 설정하는 `gdt_set_fs_base` 구현 |
| `boot/thread.h` | 수정 | `thread_t`에 `fs_base` 필드 추가 |
| `boot/thread.c` | 수정 | `threads_init`/`thread_create_with_data`가 `fs_base`를 0으로 초기화, `activate_thread`가 스위치마다 `gdt_set_fs_base` 호출 |
| `boot/process.c` | 수정 | `proc_fork`가 자식 스레드의 `fs_base`를 부모 것으로 복사 |
| `boot/syscall.h` | 수정 | `SYS_GETPID=20`, `SYS_GETUID=24`, `SYS_UNAME=122`, `SYS_ARCH_PRCTL=172` 추가 |
| `boot/syscall.c` | 수정 | `sys_arch_prctl`(SET_FS/GET_FS), `sys_getpid`, `sys_getuid`, `utsname_t`/`sys_uname` 추가, 해당 syscall 디스패치 케이스 추가 |
| `user/tls.c` | 신규 | `arch_prctl`로 FS.base를 세팅하고 `%fs:0` 직접 읽기로 검증, `getpid`/`getuid`/`uname` 결과 출력 — 검증용 유저 프로그램 |
| `Makefile` | 수정 | `user/tls.c` 빌드/링크, initrd에 `tls` 포함 |
| `initrd/.gitignore` | 수정 | 빌드 산출물 `tls` 패턴 추가 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `44-musl-hello`: musl-static으로 빌드한 첫 외부 바이너리를 initrd에 넣어 실행 검증. 지금까지 41~43에서 만든 `brk`/`mmap`/`arch_prctl`/`getpid`/`getuid`/`uname`이 실제 musl 초기화 경로(`__init_tp`, `__libc_start_main` 등)가 기대하는 순서·인자와 맞는지 처음으로 검증된다. `stat_t`/`utsname_t` 레이아웃이 실제로 문제가 되면 그때 musl이 기대하는 필드 순서에 맞춰 다시 정렬한다 — 지금까지는 이 커널 자체 정의로 미뤄뒀다.
- 현재 syscall 번호·레지스터 관례(`rbx/rcx/rdx/rsi/rdi/rbp`, `int 0x80`)는 i386 Linux ABI를 본뜬 것이고, 유저 바이너리는 실제로는 ELF64(`-m64`)로 빌드된다 — musl을 어떤 타깃(i386 vs x86_64 syscall 관례)으로 맞출지는 44에서 실제 바이너리를 붙이며 결정될 문제로 미뤄둔다.
