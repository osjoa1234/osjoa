# 07 — C 커널

**목표**: 06의 FAT16 + BIOS LBA 적재 경로는 그대로 두고, `KERNEL.BIN` payload를 순수 asm 바이너리에서 **16비트 진입 stub + 32비트 freestanding C 커널**로 교체한다. 이번 단계의 핵심은 부트로더가 읽어 온 바이트열을 바로 실행하는 구조는 유지하면서, 보호모드 진입 이후의 본체 로직을 C로 넘기는 것이다.

**06에서 이어짐**: FAT16 루트 디렉토리 검색, FAT 체인 추적, BIOS `AH=42` LBA 읽기, `KERNEL.BIN`을 `0x7E00`에 적재하는 부트섹터 흐름은 유지한다. 바뀌는 부분은 payload 내부 구조다. 06에서는 `kernel.asm` 하나가 A20 활성화부터 VGA 출력까지 모두 맡았지만, 07에서는 `entry.asm`이 real mode 진입점과 보호모드 전환만 맡고, 실제 화면 출력은 `kernel.c`의 `kernel_main()`이 담당한다.

메시지도 `...loaded from FAT16 via BIOS LBA`에서 `...C kernel loaded from FAT16 via BIOS LBA`로 이어진다. 이 문구가 뜨면 = 06의 디스크 적재 경로는 그대로 통과했고, 보호모드 이후 제어가 C 함수까지 정상 전달되었다는 뜻이다.

상위 컨텍스트(환경·툴체인·실습 진행 방식)는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
07-c-kernel/
├── boot/
│   ├── boot.asm     # FAT16 VBR: BPB + 루트 디렉토리 스캔 + FAT 체인 적재 + BIOS LBA 읽기
│   ├── entry.asm    # payload 진입점: 16비트 시작 → A20 → GDT/PM → C 함수 호출
│   └── kernel.c     # 32비트 freestanding C 커널: VGA 텍스트 모드 출력
├── linker.ld        # flat binary를 0x7E00 기준으로 배치하는 링커 스크립트
├── Makefile
└── build/           # 산출물 (생성됨)
```

## 도구

```bash
make tools     # sudo apt install -y mtools dosfstools gcc-multilib g++-multilib
```

- `gcc-multilib`, `g++-multilib`: 32비트 freestanding 오브젝트 생성.
- `ld`, `objcopy`: ELF를 `KERNEL.BIN` flat binary로 변환.
- `mtools`, `dosfstools`: FAT16 이미지 생성과 `KERNEL.BIN` 복사.

## 핵심 개념

- **부트섹터는 그대로, payload만 교체**: `boot.asm`은 06과 같은 방식으로 `KERNEL.BIN`을 `0x7E00`에 올리고 `jmp 0x0000:0x7E00`로 넘긴다. 즉 07의 학습 포인트는 파일 로딩이 아니라, 적재된 바이너리 내부에서 asm과 C가 어떻게 역할을 나누는지다.

- **16비트 진입 stub**: CPU가 payload에 도착하는 시점은 아직 real mode다. 그래서 첫 바이트는 여전히 16비트 코드여야 하며, 여기서 A20 활성화/검증과 GDT 로드, `cr0.PE` 설정, far jump까지 처리한다.

- **C는 보호모드 이후에만 실행**: `kernel.c`는 `kernel_main()` 하나만 내보내고, 실제 호출은 32비트로 전환된 뒤 `entry.asm`에서 한다. 이렇게 하면 C 코드는 BIOS 인터럽트나 real mode 세그먼트 제약을 신경 쓰지 않고, 선형 주소 공간 기준으로 VGA 메모리를 다룰 수 있다.

- **링커 스크립트가 load address를 고정**: 부트로더는 payload를 항상 `0x7E00`에 적재한다. `linker.ld`는 `.text/.rodata/.data/.bss`의 기준 주소를 여기에 맞춰 배치하고, `objcopy -O binary`로 ELF의 섹션 내용을 이어 붙인 `KERNEL.BIN`을 만든다.

- **freestanding C**: 표준 라이브러리 없이 돌아가므로 `printf`, `memset`, `main` 같은 런타임을 쓰지 않는다. 대신 VGA 텍스트 버퍼를 직접 건드리는 최소 루틴만 C 안에 둔다.

- **출력 루틴의 소유권 이동**: 06에서는 asm이 화면 지우기와 문자열 출력을 모두 처리했다. 07에서는 보호모드 진입까지만 asm이 맡고, 화면 지우기와 문자열 쓰기는 C 함수가 담당한다. 이 지점이 다음 단계들에서 C 커널을 점점 키워 갈 출발점이 된다.

## 명령

```bash
make tools      # (최초 1회) FAT 도구 + 32비트 C 빌드 패키지 설치
make            # build/os.img 생성
make run        # QEMU GUI에서 실행
make run-nogui  # 콘솔 모드
make clean
```

이미지 안에 파일이 들어갔는지 확인:

```bash
mdir -i build/os.img ::
```

GUI 없이 VGA 텍스트 버퍼를 확인하려면 QEMU 모니터에서 직접 읽는다:

```bash
qemu-system-i386 -drive format=raw,file=build/os.img -display none -monitor stdio -serial none
(qemu) xp /160bx 0xb8000
(qemu) quit
```

## 완료 기준

- `make run` 시 payload가 좌상단에 `Hello world -- protected mode (32-bit), C kernel loaded from FAT16 via BIOS LBA`를 출력한다.
- `build/boot.bin`이 정확히 512바이트다.

  ```bash
  stat -c%s build/boot.bin
  ```

- `build/KERNEL.BIN`이 ELF가 아니라 flat binary이며, `file build/kernel.elf build/KERNEL.BIN`으로 각각 ELF/데이터 파일로 구분된다.

## 다음 단계 힌트

- `08-grub-multiboot`: FAT16 파일 검색과 자체 보호모드 진입을 GRUB로 대체해 커널 적재를 단순화한다.
- `09-vga-print`: C 쪽 출력 루틴을 `printf` 스타일 인터페이스와 커서 관리로 확장한다.