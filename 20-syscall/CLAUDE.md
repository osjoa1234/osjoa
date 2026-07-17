# 20 — 시스템 콜 (int 0x80)

**목표**: `int 0x80` 인터럽트로 ring 3 → ring 0 진입 경로를 만들고, `SYS_WRITE`·`SYS_EXIT` 두 개의 syscall을 구현한다. user_task가 직접 VGA에 쓰는 대신 syscall을 통해 커널 서비스를 호출한다.

**19에서 이어짐**: 19에서 ring 3 진입까지 완성했다. user_task는 VGA 메모리에 직접 썼고 커널로 복귀하지 않았다. 20에서는 커널 서비스를 호출하는 경로(syscall gate)를 추가한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
20-syscall/
├── boot/
│   ├── console.c/h        # 19와 동일
│   ├── entry.asm          # 19와 동일
│   ├── interrupts.asm     # 19와 동일
│   ├── interrupts.c       # 수정: struct interrupt_frame 제거(헤더로 이동), syscall.h include,
│   │                      #        IDT 0x80 DPL=3(0xEE), interrupt_dispatch non-const, 0x80 분기
│   ├── interrupts.h       # 수정: struct interrupt_frame 선언 추가
│   ├── keyboard.c/h       # 19와 동일
│   ├── kheap.c/h          # 19와 동일
│   ├── monitor.c/h        # 19와 동일
│   ├── paging.c/h         # 19와 동일
│   ├── phys_mem.c/h       # 19와 동일
│   ├── timer.c/h          # 19와 동일
│   ├── context_switch.asm # 19와 동일
│   ├── gdt.h/c/asm        # 19와 동일
│   ├── thread.c/h         # 19와 동일
│   ├── syscall.h          # 신규: SYS_WRITE/SYS_EXIT enum, syscall_dispatch 선언
│   ├── syscall.c          # 신규: syscall_dispatch, sys_write, sys_exit 구현
│   └── kernel.c           # 수정: syscall.h include, user_task → int 0x80 사용
├── grub/grub.cfg
├── linker.ld
├── Makefile               # 수정: syscall.o 추가
└── build/
```

## 핵심 개념

### IDT DPL=3 게이트

`int n` 명령은 현재 CPL(ring 수준)이 IDT 엔트리의 DPL보다 높으면(수치상 크면) General Protection Fault를 일으킨다. ring 3에서 `int 0x80`을 호출하려면 IDT[0x80]의 DPL이 3이어야 한다.

| type_attr | 의미 |
|-----------|------|
| `0x8E` | 32비트 인터럽트 게이트, DPL=0 (ring 0에서만 호출 가능) |
| `0xEE` | 32비트 인터럽트 게이트, DPL=3 (ring 3에서도 호출 가능) |

`interrupts_init()`에서 256개를 모두 `0x8E`로 설정한 뒤, 0x80만 `0xEE`로 덮어쓴다.

### ring 3 → ring 0 전환 흐름

1. ring 3 코드가 `int $0x80` 실행
2. CPU가 IDT[0x80] DPL=3 확인 → GP fault 없이 통과
3. CPU가 TSS.esp0·TSS.ss0에서 커널 스택을 가져와 전환 (SS:ESP, EFLAGS, CS:EIP 저장)
4. `interrupt_stub_128` 실행: error_code=0, vector=0x80 push → `interrupt_common` 진입
5. `pushad`로 범용 레지스터 저장 → `interrupt_dispatch(&frame)` 호출
6. `frame->vector == 0x80` 분기 → `syscall_dispatch(frame)` 호출
7. 반환값을 `frame->eax`에 기록 → `popad`로 복원 → `iretd`로 ring 3 복귀

### interrupt_frame 공유

19까지는 `struct interrupt_frame`이 `interrupts.c` 안에만 정의되어 있었다. `syscall_dispatch`가 frame 포인터를 받으려면 헤더에서 공유해야 하므로 `interrupts.h`로 이동시킨다. `interrupts.c`에서 중복 정의를 제거하고 `#include "interrupts.h"`로 가져온다.

`interrupt_dispatch`의 파라미터도 `const struct interrupt_frame *` → `struct interrupt_frame *`으로 변경한다. syscall 핸들러가 `frame->eax`에 반환값을 기록해야 하기 때문이다. `popad`가 스택에서 eax를 복원하므로 frame을 수정하면 ring 3 코드가 eax로 반환값을 받는다.

### 호출 규약

Linux i386 syscall 규약을 그대로 따른다:

| 레지스터 | 역할 |
|----------|------|
| `eax` | syscall 번호 (입력) / 반환값 (출력) |
| `ebx` | 인자 1 |
| `ecx` | 인자 2 (미사용) |
| `edx` | 인자 3 (미사용) |

### syscall 목록

| 번호 | 이름 | 인자 | 반환 |
|------|------|------|------|
| 0 | `SYS_WRITE` | ebx=문자열 포인터(null-terminated) | 문자 수 |
| 1 | `SYS_EXIT` | ebx=종료 코드 | 복귀 안 함 |

`SYS_WRITE`는 커널이 포인터를 직접 `console_printf`로 출력한다. 실제 OS에서는 user-space 포인터 유효성 검사가 필요하지만, 이 단계에서는 생략한다.

`SYS_EXIT`는 메시지를 출력한 뒤 무한 `hlt` 루프로 진입한다.

### user_task 변경

19: VGA 메모리 직접 쓰기 (`volatile u16 *vga = 0xB8000`)  
20: `int $0x80`으로 `SYS_WRITE` 호출 → `SYS_EXIT` 호출

```c
__asm__ volatile (
    "int $0x80"
    : "=a" (ret)
    : "a" (SYS_WRITE), "b" (msg)
);
```

GCC inline asm의 output constraint `"=a"(ret)`으로 eax 반환값을 받는다.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 (3초 후 종료)
make clean
```

## 완료 기준

```
IDT ready: 256 entries PIC=0x20/0x28 syscall=0x80(DPL=3)
...
threads: all done -- entering user mode (ring 3), syscall via int 0x80
Hello from user mode -- int 0x80 syscall
user task exited: code=0
```

ring 3 코드가 커널 서비스를 `int 0x80`으로 호출하고, 커널이 처리한 뒤 ring 3로 복귀하는 전체 사이클이 동작한다.

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | syscall.o 추가 |
| `boot/interrupts.h` | 수정 | `struct interrupt_frame` 선언 추가 |
| `boot/interrupts.c` | 수정 | `struct interrupt_frame` 제거(헤더로 이동); `syscall.h` include; IDT 0x80을 DPL=3(0xEE)으로 설정; `interrupt_dispatch` non-const; 0x80 → `syscall_dispatch` 분기 추가 |
| `boot/syscall.h` | 신규 | `SYS_WRITE`·`SYS_EXIT` enum, `syscall_dispatch` 선언 |
| `boot/syscall.c` | 신규 | `syscall_dispatch`, `sys_write`, `sys_exit` 구현 |
| `boot/kernel.c` | 수정 | `syscall.h` include; `user_task` → VGA 직접 쓰기 대신 `int 0x80` 사용 |

## 다음 단계 힌트

- `21-initramfs`: GRUB 모듈로 initramfs 적재, 메모리 파일 접근.
