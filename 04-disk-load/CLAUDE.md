# 04 — Disk Load

**목표**: 부트섹터 512바이트로는 부족해지는 시점을 맞아, 디스크에서 추가 섹터를 `int 0x13`으로 읽어와(재시도/CF 에러체크) 적재한 코드로 점프한다.
BIOS는 1번 섹터(부트섹터)만 `0x7C00`에 올려준다. 나머지 코드는 부트섹터가 직접 디스크에서 읽어 메모리에 적재해야 한다.

**03에서 이어짐**: 03의 real mode + A20 검증/활성화 블록은 그대로 두고, 보호모드(`BITS 32`) 출력 블록을 **2번째 섹터(`stage2`)로 분리**한다. 부트섹터는 A20 검증 직후 `int 0x13`으로 sector 2를 `0x7E00`(부트섹터 바로 뒤)에 읽어온 다음, GDT/PE/far jump으로 그 적재 영역으로 진입한다. 인사말도 `...(32-bit), A20 enabled` → `...(32-bit), loaded from disk`로 한 단계 발전시켜, PM 메시지가 떴다는 사실 자체가 "디스크 로드 성공"의 증거가 되게 한다(A20 활성 + PM 진입 + stage2 적재가 한 번에 증명됨).

상위 컨텍스트(환경·툴체인·실습 진행 방식)는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```
04-disk-load/
├── boot/
│   └── boot.asm     # [sector 1] real mode → A20 → 디스크 로드 → GDT/PE → far jump
│                    # [sector 2] stage2: 32비트 VGA 출력 (디스크에서 적재됨)
├── Makefile
└── build/           # 산출물 (생성됨)
```

## 핵심 개념

- **부트 드라이브 보존**: BIOS가 진입 시 `dl`에 부트 드라이브 번호를 넘겨준다. 시작 직후 `[boot_drive]`에 저장해 두고, `int 0x13` 호출마다 `dl`에 다시 넣는다(플로피/HDD 어느 쪽으로 부팅해도 동작).
- **`int 0x13` AH=02 (read sectors)**: `al`=섹터 수, `ch`=cylinder, `cl`=sector(1-based), `dh`=head, `dl`=drive, `es:bx`=버퍼. sector 2를 cyl 0 / head 0로 읽으므로 디스크 지오메트리와 무관하게 항상 유효. A20 wraparound 테스트가 `es`를 `0xFFFF`로 바꿔 놓으므로, 읽기 전에 `es=0`으로 되돌려 버퍼를 `0000:7E00`(phys `0x7E00`)로 맞춘다.
- **재시도 + CF 에러체크**: 읽기 실패는 CF로 표시된다. `jnc`로 성공 분기, 실패면 `int 0x13` AH=00(reset disk system) 후 재시도. 재시도 카운터는 `int 0x13`이 `cx`를 입력/출력으로 쓰므로 레지스터 대신 메모리(`[retries]`)에 두고 `dec`/`jnz`로 센다. 횟수 소진 시 real mode에서 에러를 찍고 멈춤.
- **stage2로 점프**: 부트섹터 끝(`dw 0xAA55`) 뒤에 두 번째 섹터가 이어진다. `ORG 0x7C00`이 파일 전체에 적용되므로 `stage2`/`protected_start` 레이블은 phys `0x7E00`으로 해석되고, 디스크에서도 정확히 그 위치에 적재한다. 따라서 `jmp 0x08:protected_start`가 적재된 코드로 그대로 들어간다 — 로드가 실패했다면 거기엔 쓰레기뿐이라 PM 메시지가 안 뜬다.

## 명령

```bash
make            # build/boot.bin 생성 (1024바이트 = 2섹터)
make run        # QEMU GUI에서 실행
make run-nogui  # 콘솔 모드 (Ctrl+A 다음 X로 종료)
make clean
```

VGA 출력은 시리얼로 안 나오므로, GUI 없이 확인하려면 QEMU 모니터로 텍스트 버퍼를 직접 읽는다:
```bash
(sleep 2; printf 'xp /16bx 0xb8000\n'; printf 'quit\n') \
  | qemu-system-i386 -drive format=raw,file=build/boot.bin -display none -monitor stdio -serial none
# 0xb8000부터 [문자][속성 0x0f] 교차 → 48 'H', 65 'e', 6c 'l' ... = "Hello "
```

## 완료 기준

- `make run` 시 real mode 메시지가 잠깐 보이고, 디스크에서 적재된 stage2가 좌상단에 `Hello world -- protected mode (32-bit), loaded from disk` 출력. (읽기 실패 시 real mode에 `Disk read failed -- stage2 not loaded`만 뜸)
- `build/boot.bin`이 정확히 1024바이트(2섹터).
  ```bash
  stat -c%s build/boot.bin   # 1024
  ```

## 다음 단계 힌트

- `05-c-kernel`: 적재 영역(stage2)을 C 커널로 교체 — `linker.ld` 레이아웃 + asm→C 점프. 부트섹터의 디스크 로드는 이 단계에서 "커널 적재"로 자연스럽게 재사용된다(커널이 1섹터보다 커지면 `al`로 읽는 섹터 수를 늘림). GRUB(Multiboot) 위임은 `06-grub-multiboot`에서 따로 다룸.
- VGA 직접 쓰기 로직은 `07-vga-print`에서 `print` 함수 / 스크롤 / 커서 처리로 확장.
