# 40 — usermode-64

**목표**: `gdt.asm`의 `iretq` 링3 진입, ELF64 로더, `process.c`/`syscall.c` 포팅, `kernel.c` 배선까지 마쳐 `proc_spawn("init")`이 실제로 유저 셸을 띄운다. 64비트 전환 4/4.

**39에서 이어짐**: 39는 `paging.c`(4단계)/`kheap.h`(재배치)/`thread.c`(u64)만 포팅하고 커널 쓰레드 두 개로 검증했다. `process.c`/`elf.c`/`syscall.c`/`initrd.c`/`gdt.asm`은 36 시절 32비트 코드 그대로 컴파일만 되는 죽은 코드로 남겨뒀다. 이번 단계에서 이 다섯 파일을 마저 포팅하고, `kernel_main`에 36(`linux-abi`, 32비트 마지막 완성형)과 같은 시퀀스(`initrd/vfs 마운트 → threads_init → proc_init → proc_spawn("init") → proc_wait`)를 이어붙여 64비트 전환을 완성한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### gdt.asm: enter_user_mode / enter_user_mode_fork — iretq로 실제 링3 진입

37부터 `ret` 스텁이었다. `iretq`는 스택에 `[rip][cs][rflags][rsp][ss]`(낮은 주소 → 높은 주소 순으로 push) 5워드가 쌓여 있어야 하고, `cs`/`ss`는 GDT의 `ucode64`(인덱스3, 셀렉터 `0x18`)/`udata`(인덱스4, `0x20`)에 RPL 3을 더한 `0x1B`/`0x23`이어야 한다(`kernel.c`가 부팅 시 찍는 "ucode64/udata" 메시지와 동일한 셀렉터).

```
push ss(0x23) → push rsp → push rflags(0x202) → push cs(0x1B) → push rip(entry) → iretq
```

`enter_user_mode_fork(const fork_resume_t *ctx)`는 `ctx`가 가리키는 9개의 `u64` 필드에서 rip/user_rsp/rflags를 먼저 스크래치 레지스터(r8/r9/r10)로 읽어 iretq 프레임부터 쌓은 뒤, `rdi`(ctx 포인터 자체)가 자유로워지는 시점에 나머지 6개 필드(rdi/rsi/rbp/rbx/rdx/rcx)를 채우고 `rax`는 0으로 고정한다(자식 프로세스에서 `fork()`가 0을 반환하는 것과 동일한 관례). `ctx` 포인터는 `r11`에 저장해 마지막까지 들고 있는다 — `r11`은 `fork_resume_t`가 복원하는 6개 레지스터에 포함되지 않으므로 안전하다.

### fork_resume_t: 8+1 필드 전부 u64로

38의 `syscall.c`는 `interrupt_frame`(64비트)에서 값을 읽어 `fork_resume_t`(32비트 8필드, `edi/esi/ebp/ebx/edx/ecx/eip/user_esp/eflags`)로 `(u32)` 다운캐스트해 채우고 있었다 — 이번에 프로세스가 실제로 뜨는 순간 이게 canonical 상위 주소(`rip`/`user_rsp`)를 잘라먹는 진짜 버그가 됐을 것이다. `fork_resume_t`를 `interrupt_frame`과 이름까지 맞춘 9개 `u64` 필드(`rdi,rsi,rbp,rbx,rdx,rcx,rip,user_rsp,rflags`)로 넓혀 다운캐스트를 전부 제거했다. `rax`가 빠진 이유는 여전히 동일하다 — 자식의 `rax`는 항상 0으로 하드코딩되므로 굳이 저장할 필요가 없다. `process_t.pd_phys`는 39가 `paging.c`/`paging.h` 내부에서만 `pml4_phys`로 개명한 것과 별개로 이름을 그대로 뒀다 — `paging_clone_dir()`이 반환하는 값을 프로세스 하나가 소유하는 주소공간 핸들로 다루는 이 필드는 "page directory"라는 옛 이름이어도 API 경계 밖 세부사항(4단계 PML4라는 사실)을 굳이 드러낼 필요가 없다.

### ELF64 — Phdr 필드 순서가 32비트와 다르다

`Elf32_Phdr`는 `p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align` 순이지만 `Elf64_Phdr`는 `p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align` — **`p_flags`가 두 번째 필드로 앞당겨진다.** 필드 이름만 보고 순서를 유추하면 잘못된 오프셋으로 읽는다. `Elf64_Ehdr`도 `e_entry/e_phoff/e_shoff`가 `u64`로 늘어나면서 헤더 전체 크기가 52→64바이트가 되어 `elf_load`/`elf_load_process`의 최소 크기 검사도 같이 바꿨다. `e_machine`은 `EM_386`(3) 대신 `EM_X86_64`(62)를 확인한다.

### initrd: mod_start + KERNEL_OFFSET이 u32에 안 들어간다

멀티부트1 모듈 주소(`mod_start`)는 32비트 물리주소지만, 여기에 `KERNEL_OFFSET`(`0xFFFFFFFF80000000`)을 더한 가상주소는 당연히 `u32`에 담기지 않는다. `initrd_init(u32,u32)`가 이 값을 그대로 받고 있던 게 39 전까지는 아무도 호출하지 않아 드러나지 않았을 뿐이다 — `u64`로 넓히고 내부 포인터 비교(`(u64)p + 110U <= end`)도 `u64`로 고쳤다.

### kernel_main 배선 — 36의 시퀀스를 64비트로 복원

36(`linux-abi`, 32비트 마지막 완성형)의 `kernel_main` 순서(`initrd/vfs 마운트 → threads_init → proc_init → proc_spawn("init") → proc_wait`)를 39가 이미 켜둔 `paging_init`/`kheap_init`/`threads_init` 기반 위에 이어붙였다. 39의 커널 쓰레드 데모(`thread_a`/`thread_b`, `timer_sleep(200)` 데모)는 여기서 걷어내고 `proc_spawn("init")`으로 교체됐다 — 커널 쓰레드로 스케줄러를 검증했으니, 이제 그 스케줄러 위에서 실제 유저 프로세스를 돌린다.

### 유저 빌드 64비트화

`user/*.c`의 인라인 `int $0x80` 어셈블리는 소스 변경이 필요 없었다 — GCC 레지스터 클래스 제약(`"a"`,`"b"`,`"c"`,`"d"`)이 `-m64`에서 자동으로 `rax`/`rbx`/`rcx`/`rdx`(포인터 인자처럼 8바이트 값이면) 또는 `eax`/`ebx`/`ecx`/`edx`(`unsigned int`처럼 4바이트 값이면)를 알아서 고른다. 바뀐 건 Makefile의 `-m32 -march=i386`→`-m64`(`-mno-red-zone`/`-mno-sse` 계열 추가, 인터럽트 핸들러 중 레드존을 밟거나 SSE 레지스터를 쓰면 커널이 위태로워지므로 커널 코드와 동일한 제약), `elf_i386`→`elf_x86_64` 뿐이다. `user/clone.asm`은 첫 인자를 스택(`[esp+4]`)이 아니라 SysV 관례대로 `rdi`로 받게 바꿨다(`SYS_CLONE`을 실제로 쓰는 유저 라이브러리는 아직 없어 링크만 통과하면 되는 상태).

### 알려진 한계 — 중첩 인터럽트 중 스케줄러 선점

`fork`+`exec`+`wait` 자체는 정상 동작한다(`hello`를 실행하면 "hello: Hello from hello!" → "process 1 exited: code=0"까지 정확히 나온다, QEMU monitor의 `sendkey`로 확인). 하지만 그 다음 `sys_read`가 `keyboard_getchar()`의 `hlt` 루프에서 실제 키 입력을 기다리는 동안 — 이 루프는 인터럽트가 켜진 채(`syscall_dispatch`가 `sti`로 감싸져 있음) 시스템 콜 핸들러 한복판에서 돈다 — PIT 타이머 IRQ가 중첩으로 들어와 `scheduler_tick()`이 **중첩된 인터럽트 프레임 한가운데에서** 전체 `switch_context()`를 실행할 수 있다. 실제로 셸을 띄워두고 `hello`를 실행한 뒤 다음 명령을 입력하면 셸의 유저 스택이 손상되어 페이지 폴트(`interrupt 0x0E`)로 죽는 것을 확인했다.

39에서 커널 쓰레드 두 개만으로 스케줄러를 검증했을 때는 이 문제가 전혀 나타나지 않았다(순수 커널-대-커널 전환은 안전) — 이 결함은 **유저 프로세스가 시스템 콜 핸들러 안에서 실제로 블로킹하는 상황**(`sys_read`가 키 입력을 기다림)에서만 나타난다. `keyboard_getchar`/`scheduler_tick`/`switch_context` 자체는 36 시절 32비트 코드와 동일한 설계이므로, 40에서 새로 생긴 버그가 아니라 **15~18단계부터 있던 선점 안전성 결함이 실제 경과시간을 들여 인터럽트 입력을 받은 것은 이번이 처음이라 드러난 것**이다. 지금까지 모든 단계의 `make run-nogui` 완료 기준은 "`$` 프롬프트에서 5초 타임아웃으로 멈춘다"였고, 이건 40에서도 동일하게 통과한다(키 입력이 전혀 없으면 nested 타이머 틱은 idle_task/셸 스레드끼리 반복 전환할 뿐이라 안전하다). 이 결함을 고치려면 "가장 바깥쪽 인터럽트 리턴에서만 선점" 같은 중첩 깊이 카운터를 스케줄러에 추가해야 하는데, 이는 64비트 포팅 범위를 넘는 스케줄러 자체의 재설계라 40에서는 손대지 않고 기록만 남긴다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI (키보드 입력 가능 — 단, 위 한계로 두 번째 명령부터는 죽을 수 있음)
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`:

```
Hello world -- long mode (64-bit), processes
GDT ready: null/kcode64/kdata/ucode64/udata/TSS(0x28)
paging: 4-level PAE active, 2MB huge pages, 1GB identity+kernel
phys mem: 32588 free pages (127MB usable)
IDT ready: 256 entries (16-byte gates) PIC=0x20/0x28 syscall=0x80(DPL=3)
heap: kernel dir adopted, window at 0xFFFFFFFFC0400000 (mapped=128MB)
initramfs: 5 file(s) found
vfs: initrd mounted at /
timer: PIT 100Hz IRQ0 ready
keyboard ready: IRQ1 unmasked
processes: init spawned pid=0
shell: linux-abi ready
$ (대기 — 정상, 5초 후 타임아웃 종료)
```

(free page 수는 빌드마다 달라질 수 있다.)

## 이전 단계(39) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/gdt.asm` | 수정 | `enter_user_mode`/`enter_user_mode_fork`를 `ret` 스텁에서 `iretq` 기반 실제 링3 진입으로 구현 |
| `boot/process.h` | 수정 | `fork_resume_t`를 `interrupt_frame`과 이름을 맞춘 9개 `u64` 필드로 확장; `process_t.entry`를 `u64`로 |
| `boot/process.c` | 수정 | `enter_user_mode` extern 시그니처 u64화; `entry` 지역변수 u64; `ctx->eip`→`ctx->rip` |
| `boot/syscall.c` | 수정 | `SYS_FORK`/`SYS_CLONE`이 `(u32)` 다운캐스트 없이 `frame`의 u64 필드를 그대로 `fork_resume_t`에 복사 |
| `boot/elf.h` | 수정 | `Elf32_Ehdr`/`Elf32_Phdr` → `Elf64_Ehdr`/`Elf64_Phdr`(필드 순서·폭 변경) |
| `boot/elf.c` | 수정 | ELF64 파싱, `EM_X86_64`(62) 검사, 최소 크기 52→64, 오프셋/크기 연산 u64 |
| `boot/initrd.h` | 수정 | `initrd_init`이 `u64 start, u64 end`를 받도록 |
| `boot/initrd.c` | 수정 | cpio 파싱 루프의 포인터 비교를 u64로 |
| `boot/kernel.c` | 수정 | 39의 커널 쓰레드 데모(`thread_a`/`thread_b`, sleep 데모)를 걷어내고 36의 `kernel_main` 시퀀스(initrd/vfs/proc 초기화 + `proc_spawn("init")` + `proc_wait`)를 64비트로 복원 |
| `Makefile` | 수정 | 유저 바이너리 빌드를 `-m32 -march=i386 elf_i386`→`-m64(+레드존/SSE 금지) elf_x86_64`로 |
| `user/clone.asm` | 수정 | `BITS 64`, 첫 인자를 스택 대신 `rdi`(SysV)로 받도록 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- **중첩 인터럽트 중 선점 문제**(위 "알려진 한계" 참고)를 스케줄러 쪽에서 정리하는 것을 고려할 것 — 예: `interrupt_dispatch`에 중첩 깊이 카운터를 두고 `scheduler_tick()`이 깊이 1(가장 바깥쪽 리턴 직전)에서만 실제로 스위치하게 하거나, `keyboard_getchar`가 raw `hlt` 대신 `thread_park`/`wait_queue`로 진짜 블로킹을 하도록 바꾸는 방법이 있다.
- `41-brk`: `process_t`에 `heap_end` 추가, `sys_brk(45)` 구현 — musl malloc 전제조건. `elf_load_process`가 스택 1페이지만 매핑하듯, 브레이크 힙도 유저 PML4의 0번 슬롯 안에서 `paging_map_user_page`를 재사용해 온디맨드로 확장하면 된다.
