# 18 — 선점형 스케줄러

**목표**: 타이머 IRQ에서 직접 컨텍스트를 전환하는 선점형 라운드 로빈 스케줄러를 구현하고, `thread_sleep(ms)` / `wakeup`으로 슬립·웨이크업을 추가한다.

**17에서 이어짐**: 17에서 협력적 `thread_yield()` 기반 스케줄링이 동작했다. 18에서는 `THREAD_SLEEPING` 상태와 `wake_tick` 필드를 추가하고, 타이머 IRQ 핸들러에서 `scheduler_tick()`을 호출해 선점(preemption)을 구현한다. `kernel_main`이 `hlt` 루프로 대기하면서 타이머가 모든 스케줄링을 담당하도록 흐름을 변경한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
18-scheduler/
├── boot/
│   ├── console.c/h        # 17과 동일
│   ├── entry.asm          # 17과 동일
│   ├── interrupts.asm     # 17과 동일
│   ├── interrupts.c/h     # 수정: EOI를 핸들러 호출 전에 전송
│   ├── keyboard.c/h       # 17과 동일
│   ├── kheap.c/h          # 17과 동일
│   ├── monitor.c/h        # 수정: cmd_threads에 sleeping 상태 표시
│   ├── paging.c/h         # 17과 동일
│   ├── phys_mem.c/h       # 17과 동일
│   ├── timer.c/h          # 수정: timer_hz 추가, timer_irq에서 scheduler_tick 호출
│   ├── context_switch.asm # 17과 동일
│   ├── thread.c           # 수정: SLEEPING 상태, thread_sleep, scheduler_tick, scheduler_ready 플래그
│   ├── thread.h           # 수정: THREAD_SLEEPING, wake_tick, 새 API 선언
│   └── kernel.c           # 수정: thread_sleep 사용, hlt 대기 루프
├── grub/grub.cfg
├── linker.ld
├── Makefile               # 수정: timer.o와 thread.o 의존성 업데이트
└── build/
```

## 핵심 개념

### EOI-before-handler (`interrupts.c`)

17에서는 `handle_irq`가 핸들러를 호출한 뒤 EOI를 전송했다. 18에서는 **EOI를 먼저 전송**한다:

```c
pic_send_eoi((u8)irq);   // PIC 상태 해제 → 다음 타이머 IRQ 즉시 수신 가능
if (irq == 0U) timer_irq();
```

이 순서 변경이 선점형 스케줄링의 전제 조건이다. `timer_irq → scheduler_tick → switch_context`가 다른 스레드로 ESP를 전환하면, 해당 스레드는 타이머 ISR 체인 바깥에서 실행되기 시작한다. EOI가 이미 전송돼 있어야 다음 타이머 인터럽트가 정상적으로 발생한다.

EOI-after-handler 방식에서 EOI-before-handler로 바꾸면 인터럽트 처리 중 재진입(nested IRQ)이 가능해지지만, 100Hz 타이머와 짧은 핸들러에서는 실용적으로 문제가 없다.

### scheduler_tick (`thread.c`)

매 타이머 틱(10ms @ 100Hz)마다 호출된다:

```
1. wake_sleeping(): wake_tick에 도달한 SLEEPING 스레드를 RUNNING으로 전환
2. next_runnable(current)으로 다음 실행 후보를 탐색
3. 현재 스레드가 idle_task이면 → 즉시 스위치 (반응성 우선)
4. 일반 스레드면 → SCHED_QUANTUM(10 ticks = 100ms) 소진 후 스위치
```

`scheduler_ready` 플래그로 `threads_init()` 이전에 타이머 IRQ가 발생해도 안전하게 무시한다.

### ISR 내 switch_context 호출의 안전성

`scheduler_tick`은 타이머 ISR 안(IF=0)에서 `switch_context`를 호출한다.

- **현재 스레드 저장**: `pushfd`가 IF=0인 EFLAGS를 스택에 남긴다. 나중에 이 스레드로 돌아오면 `popfd`로 IF=0이 복원되고, ISR 체인(`timer_irq → handle_irq → interrupt_dispatch → ASM stub`)을 통해 정상적으로 `iret`이 실행돼 원래 EFLAGS(IF=1)를 복원한다.
- **새 스레드 시작(최초 실행)**: 초기 스택의 EFLAGS=0x202(IF=1)를 `popfd`로 복원 → `ret`으로 fn 진입. 인터럽트가 활성화된 상태에서 fn이 직접 실행된다.
- **thread_yield에서 재진입 방지**: `thread_yield`와 `thread_sleep`은 `interrupts_disable()`로 시작해 임계 구간(find-next → switch_context)을 보호하고, 스위치 후 복귀 시 `interrupts_enable()`로 마무리한다.

### thread_sleep / wakeup

```c
void thread_sleep(u32 ms) {
    interrupts_disable();
    current->state     = THREAD_SLEEPING;
    current->wake_tick = timer_ticks() + ms * timer_hz() / 1000U;
    // next_runnable → switch_context
    interrupts_enable();   // 복귀 후 실행
}
```

`wake_sleeping()`은 `scheduler_tick` 안에서 매 틱 호출돼, 만료된 슬립을 RUNNING으로 전환한다.

### idle_task hlt 루프

```c
while (thread_any_runnable()) {
    __asm__ volatile ("hlt");
}
```

- `thread_any_runnable()`은 비idle 스레드 중 DEAD가 아닌 것이 하나라도 있으면 true를 반환한다 (SLEEPING 포함 — 아직 끝나지 않은 스레드).
- `hlt`는 다음 인터럽트까지 CPU를 절전 상태로 둔다. 타이머 IRQ가 발생하면 `scheduler_tick`이 실행돼 자동으로 다음 스레드를 선택한다.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 후 3초 타임아웃 종료
make clean
```

## 완료 기준

```
threads: created A(id=1) B(id=2) -- preemptive scheduler
thread A: step 0
thread B: step 0
thread A: step 1
thread B: step 1
thread A: step 2
thread B: step 2
threads: all done -- entering monitor
```

A는 200ms, B는 300ms 간격으로 슬립하며 교대 실행된다. `thread_yield()` 없이 타이머 인터럽트만으로 스케줄링이 이뤄진다.

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | timer.o에 thread.h 의존성 추가, thread.o에 interrupts.h·timer.h 의존성 추가 |
| `boot/interrupts.c` | 수정 | handle_irq에서 EOI를 핸들러 호출 전에 전송 |
| `boot/timer.h` | 수정 | timer_hz() 선언 추가 |
| `boot/timer.c` | 수정 | timer_hz() 구현, timer_irq에서 scheduler_tick() 호출 |
| `boot/thread.h` | 수정 | THREAD_SLEEPING, wake_tick 필드, thread_sleep·scheduler_tick 선언 추가 |
| `boot/thread.c` | 수정 | scheduler_ready 플래그, wake_sleeping, next_runnable, thread_sleep, scheduler_tick, thread_exit 재구현; thread_yield에 interrupts_disable/enable 추가 |
| `boot/monitor.c` | 수정 | cmd_threads에서 THREAD_SLEEPING 상태 표시, sleep 명령을 thread_sleep으로 교체 |
| `boot/kernel.c` | 수정 | task_a/task_b가 thread_sleep 사용, 대기 루프를 hlt로 변경 |

## 다음 단계 힌트

- `19-user-mode`: GDT/TSS 정리, ring 3 진입, 첫 사용자 코드 실행.
