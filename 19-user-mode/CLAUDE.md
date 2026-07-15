# 19 — 사용자 모드 (ring 3)

**목표**: GDT에 user code/data 세그먼트와 TSS를 추가하고, `iret`으로 ring 3에 진입한다. 첫 사용자 코드가 VGA 메모리에 직접 문자를 써서 ring 3 실행을 확인한다.

**18에서 이어짐**: 18에서 선점형 스케줄러와 `thread_sleep`이 동작했다. 19에서는 커널 스레드들이 모두 종료된 뒤 `enter_user_mode`로 ring 3에 진입한다. syscall은 20장에서 구현한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
19-user-mode/
├── boot/
│   ├── console.c/h        # 18과 동일
│   ├── entry.asm          # 수정: stack_top을 global로 export
│   ├── interrupts.asm     # 18과 동일
│   ├── interrupts.c/h     # 18과 동일
│   ├── keyboard.c/h       # 18과 동일
│   ├── kheap.c/h          # 18과 동일
│   ├── monitor.c/h        # 18과 동일
│   ├── paging.c/h         # 수정: page_table_0과 page_directory에 user bit 추가
│   ├── phys_mem.c/h       # 18과 동일
│   ├── timer.c/h          # 18과 동일
│   ├── context_switch.asm # 18과 동일
│   ├── thread.c/h         # 18과 동일
│   ├── gdt.h              # 신규: gdt_init 선언
│   ├── gdt.c              # 신규: GDT 6 엔트리 + TSS 초기화
│   ├── gdt.asm            # 신규: gdt_flush, tss_flush, enter_user_mode
│   └── kernel.c           # 수정: gdt_init을 interrupts_init 이전에 호출, user_task, enter_user_mode 호출
├── grub/grub.cfg
├── linker.ld
├── Makefile               # 수정: gdt_asm.o, gdt.o 추가
└── build/
```

## 핵심 개념

### GDT 구조 (`gdt.c`)

GRUB이 설정한 GDT를 그대로 쓰던 것을 우리 GDT로 교체한다. 6개 엔트리:

| 인덱스 | Selector | 설명 | Access |
|--------|----------|------|--------|
| 0 | 0x00 | null | 0x00 |
| 1 | 0x08 | kernel code (DPL=0) | 0x9A |
| 2 | 0x10 | kernel data (DPL=0) | 0x92 |
| 3 | 0x1B | user code (DPL=3)   | 0xFA |
| 4 | 0x23 | user data (DPL=3)   | 0xF2 |
| 5 | 0x28 | TSS                 | 0x89 |

ring 3 selector는 `index << 3 | 3` 형태다 (0x18|3=0x1B, 0x20|3=0x23).

### gdt_init 순서 (`kernel.c`)

`gdt_init()`을 반드시 `interrupts_init()` **이전**에 호출해야 한다.

`interrupts_init()`은 `read_cs()`로 현재 CS를 읽어 IDT 모든 엔트리의 selector로 저장한다. GRUB이 설정한 CS는 GRUB의 GDT 기준의 selector다. 나중에 `gdt_init()`으로 GDT를 교체하면, 동일한 selector 번호가 새 GDT에서 다른 세그먼트(예: user code, DPL=3)를 가리킬 수 있다. 이 상태에서 타이머 인터럽트가 오면 CPU가 IDT entry의 selector를 새 GDT로 해석하고 GP fault가 발생한다.

`gdt_init()`을 먼저 호출하면 `read_cs()`가 새 GDT의 CS=0x08을 반환하므로 IDT가 올바른 kernel code segment를 참조한다.

### gdt_flush (`gdt.asm`)

`lgdt`으로 새 GDT를 로드한 직후 CS를 갱신해야 한다. CS는 `mov` 명령으로 직접 변경할 수 없으므로 far return을 사용한다:

```asm
push dword 0x08       ; CS
push dword .reload_cs ; EIP
retf                  ; CS:EIP로 far return
```

DS, ES, FS, GS, SS는 `mov ax, 0x10; mov ds, ax` 형태로 직접 설정한다.

### TSS (`gdt.c`)

ring 3 → ring 0 전환 시 CPU가 TSS에서 커널 스택(esp0, ss0)을 가져온다. 19장에서는 syscall이 없으므로 실제로 TSS를 사용하지 않지만, `ltr` 명령으로 TSS를 로드해두어야 올바른 CPU 상태를 유지한다.

```c
tss.ss0  = 0x10;         // kernel data segment
tss.esp0 = stack_top;    // entry.asm의 커널 스택 꼭대기
```

`ltr 0x28`로 TR 레지스터에 TSS를 로드한다.

### enter_user_mode (`gdt.asm`)

`iret`으로 ring 3에 진입한다. 스택에 아래 순서로 push한 뒤 `iret`:

```
SS     = 0x23  (user data | 3)
ESP    = user stack pointer
EFLAGS = 0x202 (IF=1, reserved bit)
CS     = 0x1B  (user code | 3)
EIP    = user function address
```

`iret` 전에 DS, ES, FS, GS도 0x23으로 설정해야 한다. 설정하지 않으면 ring 3 코드가 커널 데이터 세그먼트(DPL=0)로 데이터 세그먼트를 사용하게 되어 메모리 접근 시 GP fault가 발생한다.

### 페이지 user bit (`paging.c`)

ring 3 코드가 메모리에 접근하려면 페이지 테이블 엔트리에 user bit(bit 2)가 켜져 있어야 한다. 기존 `0x03`(P+RW, supervisor only)에서 `0x07`(P+RW+User)로 변경한다.

이 변경으로 하위 4MB 전체가 ring 3에서 접근 가능해진다. VGA 메모리(0xB8000)와 user_stack(커널 .bss에 정의)이 모두 이 범위 안에 있다.

### user_task

ring 3에서 VGA 텍스트 메모리(0xB8000)에 직접 쓴다. VGA 메모리는 메모리 맵드 I/O이므로 I/O 포트 없이 일반 메모리 쓰기로 접근할 수 있다. 20장(syscall)이 없으므로 커널로 복귀하지 않고 무한 루프로 대기한다.

```c
volatile u16 *vga = (volatile u16 *)0x000B8000U;
vga[80 + i] = 0x1F00 | ch;  // row 1, white on blue
```

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI: VGA 2번째 행에 "Hello from user mode" 확인
make run-nogui  # debugcon 로그로 "entering user mode (ring 3)"까지 확인
make clean
```

## 완료 기준

```
GDT ready: 6 entries kernel(0x08/0x10) user(0x1B/0x23) TSS(0x28)
IDT ready: 256 entries PIC=0x20/0x28
...
threads: all done -- entering user mode (ring 3)
```

`make run` (GUI)에서 VGA 화면 2번째 행에 흰색/파란 배경으로 "Hello from user mode -- ring 3 direct VGA write"가 표시된다.

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | gdt_asm.o, gdt.o 추가 |
| `boot/entry.asm` | 수정 | `global stack_top` 추가 (TSS esp0에서 사용) |
| `boot/paging.c` | 수정 | page_table_0, page_directory flags 0x03→0x07 (user bit 추가) |
| `boot/gdt.h` | 신규 | `gdt_init(u32 kernel_stack)` 선언 |
| `boot/gdt.c` | 신규 | GDT 6 엔트리 설정, TSS 초기화 |
| `boot/gdt.asm` | 신규 | `gdt_flush`, `tss_flush`, `enter_user_mode` 구현 |
| `boot/kernel.c` | 수정 | gdt_init을 interrupts_init 이전으로 이동; user_task, user_stack, enter_user_mode 호출 추가 |

## 다음 단계 힌트

- `20-syscall`: `int 0x80` 기반 시스템 콜 진입, user_task에서 커널 서비스 호출.
