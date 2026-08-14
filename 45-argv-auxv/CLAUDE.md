# 45 — argv-auxv

**목표**: 아직 musl 없이, 진짜 libc가 프로세스 시작 시 기대하는 초기 유저 스택(`argc`/`argv`/`envp`/`auxv`)을 갖춘다. `_start`가 인자를 신경 쓰지 않던 지금까지의 커스텀 유저 프로그램과 달리, 실제 libc의 `_start`(musl 포함)는 최초 `%rsp`가 정확히 `argc`를 가리키고 있길 기대하고, TLS 초기화는 `auxv`의 `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`으로 자기 자신의 프로그램 헤더를 찾는다. 이 레이아웃을 커널이 실제로 정확하게 채워 넣는지, 아직 musl 없이 손수 만든 프로그램으로 먼저 검증한다.

**44에서 이어짐**: 44는 `syscall` 명령 + 진짜 x86_64 번호로 커널에 진입하는 배관을 놓고 `int 0x80` 경로를 완전히 없앴다 — `argv`/`auxv`는 손대지 않았고, `PROC_USTACK_TOP`도 여전히 43 이전 값(`0x400000`) 그대로였다. 이번 단계에서 그 스택 레이아웃을 채우다 보니, 지금까지 커스텀 테스트 프로그램들로는 한 번도 안 걸렸던 기존 버그 두 개가 함께 드러났다 — 진짜 ABI가 기대하는 것과 정말 맞는지는 실제로 채워봐야 안다는 뜻이다.

## 핵심 개념

### elf_setup_stack — argv/envp/auxv를 스택 맨 위 페이지 하나에 손으로 쓴다

Linux의 표준 초기 스택 레이아웃은 `argc, argv[0..], NULL, envp[0..], NULL, auxv[0].type, auxv[0].val, ..., AT_NULL, AT_NULL`이다. `elf_load_process`(`elf.c`)가 이제 `out_phdr`/`out_phnum`/`out_phentsize`를 추가로 돌려주고(첫 `PT_LOAD` 세그먼트가 `e_phoff`를 담고 있는 위치를 찾아 `AT_PHDR` 값을 역산), 새 `elf_setup_stack`이 스택 맨 위 페이지(`PROC_USTACK_TOP - 0x1000`) 하나에 `argc=1`, `argv[0]=(exec한 이름)`, 빈 `envp`, `AT_PHDR/AT_PHENT/AT_PHNUM/AT_PAGESZ/AT_ENTRY/AT_RANDOM/AT_SECURE/AT_NULL`을 순서대로 써넣고 그 시작 주소(=초기 `%rsp` 값)를 돌려준다. `proc_spawn`/`proc_exec`는 이제 `enter_user_mode`에 상수 `PROC_USTACK_TOP` 대신 이 계산된 주소를 넘긴다(`process_t.user_rsp` 필드로 들고 다닌다).

이번엔 요구되는 데이터가 argv/auxv뿐이라 스택 맨 위 페이지 딱 한 장이면 충분하지만, 유저 스택 전체는 1페이지(4KB)에서 8페이지(32KB)로 늘렸다 — 여러 페이지에 걸친 진짜 프로그램(46에서 올 musl 정적 바이너리)이 초기화 도중 스택을 더 쓸 걸 대비한 여유분이다.

### PROC_USTACK_TOP을 4MB에서 16MB로

`PROC_USTACK_TOP`(`0x00400000`)이 정적(비-PIE) ELF64 바이너리의 관행적 로드 주소(`0x400000`)와 정확히 같은 값이었다. 지금까지 커스텀 `user.ld`가 `0x00300000`(3MB)에 링크해서 안 걸렸을 뿐, 유저 스택 맨 위 페이지가 이미 그 자리를 차지하고 있었던 것이다. `0x01000000`(16MB)로 올렸다 — 46에서 붙일 musl 정적 바이너리의 기본 로드 주소(`0x400000`)와 겹치지 않도록 미리 옮겨둔 것이다. `PROC_MMAP_TOP`(mmap이 내려가는 시작점)은 이제 스택 전체(8페이지) 아래로 재계산된다 — `mmap`의 결과 주소가 44 때(`0x3FD000`)와 달라지는 건 이 재계산 때문이며 버그가 아니다.

### elf_load_process의 숨어있던 페이지 정렬 버그

`p_vaddr`가 페이지 경계에 딱 맞지 않는 세그먼트를 로드할 때, 필요한 페이지 수를 `p_memsz`만으로 계산해 마지막 페이지 하나를 통째로 안 매핑하고, 파일 내용도 "프레임 0번째 바이트 = `p_vaddr`의 내용"이라고 잘못 가정해서 엉뚱한 오프셋에 복사하고 있었다. 지금까지 커스텀 `user.ld`(전 섹션이 우연히 페이지 정렬)로는 전혀 안 드러났는데, `elf_setup_stack`을 만들며 phdr을 정확히 읽어야 할 필요가 생겨 코드를 다시 들여다보다 발견했다. "정렬된 시작 주소 + 그 안에서의 오프셋"을 기준으로 다시 계산하도록 고쳤다 — `page_off = p_vaddr - (p_vaddr & ~0xFFF)`를 구해서, 각 페이지의 각 바이트가 파일의 어느 오프셋에 대응하는지(`seg_off - page_off`)로 판단한다. 46에서 실제 musl 바이너리를 붙이면 이 경로가 바로 걸린다(GNU ld가 기본으로 만드는 `RELRO` 세그먼트는 `Align 1`이라 항상 페이지 경계에 안 맞는다).

### CR4.OSFXSR/OSXMMEXCPT — SSE를 켜지 않으면 진짜 컴파일된 코드가 못 돈다

이 커널 자신은 `-mno-sse`로 빌드되지만, 일반 `gcc`/`musl-gcc`로 컴파일한 코드는 기본으로 SSE2를 쓴다(`memcpy`/`memset`, 부동소수점 등). `entry.asm`이 PAE만 켜고 `OSFXSR`/`OSXMMEXCPT`(CR4 9,10비트)를 안 켜면 SSE 명령이 `#UD`(Invalid Opcode)로 죽는다. PAE 옆에 같이 켰다 — CR0도 `EM` 클리어/`MP` 설정으로 정리했다. 지금까지 유저 프로그램은 전부 `-mno-sse -mno-sse2 -mno-mmx`로 직접 빌드해왔기 때문에 이 문제가 한 번도 안 드러났다. (스레드 전환 시 XMM 레지스터를 저장·복원하지는 않는다 — SSE 쓰는 유저 프로세스가 하나만 동시에 돈다는 전제로 미뤄둔 부분이다.)

### proc_exec가 인자 문자열을 free 후에 읽던 버그

`exec(name)`의 `name`은 exec하는 프로세스 자신의 유저 메모리를 가리키는 포인터다. `proc_exec`가 `paging_free_user_pages`로 옛 주소공간을 통째로 반납한 *다음에* `elf_setup_stack`에 그 포인터를 그대로 넘겨 `argv[0]`을 채우려 했다 — 반납된 프레임은 곧바로 새 ELF 세그먼트에 재할당되어 버려서 `argv[0]`이 빈 문자열이 됐다. `name`을 free 전에 커널 스택의 지역 배열(`argv0[64]`)로 복사해두는 것으로 고쳤다. `proc_spawn`(커널이 직접 `"init"` 같은 리터럴로 호출)에는 이 문제가 없다 — 항상 커널 자신의 `.rodata`를 가리켜서 free 대상이 아니다.

### user/syscall64.c — 손으로 짠 `_start`로 진짜 초기 스택을 직접 읽는다

일반 `void _start(void) { ... }`는 컴파일러가 함수 프롤로그(`push rbp` 등)를 먼저 넣어서, 그 시점엔 커널이 넘겨준 원본 `%rsp` 값이 이미 로컬 변수 공간 계산에 밀려 정확히 어디였는지 알 수 없다. `_start`를 파일 스코프 `__asm__` 블록으로 손수 짜서 `%rsp`를 그대로 `%rdi`에 담아 `start_main(unsigned long *stack)`로 넘긴다 — 진짜 musl의 `crt_arch.h`가 하는 것과 같은 패턴이다. `start_main`은 `stack[0]`을 `argc`로, `stack+1`을 `argv`로 읽고, `argv+argc+1`부터 `envp`, 그 NULL 다음부터 `auxv`로 걸어가서 `AT_PAGESZ`(6) 값을 찾아 출력한다 — `elf_setup_stack`이 만든 레이아웃이 진짜로 맞는지 프로그램 스스로 확인하는 것이다. 이후는 44의 `arch_prctl`/`getpid`/`getuid`/`uname` 검증을 그대로 이어간다 — 같은 파일, 같은 명령(`syscall64`), 한 단계 더 검증하는 내용이 늘었을 뿐이다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 44와 동일하게 `$` 프롬프트에서 멈춘다. GUI(`make run`)에서 `syscall64`를 입력하면:

```
$ syscall64
syscall64: argc = 0x0000000000000001
syscall64: argv[0] = syscall64
syscall64: auxv AT_PAGESZ = 0x0000000000001000
syscall64: arch_prctl(SET_FS) rc = 0x0000000000000000
syscall64: fs:0 = 0x1234567890ABCDEF
syscall64: arch_prctl(GET_FS) rc = 0x0000000000000000
syscall64: fs_base readback = 0x00000000003006C0
syscall64: getpid = 0x0000000000000001
syscall64: getuid = 0x0000000000000000
syscall64: uname rc = 0x0000000000000000
syscall64: uname sysname = custom-os
syscall64: uname release = 0.43.0
process 1 exited: code=0
$
```

`argc=1`, `argv[0]=syscall64`, `AT_PAGESZ=0x1000`이 정확히 나와야 스택 레이아웃이 맞은 것이다. `hello`/`hello2`/`brk`/`mmap`/`tls`도 그대로 동작해야 한다(회귀 없음, 특히 `elf_load_process`의 페이지 정렬 수정이 기존 프로그램들의 로딩을 깨지 않았는지가 중요) — QEMU 모니터의 `sendkey`(QMP `send-key`)로 순서대로 확인했다.

## 이전 단계(44) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/entry.asm` | 수정 | CR4.OSFXSR/OSXMMEXCPT, CR0.EM 클리어/MP 설정 — SSE2 코드가 `#UD` 없이 실행되도록 |
| `boot/elf.h` | 수정 | `elf_load_process`에 `out_phdr`/`out_phnum`/`out_phentsize` 추가, `elf_setup_stack` 선언 |
| `boot/elf.c` | 수정 | 세그먼트 페이지 정렬 버그 수정(오프셋 있는 `p_vaddr` 처리), `AT_PHDR` 계산 추가, 새 `elf_setup_stack`(argv/envp/auxv 스택 구성) |
| `boot/process.h` | 수정 | `PROC_USTACK_PAGES`(8) 추가, `PROC_USTACK_TOP` 4MB→16MB, `process_t.user_rsp` 필드 추가 |
| `boot/process.c` | 수정 | `proc_spawn`/`proc_exec`가 `elf_setup_stack` 호출 후 계산된 RSP 사용, `proc_exec`는 `name` 문자열을 `paging_free_user_pages` 전에 지역 버퍼로 복사(use-after-free 수정) |
| `user/syscall64.c` | 수정 | `_start`를 파일 스코프 asm으로 다시 작성해 진짜 초기 스택(`argc`/`argv`/`auxv`)을 직접 읽어 출력, 이후 44의 검증 내용을 그대로 이어감 |
| `Makefile` | 수정 | `boot/elf.c`가 `boot/process.h`를 include하게 된 것을 의존성에 반영 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `46-musl-hello`: 45까지 갖춘 뒤 진짜 musl-gcc 정적 바이너리를 붙인다. `mmap`/`brk`는 이미 41/42에서 있지만 지금은 `syscall_dispatch`에 진짜 번호(`mmap=9`, `brk=12`)로 이미 연결돼 있다 — `munmap=11`, `writev`/`ioctl`/`set_tid_address`처럼 지금은 아예 없는 syscall이 strace로 확인되며 추가될 것이다.
- 여전히 `PT_TLS`가 있는 바이너리는 검증하지 않았다 — `elf_setup_stack`이 채우는 `AT_PHDR`/`AT_PHNUM`은 형식만 맞춰뒀을 뿐, 그걸 실제로 읽어 TLS 블록을 할당하는 바이너리가 와야 진짜 검증된다.
