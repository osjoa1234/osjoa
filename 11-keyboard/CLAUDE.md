# 11 — 키보드 드라이버

**목표**: 10의 IDT/PIC 경로를 그대로 유지한 채, IRQ1만 열고 PS/2 키보드 스캔코드를 읽어 VGA 콘솔에 문자로 출력한다. `int3` 브레이크포인트 데모 대신 실제 키 입력이 콘솔에 나타나는 경험을 제공한다.

**10에서 이어짐**: 10에서는 IDT 256개 엔트리와 PIC 리맵(0x20/0x28)을 만들고 IRQ0 타이머 데모로 검증했다. 11에서는 그 인프라를 유지하되 타이머 데모를 제거하고, `interrupts_init()`이 어떤 IRQ도 unmask하지 않도록 변경한다. `keyboard_init()`이 IRQ1을 열고, `handle_irq()` 안에서 irq==1일 때 `keyboard_irq()`를 호출하는 형태로 키보드를 붙인다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
11-keyboard/
├── boot/
│   ├── console.c        # 10과 동일
│   ├── console.h
│   ├── entry.asm        # 10과 동일
│   ├── interrupts.asm   # 10과 동일
│   ├── interrupts.c     # IRQ1→keyboard_irq 디스패치, interrupts_unmask_irq 추가
│   ├── interrupts.h     # interrupts_unmask_irq 공개, 타이머 관련 제거
│   ├── keyboard.c       # NEW: 스캔코드 테이블 + shift 상태 + keyboard_irq
│   ├── keyboard.h       # NEW
│   └── kernel.c         # keyboard_init() 호출, 키 입력 대기 루프
├── grub/
│   └── grub.cfg
├── linker.ld
├── Makefile
└── build/
```

## 핵심 개념

- **IRQ1 = PS/2 키보드**: 키가 눌리면 8042 컨트롤러가 IRQ1을 발생시키고, 스캔코드는 포트 `0x60`에 적재된다. 핸들러가 `inb(0x60)`으로 읽어야 다음 IRQ가 발생할 수 있다.

- **스캔코드 Set 1**: BIOS가 넘겨준 기본 형식. 키를 누를 때(make code)와 뗄 때(break code = make | 0x80)가 구분된다. break code는 무시하고 make code만 처리한다.

- **shift 상태 추적**: make code 0x2A(왼쪽 Shift), 0x36(오른쪽 Shift)가 오면 `shift_held=1`, break code 0xAA/0xB6에서 `shift_held=0`. 나머지 make code는 `shift_held` 값으로 `sc_lower`/`sc_upper` 테이블을 선택해 ASCII로 변환한다.

- **interrupts_init에서 IRQ 분리**: `interrupts_init()`은 IDT 적재와 PIC 리맵만 수행하고, IRQ unmask는 각 드라이버(`keyboard_init`)가 `interrupts_unmask_irq()`를 호출해 직접 제어한다.

- **`run-nogui` 검증**: 키보드는 GUI 없이 자동 종료할 방법이 없으므로, `timeout 3 qemu... || true`로 3초 부팅 후 강제 종료한다. debugcon 로그에 `keyboard ready: IRQ1 unmasked`가 보이면 정상.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI — 키 입력 시 콘솔에 문자 출력
make run-nogui  # debugcon 로그 확인 후 3초 타임아웃 종료
make clean
```

## 완료 기준

- `make run` 시 `keyboard demo: type to echo keystrokes` 출력 후 키 입력이 흰 글자로 에코된다.
- Shift 키를 누른 채 알파벳을 입력하면 대문자가 나온다.
- `make run-nogui`가 debugcon 로그를 출력하고 오류 없이 종료한다.
- `grub-file --is-x86-multiboot build/kernel.elf`가 성공 종료한다.

## 다음 단계 힌트

- `12-paging`: 페이지 폴트 벡터 `0x0E`를 지금의 예외 디스패치 경로에 연결해, fault 주소와 에러 코드를 찍으며 가상 메모리로 넘어간다.
