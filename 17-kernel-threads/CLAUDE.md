# 17 — 커널 스레드

**목표**: 커널 태스크 구조체와 협력적 컨텍스트 스위칭을 구현하고, 첫 커널 스레드 두 개를 실행한다.

**16에서 이어짐**: 16에서 키보드 링 버퍼와 커널 모니터가 준비됐다. 17에서는 `thread_t` 구조체와 원형 리스트, `switch_context` ASM 루틴, `thread_yield()`를 추가해 협력적 멀티태스킹을 구현한다. `kernel_main`이 두 스레드를 생성·실행한 뒤 모니터로 진입하도록 흐름을 변경한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
17-kernel-threads/
├── boot/
│   ├── console.c/h        # 16과 동일
│   ├── entry.asm          # 16과 동일
│   ├── interrupts.asm/c/h # 16과 동일
│   ├── keyboard.c/h       # 16과 동일
│   ├── kheap.c/h          # 16과 동일
│   ├── monitor.c/h        # 수정: threads 명령 추가
│   ├── paging.c/h         # 16과 동일
│   ├── phys_mem.c/h       # 16과 동일
│   ├── timer.c/h          # 16과 동일
│   ├── context_switch.asm # NEW: pushfd/pushad 기반 ESP 저장·복원
│   ├── thread.c           # NEW: thread_t, thread_create, thread_yield
│   ├── thread.h           # NEW
│   └── kernel.c           # 수정: threads_init, thread_create×2, while thread_any_runnable
├── grub/grub.cfg
├── linker.ld
├── Makefile               # context_switch.o, thread.o 추가
└── build/
```

## 핵심 개념

### 컨텍스트 스위칭 (`context_switch.asm`)

`switch_context(u32 *old_esp, u32 new_esp)` 함수가 두 스레드의 CPU 상태를 교환한다.

```
호출 직전 스택: [esp]   = ret addr
               [esp+4] = &old_esp
               [esp+8] = new_esp

pushfd  → esp -= 4  (EFLAGS 저장)
pushad  → esp -= 32 (EDI ESI EBP - EBX EDX ECX EAX, 8개 레지스터)
        총 36바이트 추가됨

[esp+40] = &old_esp 파라미터 위치
[esp+44] = new_esp  파라미터 위치
```

`mov [ecx], esp`로 현재 ESP를 `*old_esp`에 저장하고, `mov esp, edx`로 새 ESP를 적재한 뒤 `popad / popfd / ret`으로 복원한다.

### 신규 스레드 초기 스택 레이아웃 (`thread_create`)

`sp = stack_top`부터 11개 항목을 역순으로 푸시한다:

```
push thread_exit  ← fn이 return할 때 불릴 "복귀 주소"
push fn           ← ret로 뛰어들 진입점
push 0x202        ← EFLAGS (IF=1)
push 0 × 8        ← EAX ECX EDX EBX dummy EBP ESI EDI (popad 순서 역방향)

t->esp = sp (= stack_top - 44)
```

`switch_context`가 새 스레드의 `esp`를 적재하면:
- `popad` → 레지스터 복원
- `popfd` → EFLAGS = 0x202
- `ret` → EIP = fn 진입
- fn이 return 시 → `ret`이 `thread_exit` 호출

### 스택 크기와 kmalloc 제한

`THREAD_STACK_SIZE = 4096U`로 설정한다. kheap 블록 헤더(12 bytes) 오버헤드 때문에, 두 번째 `heap_grow` 후 합산 블록 크기는 `4056 + 4096 = 8152` bytes다. 8192를 요청하면 8192 > 8152라 kmalloc이 NULL을 반환해 스레드 생성이 실패한다. 4096은 이 크기 내에 안전하게 들어간다.

### 협력적 스케줄링

선점 없이 `thread_yield()`를 명시적으로 호출해야 교대한다. `idle_task(=main)`→`thread_a`→`thread_b`→`idle_task` 순의 원형 리스트를 순회한다. DEAD 상태 스레드는 건너뛴다. `thread_any_runnable()`이 false가 되면 모니터로 진입한다.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 후 3초 타임아웃 종료
make clean
```

## 완료 기준

```
threads: created A(id=1) B(id=2) -- cooperative round-robin
thread A: step 0
thread B: step 0
thread A: step 1
thread B: step 1
thread A: step 2
thread B: step 2
threads: all done -- entering monitor
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | context_switch.o, thread.o 빌드 대상 추가 |
| `boot/context_switch.asm` | 신규 | pushfd/pushad 기반 switch_context(old_esp, new_esp) |
| `boot/thread.h` | 신규 | thread_t 구조체, 상태 상수, 공개 API 선언 |
| `boot/thread.c` | 신규 | threads_init, thread_create, thread_yield, thread_exit, thread_any_runnable |
| `boot/monitor.c` | 수정 | thread.h include, cmd_threads(), threads 명령 dispatch·help 추가 |
| `boot/kernel.c` | 수정 | threads_init, task_a/task_b 정의, thread_create×2, while thread_any_runnable loop 추가 |

## 다음 단계 힌트

- `18-scheduler`: 선점형 타이머 인터럽트 기반 라운드 로빈 스케줄러와 sleep/wakeup.
