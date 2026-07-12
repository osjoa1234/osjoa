# 15 — PIT 타이머

**목표**: PIT(Programmable Interval Timer) IRQ0으로 tick 카운터를 구현하고, `timer_sleep(ms)`로 밀리초 단위 대기를 제공한다.

**14에서 이어짐**: 14에서 kmalloc/kfree가 준비됐다. 15에서는 `timer.c`를 추가하고 interrupts.c의 IRQ0 핸들러에 연결한다. kernel.c에서 `timer_sleep(500)`으로 500ms 대기 후 tick 변화를 출력해 동작을 확인한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
15-pit-timer/
├── boot/
│   ├── console.c/h        # 14와 동일
│   ├── entry.asm          # 14와 동일
│   ├── interrupts.asm/c/h # IRQ0 핸들러에서 timer_irq() 호출 추가
│   ├── keyboard.c/h       # 14와 동일
│   ├── kheap.c/h          # 14와 동일
│   ├── paging.c/h         # 14와 동일
│   ├── phys_mem.c/h       # 14와 동일
│   ├── timer.c            # NEW: PIT 설정, tick 카운터, timer_sleep
│   ├── timer.h            # NEW
│   └── kernel.c           # timer_init() 호출, sleep 500ms 시연
├── grub/grub.cfg
├── linker.ld
├── Makefile               # timer.o 추가
└── build/
```

## 핵심 개념

- **PIT 채널 0**: I/O 포트 0x40(데이터), 0x43(커맨드). 커맨드 0x36 = 채널0, lobyte/hibyte, mode 3(square wave).

- **주파수 설정**: PIT 기본 클럭 1,193,182 Hz. `divisor = 1193182 / hz`를 채널0에 low byte → high byte 순서로 쓴다. 100Hz → divisor ≈ 11931 → 실제 주기 10ms.

- **tick 카운터**: `static volatile u32 tick_count` — volatile이 필수. 컴파일러가 ISR 바깥에서의 읽기를 최적화로 제거하지 못하게 막는다.

- **`timer_sleep(ms)`**: `target = tick_count + (ms * hz / 1000)`를 계산하고, `tick_count < target`인 동안 `hlt`로 대기한다. `hlt`는 다음 인터럽트까지 CPU를 절전 상태로 두므로, 매 IRQ0마다 깨어나 조건을 확인한다.

- **IRQ0 연결**: `interrupts.c`의 `handle_irq`에서 `irq == 0`이면 `timer_irq()`를 호출한다. `timer_irq()`는 `tick_count++`만 한다. EOI는 handle_irq가 공통으로 보낸다.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 후 3초 타임아웃 종료
make clean
```

## 완료 기준

- `timer: PIT 100Hz IRQ0 ready` 출력
- `sleep 500ms: ticks before=N` 출력 후 500ms 대기
- `sleep done: ticks after=M (delta=50)` — 100Hz × 0.5s = 50 tick

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | timer.c 빌드 대상 추가 |
| `boot/interrupts.c` | 수정 | IRQ0 핸들러에서 timer_irq() 호출 추가 |
| `boot/kernel.c` | 수정 | timer_init() 호출, timer_sleep(500) 시연 |
| `boot/timer.c` | 신규 | PIT 채널 0 초기화, tick 카운터, timer_sleep() |
| `boot/timer.h` | 신규 | 타이머 헤더 |

## 다음 단계 힌트

- `16-kernel-monitor`: 키보드 입력 기반 커널 모니터와 간단한 명령 파서.
