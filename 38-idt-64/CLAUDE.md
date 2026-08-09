# 38 — idt-64

**목표**: 64비트 IDT 재구성(16바이트 게이트 디스크립터) — 64비트 전환 2/3.

**37에서 이어짐**: 37은 `interrupts.asm`/`context_switch.asm`을 `BITS 64`로만 바꿔서 빌드가 되게 해뒀을 뿐, `kernel_main`이 `interrupts_init()`을 아예 호출하지 않아 IDT 자체가 죽어 있었다. 여기서 게이트 디스크립터를 16바이트로, 인터럽트 프레임을 실제 push 순서에 맞는 `u64` 필드로 다시 설계하고, `kernel_main`에서 IDT/타이머/키보드를 실제로 켠다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### 16바이트 IDT 게이트

32비트 게이트(8바이트: offset_low/selector/zero/type_attr/offset_high)로는 canonical 상위 주소(`0xFFFFFFFF80xxxxxx`)의 핸들러를 담을 수 없다. 64비트 게이트는 핸들러 주소를 세 조각(offset_low 16비트/offset_mid 16비트/offset_high 32비트)으로 나눠 담고, `ist`(Interrupt Stack Table index, 지금은 0 고정)와 8바이트 reserved가 추가된다.

```c
struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
} __attribute__((packed));
```

`idt_pointer.base`도 `u32`→`u64`로 바뀌면서 구조체 크기가 6→10바이트가 되고, `lidt`가 자동으로 64비트 IDTR 포맷으로 로드한다.

### 인터럽트 프레임 재설계 — 실제 push 순서에 맞추기

`interrupts.asm`의 `interrupt_common`은 이미 37에서 `push rax/rcx/rdx/rbx/rbp/rsi/rdi`(전부 8바이트, 7개, `esp` 없음)로 짜여 있었다. 문제는 `interrupts.h`의 `struct interrupt_frame`이 옛날 32비트 `pusha` 순서(edi,esi,ebp,**esp**,ebx,edx,ecx,eax — 8개)를 그대로 `u32`로 갖고 있었다는 것 — 필드 폭과 순서가 둘 다 어긋나 있어 `frame->vector`가 실제로는 `rdx`의 하위 4바이트를 읽는 식으로 완전히 엉뚱한 값이 나오는 상태였다. 이번에 asm이 실제로 쌓는 순서 그대로 struct를 다시 그렸다:

스택 최상단(rsp)부터: `rdi, rsi, rbp, rbx, rdx, rcx, rax`(수동 push 7개) → `vector, error_code`(ISR 스텁이 push) → `rip, cs, rflags, rsp, ss`(CPU가 iretq용으로 자동 push하는 5개).

```c
struct interrupt_frame {
    u64 rdi;  u64 rsi;  u64 rbp;  u64 rbx;
    u64 rdx;  u64 rcx;  u64 rax;
    u64 vector;  u64 error_code;
    u64 rip;  u64 cs;  u64 rflags;  u64 user_rsp;  u64 user_ss;
};
```

`mov rdi, rsp; call interrupt_dispatch`로 이 구조체 포인터를 그대로 넘기므로, 필드 순서가 스택 레이아웃과 1:1로 맞아야 한다.

### syscall.c 갱신 — fork_resume_t는 39로 유보

프레임 필드명이 `eax→rax`, `ebx→rbx` 등으로 바뀌면서 `syscall.c`의 모든 `frame->e**` 접근을 `frame->r**`로 갱신했다. `SYS_FORK`/`SYS_CLONE`이 프레임 값을 옮겨 담는 `fork_resume_t`(`process.h`)는 여전히 `u32` 8~9필드 그대로 두고, 대입부만 `(u32)frame->rdi`처럼 다운캐스트했다. `fork_resume_t`를 지금 `u64`로 넓혀도 `proc_init()`/`proc_spawn()`/`enter_user_mode_fork()`가 `kernel_main`에서 전혀 호출되지 않는 죽은 코드라 38의 동작에는 아무 영향이 없고, `process.c`/`paging.c`/`thread.c` 전체가 어차피 39~40에서 다시 짜여질 대상이라 지금 `fork_resume_t`만 먼저 넓히는 건 이 단계(IDT/인터럽트 프레임)의 범위를 벗어난 선반영이라고 판단했다 — `paging.c`가 지금도 옛 2-레벨 모델을 그대로 갖고 있는 것과 같은 성격으로, `process.c`/`syscall.c`를 실제로 포팅하는 40에서 `fork_resume_t`도 같이 넓힌다.

### console_printf `%l` 길이 지정자 추가

`rip`/`cr2`처럼 canonical 64비트 주소를 16진수로 찍을 방법이 없었다(`%X`는 `u32` 인자만 읽음). `%x`/`%X`/`%u` 앞에 `l`을 붙이면 `u64` 인자를 읽어 `console_write_unsigned64_padded`로 출력하도록 `console_vprintf`를 확장했다(`%016lX` 형태로 사용).

### 완료 기준에서 실제로 검증되는 것

`kernel_main`에서 `interrupts_init()` → `timer_init(100)` → `keyboard_init()` → `interrupts_enable()` 순으로 켠 뒤, `timer_sleep(200)`으로 하드웨어 IRQ0 경로(벡터 0x20, PIC EOI, `pic_send_eoi`)를 200ms 동안 반복 태운다 — 15-pit-timer/36에서 쓰던 것과 같은 "sleep 전후 tick 비교" 방식이다. 100Hz에서 delta=20이 나오면 IRQ 파이프라인 전체(PIC 리맵→16바이트 IDT→ISR 스텁→새 `interrupt_frame`→`handle_irq`→EOI)가 새 구조 그대로 정확히 살아있다는 뜻이다.

`timer_irq()`가 부르는 `scheduler_tick()`은 `threads_init()`이 아직 호출되지 않아 `scheduler_ready=0`으로 즉시 리턴한다 — 스레드/프로세스 인프라 없이도 안전하게 IRQ0을 켤 수 있는 이유다.

### 이번 단계에서 미룬 것 — `syscall`/`sysret` MSR 진입

상위 로드맵에 `syscall MSR 기반 시스템 콜 진입`이 38 설명에 같이 적혀 있었지만, 실제로 이 경로를 검증하려면 `sysretq`가 강제로 CPL3로 복귀하는데 아직 유저 접근 가능 페이지·GDT의 유저 세그먼트 순서(`SYSRET`는 `STAR[63:48]+8`=SS, `+16`=CS를 요구하므로 현재 GDT의 ucode64(0x18)/udata(0x20) 순서를 뒤집어야 함)·실제 ring 3 대상이 전혀 준비되어 있지 않다. `enter_user_mode`가 37부터 이미 `ret`뿐인 스텁인 것과 같은 이유로, 이 부분은 40에서 프로세스/유저모드가 실제로 살아날 때 GDT 유저 세그먼트 재배치와 함께 구현하는 편이 안전하다고 판단해 미뤘다. 지금 `int 0x80` 게이트(DPL=3, 16바이트)는 이미 정상 동작하므로 40에서 실제 유저 프로세스가 뜨면 이 경로부터 먼저 검증한다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 출력 확인 — 5초 후 종료 (정상)
make clean
```

## 완료 기준

`make run-nogui`:

```
Hello world -- long mode (64-bit)
GDT ready: null/kcode64/kdata/ucode64/udata/TSS(0x28)
paging: 4-level PAE active, 2MB huge pages, 1GB identity+kernel
phys mem: 32553 free pages (127MB usable)
IDT ready: 256 entries (16-byte gates) PIC=0x20/0x28 syscall=0x80(DPL=3)
timer: PIT 100Hz IRQ0 ready
keyboard ready: IRQ1 unmasked
sleep 200ms: ticks before=1
sleep done: ticks after=21 (delta=20)
long mode: IDT-64 ready (processes in 39)
```

(정확한 tick 수는 빌드마다 달라질 수 있다.)

## 이전 단계(37) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/interrupts.h` | 수정 | `struct interrupt_frame`을 실제 push 순서(rdi,rsi,rbp,rbx,rdx,rcx,rax,vector,error_code,rip,cs,rflags,user_rsp,user_ss) 그대로 `u64` 필드로 재설계 |
| `boot/interrupts.c` | 수정 | `idt_entry` 16바이트(ist/offset_mid/offset_high/reserved 추가); `idt_pointer.base` u64; `idt_set_entry(u8, u64, ...)`; `handle_exception`/`handle_irq`/`interrupt_dispatch`가 새 필드명 사용; rip/cr2를 `%016lX`로 출력 |
| `boot/console.c` | 수정 | `console_write_unsigned64_padded` 추가; `console_vprintf`에 `%l` 길이 지정자(u64 인자) 지원 |
| `boot/syscall.c` | 수정 | `frame->eax/ebx/ecx/edx/edi/esi/ebp/user_esp/eflags` → `frame->rax/rbx/rcx/rdx/rdi/rsi/rbp/user_rsp/rflags`; 정수 인자는 `(u32)` 다운캐스트, 포인터 인자는 64비트 그대로 캐스트; `SYS_FORK`/`SYS_CLONE`은 여전히 `u32` `fork_resume_t`에 `(u32)frame->r**`로 다운캐스트해서 채움(구조체 자체는 39까지 유보) |
| `boot/kernel.c` | 수정 | `interrupts_init()`/`timer_init(100)`/`keyboard_init()`/`interrupts_enable()` 호출 추가; 15-pit-timer/36과 같은 `timer_sleep(200)` sleep-전후-tick 비교로 IRQ0 데모; 준비 메시지를 "IDT-64 ready (processes in 39)"로 갱신 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `39-paging-thread-64`: `paging.c` 전체 재작성(4단계 PML4/PDPT/PD/PT), `kheap.h` 주소 재배치, `thread.h`/`thread.c` u64 전환 — 커널 쓰레드만으로 검증하고 유저모드는 아직 건드리지 않는다(64비트 전환 3/4). `process.c`/`elf.c`/`syscall.c`/`initrd.c`/`gdt.asm`은 39에서 그대로 둔다(여전히 32비트 타입, 여전히 죽은 코드).
- `40-usermode-64`: 39 위에서 ELF64, `process.c`/`syscall.c` 포팅, `gdt.asm`의 `enter_user_mode`/`enter_user_mode_fork` 실제 구현까지 마쳐 `proc_spawn("init")`이 실제로 셸을 띄운다(64비트 전환 4/4).
- **`process.h`의 `fork_resume_t`가 여전히 `u32` 8필드다.** `syscall.c`의 `SYS_FORK`/`SYS_CLONE`이 (38에서 새로 짠) `u64` `interrupt_frame`에서 값을 `(u32)`로 다운캐스트해서 채우는 중인데, 이건 지금은 죽은 코드(`proc_init`/`proc_spawn` 미호출)라 문제가 없지만 40에서 프로세스를 실제로 띄우는 순간 canonical 상위 주소(`rip`/`user_rsp`)가 32비트로 잘려나가는 진짜 버그가 된다. `process.c`/`syscall.c`를 포팅할 때 `fork_resume_t`도 `u64` 9필드로 같이 넓힐 것.
- 40에서 유저 모드가 실제로 살아나면, 이 단계에서 미뤄둔 `syscall`/`sysret` MSR 경로(GDT 유저 세그먼트 순서 재배치 + `STAR`/`LSTAR`/`SFMASK` 설정 + 엔트리 스텁)를 `int 0x80` 경로와 나란히 구현하는 것을 고려할 것.
