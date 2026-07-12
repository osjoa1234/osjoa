# 08 — GRUB2 Multiboot

**목표**: 07의 `entry.asm + kernel.c` 커널 구조는 유지하고, `boot.asm`이 직접 FAT16에서 `KERNEL.BIN`을 찾던 부팅 경로를 **GRUB2 + Multiboot**로 교체한다. 이번 단계의 핵심은 "커널 본체는 거의 그대로 두고, 로더를 직접 작성하던 부분을 표준 부트로더에 넘기면 무엇이 단순해지는가"를 확인하는 것이다.

**07에서 이어짐**: 07에서는 FAT16 이미지 안의 `KERNEL.BIN`을 `0x7E00`에 적재한 뒤, 16비트 진입 stub가 A20과 보호모드 전환을 맡고 `kernel.c`로 제어를 넘겼다. 08에서는 화면 출력 주체가 여전히 `kernel.c`라는 점은 같지만, 적재와 CPU 초기 모드 준비는 GRUB2가 맡는다. 그래서 새 단계에는 `boot.asm`이 없고, `entry.asm`은 이제 Multiboot 헤더와 스택 준비, C 함수 호출만 담당한다.

메시지도 `...C kernel loaded from FAT16 via BIOS LBA`에서 `...C kernel loaded by GRUB2 via Multiboot`로 이어진다. 이 문구가 보이면 = 07의 C 커널 흐름은 유지된 채, 부팅 경로만 GRUB2로 교체된 것이다.

상위 컨텍스트(환경·툴체인·실습 진행 방식)는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
08-grub-multiboot/
├── boot/
│   ├── entry.asm    # Multiboot 헤더 + 32비트 진입점 + C 호출
│   └── kernel.c     # 32비트 freestanding C 커널: VGA 텍스트 모드 출력
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

- **직접 로딩 대신 표준 로더 사용**: 07까지는 부트섹터가 디스크 포맷과 파일 위치를 이해해야 했다. 08에서는 GRUB2가 커널 ELF를 읽고 메모리에 올려 주므로, 커널 쪽 코드는 "이미 보호모드로 들어온 뒤 무엇을 할 것인가"에 더 집중할 수 있다.

- **Multiboot 헤더가 커널의 계약**: GRUB는 커널 파일 앞부분에서 Multiboot 헤더를 찾아 이 커널이 어떤 방식으로 부팅되어야 하는지 판단한다. `entry.asm`의 첫 섹션에 이 헤더를 두는 이유가 여기에 있다.

- **진입 시점이 이미 32비트**: 07의 `entry.asm`은 real mode에서 시작했기 때문에 A20, GDT, `cr0.PE` 설정이 필요했다. 08의 `entry.asm`은 GRUB가 넘겨준 32비트 보호모드에서 시작하므로, 스택을 잡고 C 함수만 호출하면 된다.

- **커널 포맷이 flat binary에서 ELF로 이동**: GRUB는 ELF 실행 파일을 직접 적재할 수 있다. 그래서 이번 단계는 `KERNEL.BIN`을 만들지 않고, `kernel.elf`를 그대로 ISO 안의 `/boot/kernel.elf`로 넣는다.

- **GRUB 설정도 코드의 일부**: `grub.cfg`의 `multiboot /boot/kernel.elf` 한 줄이 "이 파일을 Multiboot 커널로 적재하라"는 선언이다. 이전 단계의 BPB/FAT 루트 디렉토리 스캔 코드가 하던 역할을, 이제는 이 설정과 GRUB 자체가 대신한다.

- **Multiboot 정보 포인터 확인**: `kernel.c`는 `eax`의 magic 값과 `ebx`의 Multiboot 정보 포인터를 받아 화면에 표시한다. 이 값이 보이면 GRUB handoff가 실제로 일어났다는 것을 확인할 수 있다.

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

- `make run` 시 좌상단에 `Hello world -- protected mode (32-bit), C kernel loaded by GRUB2 via Multiboot`가 출력된다.
- 둘째 줄에 `multiboot info at 0x...`가 출력된다.
- `grub-file --is-x86-multiboot build/kernel.elf`가 성공 종료한다.
- `build/os.iso`가 생성되고, `xorriso -indev build/os.iso -find / -type f -print`에 `/boot/kernel.elf`, `/boot/grub/grub.cfg`가 보인다.

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | GRUB ISO 생성 빌드로 전환; FAT 이미지 관련 룰 제거 |
| `boot/boot.asm` | 삭제 | FAT16 부트로더 제거 (GRUB가 대체) |
| `boot/entry.asm` | 수정 | Multiboot 헤더 추가, real mode 진입 코드 제거, 스택 준비 후 C 호출만 남김 |
| `boot/kernel.c` | 수정 | Multiboot magic/info 포인터 출력 추가; 메시지 변경 |
| `grub/grub.cfg` | 신규 | GRUB 메뉴 설정 |
| `linker.ld` | 수정 | Multiboot ELF 표준 적재 주소로 재배치 |

## 다음 단계 힌트

- `09-vga-print`: 지금의 단순 문자열 출력 루틴을 커서, 개행, 색상, 숫자 포맷팅이 있는 콘솔 계층으로 확장한다.
- `10-interrupts`: GRUB가 넘긴 환경 위에서 IDT와 PIC를 올려 인터럽트를 다룰 준비를 한다.