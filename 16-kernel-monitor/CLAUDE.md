# 16 — 커널 모니터

**목표**: 키보드 입력을 링 버퍼로 수신하고, `readline` + 명령 파서로 대화형 커널 모니터를 구현한다.

**15에서 이어짐**: 15에서 PIT 타이머와 keyboard_irq가 준비됐다. 16에서는 키보드 IRQ 핸들러를 직접-에코 방식에서 링 버퍼 방식으로 바꾸고, `monitor.c`를 추가해 커맨드 루프를 구성한다. kernel.c의 마지막은 `halt_forever()` 대신 `monitor_run()`으로 대체된다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
16-kernel-monitor/
├── boot/
│   ├── console.c/h        # 수정: '\b' 처리 추가
│   ├── entry.asm          # 15와 동일
│   ├── interrupts.asm/c/h # 15와 동일
│   ├── keyboard.c/h       # 수정: 링 버퍼 + keyboard_getchar(), 직접 에코 제거, '\b' 스캔코드 매핑
│   ├── kheap.c/h          # 15와 동일
│   ├── monitor.c          # NEW: readline, 명령 파서, monitor_run()
│   ├── monitor.h          # NEW
│   ├── paging.c/h         # 15와 동일
│   ├── phys_mem.c/h       # 15와 동일
│   ├── timer.c/h          # 15와 동일
│   └── kernel.c           # 수정: monitor_run() 호출, 데모 제거
├── grub/grub.cfg
├── linker.ld
├── Makefile               # monitor.o 추가
└── build/
```

## 핵심 개념

- **링 버퍼**: `kb_buf[256]` + `kb_head`/`kb_tail`. IRQ 핸들러는 `kb_tail`에 쓰고, `keyboard_getchar()`은 `kb_head`에서 읽는다. 단일 코어이므로 head/tail 분리만으로 레이스 없음.

- **`keyboard_getchar()`**: `kb_head == kb_tail`인 동안 `hlt`로 대기. 어떤 인터럽트(IRQ0·IRQ1)로도 깨어나 조건 재확인 → 키 입력 IRQ면 바로 문자 반환.

- **`'\b'` 스캔코드**: 스캔코드 0x0E는 기존 테이블에서 0(무시)이었음. `sc_lower[14]`와 `sc_upper[14]`를 `'\b'`로 바꿔 백스페이스를 버퍼에 전달.

- **console `'\b'` 처리**: `console_put_char`에 `'\b'` 케이스 추가. `column > 0`이면 커서를 한 칸 왼쪽으로만 이동(지우지 않음). `readline`이 `"\b \b"` 패턴으로 직접 지운다: ① `'\b'`로 뒤로 이동 → ② `' '`로 덮어쓰기 → ③ `'\b'`로 다시 이동.

- **`monitor_run()`**: 무한 루프로 `"monitor> "` 프롬프트 출력 → `readline` → `dispatch`. `kernel_main`의 끝이 이 함수 호출로 대체되므로 `halt_forever()`는 매직 불일치 오류 경로에만 남는다.

- **지원 명령**: `help`, `clear`, `mem`, `ticks`, `sleep <ms>`, `echo <text>`.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI (키보드 인터랙티브)
make run-nogui  # debugcon 로그 확인 후 3초 타임아웃 종료
make clean
```

## 완료 기준

- 부팅 후 `kernel monitor ready -- type 'help' for commands` 출력
- `monitor> ` 프롬프트에서 명령 입력·실행 가능
- 백스페이스로 입력 문자 지우기 동작

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | monitor.o 빌드 대상 추가 |
| `boot/console.c` | 수정 | console_put_char에 '\b' 커서 이동 추가 |
| `boot/keyboard.h` | 수정 | keyboard_getchar() 선언 추가 |
| `boot/keyboard.c` | 수정 | 링 버퍼 도입, irq 핸들러를 버퍼링 방식으로 변경, '\b' 스캔코드 매핑, keyboard_getchar() 추가 |
| `boot/kernel.c` | 수정 | monitor_run() 호출로 교체, 데모 섹션 제거 |
| `boot/monitor.c` | 신규 | readline, 명령 파서, monitor_run() |
| `boot/monitor.h` | 신규 | 모니터 헤더 |

## 다음 단계 힌트

- `17-kernel-threads`: 커널 태스크 구조체, 문맥 전환, 첫 커널 쓰레드 실행.
