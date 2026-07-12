# 09 — VGA Text Mode Console

**목표**: 08의 `GRUB2 + Multiboot` 부팅 경로는 그대로 유지하고, `kernel.c`의 고정 위치 문자열 출력 코드를 **커서 기반 VGA 텍스트 콘솔**로 확장한다. 이번 단계의 핵심은 "0xB8000에 문자열 몇 개를 직접 써 보는 것"에서 끝나지 않고, 이후 단계에서 계속 재사용할 수 있는 `putchar`/`printf` 계층을 만드는 것이다.

**08에서 이어짐**: 08에서는 `kernel_main`이 VGA 메모리의 정해진 셀에 메시지와 16진수를 직접 기록했다. 09에서는 부팅 방식과 32비트 진입 흐름은 그대로 두고, 화면 출력 쪽만 `row`, `column`, `color` 상태를 갖는 콘솔로 바꾼다. 메시지도 `...loaded by GRUB2 via Multiboot` 다음 줄에 `VGA console ready...`가 이어져, 커널이 이제 화면을 조금 더 운영체제답게 다루기 시작했음을 보여 준다.

상위 컨텍스트(환경·툴체인·실습 진행 방식)는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
09-vga-print/
├── boot/
│   ├── entry.asm    # Multiboot 헤더 + 32비트 진입점 + C 호출
│   └── kernel.c     # VGA 텍스트 콘솔 + 작은 printf
├── grub/
│   └── grub.cfg     # GRUB 메뉴와 커널 적재 설정
├── linker.ld        # ELF 커널 배치 주소와 섹션 순서
├── Makefile
└── build/           # 산출물 (생성됨)
```

## 도구

```bash
make tools     # sudo apt install -y xorriso grub-pc-bin grub-common gcc-multilib g++-multilib
```

- `grub-pc-bin`, `grub-common`, `xorriso`: GRUB 부팅 ISO 생성.
- `gcc-multilib`, `g++-multilib`: 32비트 freestanding 커널 빌드.

## 핵심 개념

- **VGA 텍스트 버퍼 직접 제어**: 화면 메모리 `0xB8000`의 각 셀은 문자 1바이트 + 색상 1바이트다. 이제 커널이 문자열 시작 위치를 일일이 계산하지 않고, 현재 커서 위치를 기준으로 다음 문자를 이어서 쓴다.

- **커서 상태 유지**: `row`, `column`, `color`를 전역 콘솔 상태로 들고 가면서 개행과 다음 출력 위치를 결정한다. 이 상태가 생기면 이후 단계에서 `printf("irq=%u\n", irq);` 같은 로그를 자연스럽게 누적할 수 있다.

- **자동 줄바꿈과 스크롤**: 열이 80칸을 넘으면 다음 줄로 내려가고, 25행을 넘으면 한 줄씩 위로 당긴 뒤 마지막 줄을 비운다. 지금은 화면에 몇 줄만 찍더라도, 이후 인터럽트/키보드 단계에서는 이 동작이 필수다.

- **색상 분리**: 성공 메시지, 주소 출력, 일반 로그를 다른 색으로 표시할 수 있다. 그래픽 모드가 아니라 텍스트 셀 속성만 바꾸는 수준이지만, 초기 커널 디버깅에는 충분히 실용적이다.

- **작은 `printf` 구현**: libc 전체가 아니라 `%s`, `%c`, `%d`, `%u`, `%x`, `%X`, `%%` 정도만 지원한다. 이 정도면 주소, 상태 코드, 카운터, 간단한 문자열 로그를 모두 출력할 수 있다.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI에서 ISO 부팅
make run-nogui  # GUI 없이 QEMU 모니터 접속
make clean
```

커널이 Multiboot 형식인지 확인:

```bash
grub-file --is-x86-multiboot build/kernel.elf
```

ISO 내용 확인:

```bash
xorriso -indev build/os.iso -find / -type f -print
```

GUI 없이 VGA 텍스트 버퍼를 확인하려면 QEMU 모니터에서 직접 읽는다:

```bash
qemu-system-i386 -cdrom build/os.iso -display none -monitor stdio -serial none
(qemu) xp /160bx 0xb8000
(qemu) quit
```

## 완료 기준

- `make run` 시 첫 줄에 `Hello world -- protected mode (32-bit), C kernel loaded by GRUB2 via Multiboot`가 출력된다.
- 다음 줄들에 `VGA console ready...`, `multiboot info at 0x...`, `printf check: ...`가 색을 달리해 출력된다.
- 출력 문자열이 더 길어지더라도 커서, 개행, 줄바꿈, 스크롤을 처리하는 코드 경로가 준비되어 있다.
- `grub-file --is-x86-multiboot build/kernel.elf`가 성공 종료한다.
- `build/os.iso`가 생성되고, `xorriso -indev build/os.iso -find / -type f -print`에 `/boot/kernel.elf`, `/boot/grub/grub.cfg`가 보인다.

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | console.c 빌드 대상 추가 |
| `boot/console.c` | 신규 | VGA 텍스트 콘솔 — 커서, 자동 줄바꿈/스크롤, printf |
| `boot/console.h` | 신규 | 콘솔 헤더 |
| `boot/kernel.c` | 수정 | console_init() 호출, 출력을 콘솔 함수로 전환 |

## 다음 단계 힌트

- `10-interrupts`: PIC 리맵과 IDT를 올린 뒤, 예외/IRQ 진입을 이 콘솔에 출력해 디버깅한다.
- `11-keyboard`: 키 입력 스캔 코드를 해석해, 이 콘솔에 문자와 로그를 다시 뿌린다.