# 10 — PIC Remap, IDT, and Interrupt Handlers

**목표**: 09의 `VGA console + printf`를 그대로 가져와, 이제 커널이 **IDT를 직접 올리고 PIC를 리맵한 뒤 예외와 하드웨어 IRQ를 받아 출력**하게 만든다. 이번 단계의 핵심은 인터럽트를 "이론적으로 알고 있는 상태"에서 끝내지 않고, `int3`와 실제 `IRQ0` 타이머 틱이 커널 코드로 들어오는 경로를 눈으로 확인하는 것이다.

**09에서 이어짐**: 09에서는 GRUB2가 넘겨준 32비트 환경에서 콘솔과 `printf`를 만들었다. 10에서는 그 출력 계층을 그대로 유지한 채, `kernel_main`이 문자열을 찍은 다음 `interrupts_init()`으로 IDT 256개 엔트리를 채우고 PIC를 `0x20/0x28`로 옮긴다. 메시지도 `VGA console ready...` 다음에 `IDT ready...`, `interrupt demo...`, 그리고 실제 `interrupt 0x03`, `interrupt 0x20` 로그가 이어져 학습 흐름이 끊기지 않는다.

상위 컨텍스트(환경·툴체인·실습 진행 방식)는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
10-interrupts/
├── boot/
│   ├── console.c        # VGA 콘솔 + printf + debugcon 미러링
│   ├── console.h
│   ├── entry.asm        # Multiboot 헤더 + 32비트 진입점 + C 호출
│   ├── interrupts.asm   # 256개 인터럽트 스텁 + 공통 진입
│   ├── interrupts.c     # IDT 구성, PIC 리맵, 예외/IRQ 디스패치
│   ├── interrupts.h
│   └── kernel.c         # 콘솔 초기화 후 인터럽트 데모 시작
├── grub/
│   └── grub.cfg
├── linker.ld
├── Makefile
└── build/               # 산출물 (생성됨)
```

## 도구

```bash
make tools
```

- `grub-pc-bin`, `grub-common`, `xorriso`: GRUB 부팅 ISO 생성.
- `gcc-multilib`, `g++-multilib`: 32비트 freestanding 커널 빌드.
- `qemu-system-x86`: 인터럽트 데모 실행.

## 핵심 개념

- **IDT 직접 구성**: 256개 엔트리를 커널이 직접 채우고 `lidt`로 적재한다. 이제 CPU 예외와 PIC IRQ가 C 코드까지 들어올 수 있는 테이블을 OS가 소유하게 된다.

- **PIC 리맵**: 기본 PIC 벡터 `0x08`~`0x0F`는 CPU 예외와 겹치므로, 마스터/슬레이브를 `0x20`과 `0x28`로 옮긴다. 이 단계부터 `IRQ0` 타이머는 `int 0x20`으로 보이게 된다.

- **공통 스텁 + C 디스패치**: 어셈블리 스텁이 레지스터를 저장하고 벡터 번호와 에러 코드를 맞춰 넣은 뒤, C의 `interrupt_dispatch()`로 넘긴다. 이후 단계에서 키보드, 페이지 폴트, 시스템 콜처럼 종류가 늘어나도 디스패치 지점은 유지된다.

- **예외와 IRQ를 같은 콘솔에 기록**: `int3` 브레이크포인트와 실제 PIT `IRQ0` 타이머 틱을 같은 `console_printf()` 경로로 출력한다. 09에서 만든 출력 계층이 이제 디버깅 도구로 승격된다.

- **헤드리스 검증 경로**: `console.c`는 QEMU debug console 포트 `0xE9`에도 같은 문자를 미러링한다. 그래서 GUI 없이도 `make run-nogui`로 인터럽트 로그를 바로 읽을 수 있다.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI에서 ISO 부팅
make run-nogui  # debugcon으로 로그를 터미널에 출력하고 데모 종료
make clean
```

Multiboot 형식 확인:

```bash
grub-file --is-x86-multiboot build/kernel.elf
```

ISO 내용 확인:

```bash
xorriso -indev build/os.iso -find / -type f -print
```

## 완료 기준

- `make run` 시 09의 기존 문자열들 뒤에 `IDT ready...`, `interrupt demo...`가 이어서 출력된다.
- 이어서 `interrupt 0x03 Breakpoint ...` 로그가 보인다.
- `IRQ0`가 `interrupt 0x20 irq=0 tick=...` 형태로 5번 들어오고, 마지막에 `PIC timer demo complete...`가 출력된다.
- `grub-file --is-x86-multiboot build/kernel.elf`가 성공 종료한다.
- `make run-nogui`가 debugcon 로그를 출력한 뒤 종료한다.

## 다음 단계 힌트

- `11-keyboard`: 지금 만든 IDT/PIC 경로에서 `IRQ1`만 열고 스캔 코드를 읽어 콘솔에 문자로 표시한다.
- `12-paging`: 페이지 폴트 벡터 `0x0E`를 지금의 예외 디스패치 경로에 연결해, fault 주소와 에러 코드를 찍으며 가상 메모리로 넘어간다.