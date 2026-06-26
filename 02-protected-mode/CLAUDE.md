# 02 — Protected Mode

**목표**: 16비트 real mode에서 32비트 보호모드로 전환한다.
GDT(Global Descriptor Table)를 정의하고 `cr0`의 PE 비트를 세운 뒤 far jump으로 CS를 갈아끼운다. 보호모드에서는 BIOS `int`를 못 쓰므로 VGA 텍스트 버퍼(`0xB8000`)에 직접 쓴다.

**01에서 이어짐**: 01의 real-mode `Hello world` 출력 코드(`print` 루프)를 그대로 출발점으로 두고, 출력 후 `hlt`로 멈추던 자리를 보호모드 전환으로 바꾼다. 인사말도 `Hello world -- real mode (16-bit)` → `Hello world -- protected mode (32-bit)`로 이어가 모드 전환이 한눈에 드러나게 한다. 01과 달라지는 점은 `sti` 생략뿐이다 — IDT 없이 PM에 진입하므로 인터럽트가 들어오면 처리할 수 없어 `cli` 상태를 유지한다(코드에는 주석을 달지 않고 이유는 여기 문서에 둔다).

상위 컨텍스트(환경·툴체인·실습 진행 방식)는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```
02-protected-mode/
├── boot/
│   └── boot.asm     # real mode 진입 → GDT → PE → far jump → 32비트 VGA 출력
├── Makefile
└── build/           # 산출물 (생성됨)
```

## 핵심 개념

- **GDT**: null + code(`0x08`) + data(`0x10`) 디스크립터. flags `11001111b` = 4KB granularity + 32비트, limit `0xFFFFF` → 4GB flat 모델.
- **PE 비트**: `cr0`의 bit 0을 1로. 이 순간 모드 플래그가 바뀌지만 CS는 아직 real mode 값.
- **far jump**: `jmp 0x08:protected_start`로 파이프라인을 비우고 CS를 보호모드 코드 셀렉터로 교체. 여기부터 `BITS 32`.
- 보호모드에서 `int 0x10` 사용 불가 → `0xB8000`에 `[문자][속성]` 2바이트 단위로 직접 기록 (`0x0F` = 검정 배경 흰 글자).

## 명령

```bash
make            # build/boot.bin 생성
make run        # QEMU GUI에서 실행
make run-nogui  # 콘솔 모드 (Ctrl+A 다음 X로 종료)
make clean
```

## 완료 기준

- `make run` 시 real mode 메시지가 잠깐 출력됐다가, 보호모드 진입 직후 화면을 비우고 보호모드 메시지가 화면 좌상단에 VGA로 표시됨.
- `build/boot.bin`이 정확히 512바이트.
  ```bash
  stat -c%s build/boot.bin   # 512
  ```

## 다음 단계 힌트

- `03-a20`: A20 게이트를 활성화하고 1MB 위 주소가 wraparound 없이 접근되는지 검증.
- `04-disk-load`: 부트섹터 512바이트로는 부족해짐. 디스크에서 추가 섹터를 읽어와(`int 0x13`, 재시도/CF 에러체크) 적재한 코드로 점프.
- `05-c-kernel`: 적재 영역을 C 커널로 교체 — `linker.ld` + asm→C 점프. GRUB(Multiboot) 위임은 `06-grub-multiboot`에서 따로 다룸.
- VGA 직접 쓰기 로직은 `04-vga-print`에서 `print` 함수 / 스크롤 / 커서 처리로 확장.
