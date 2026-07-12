# 03 — A20 Gate

**목표**: A20 어드레스 라인을 활성화해 1MB 위 주소가 wraparound 없이 접근되게 하고, 그 사실을 직접 검증한다.
부팅 직후 A20이 꺼져 있으면 20번째 주소 비트가 0으로 고정되어 1MB 이상 주소가 0으로 되감긴다(8086 호환 동작). 키보드 컨트롤러(8042)로 A20을 켠 뒤 wraparound 테스트로 켜졌는지 확인한다.

**02에서 이어짐**: 02 코드를 거의 그대로 두고 — 보호모드(`BITS 32`) 블록은 한 글자도 안 바꿈 — real mode `print` 직후에 "A20 켜기 + 검증" 블록만 끼워 넣는다. 인사말도 `Hello world -- protected mode (32-bit)` → `...(32-bit), A20 enabled`로 한 단계 발전시켜, 보호모드 메시지가 떴다는 사실 자체가 A20 검증 통과의 증거가 되게 한다. 검증 코드는 최소화: 별도 함수 없이 인라인 wraparound 한 번으로 끝내고, 실패 시에만 real mode에서 에러를 찍고 멈춘다.

상위 컨텍스트(환경·툴체인·실습 진행 방식)는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```
03-a20/
├── boot/
│   └── boot.asm     # real mode → A20 검증/활성화 → GDT → PE → far jump → 32비트 VGA 출력
├── Makefile
└── build/           # 산출물 (생성됨)
```

## 핵심 개념

- **Fast A20(포트 `0x92`)로 활성화**: System Control Port A를 읽어 bit1을 세팅 후 다시 쓰는 3줄(`in`/`or al,2`/`out`). `or`라 bit0(fast reset)은 읽은 값 그대로 보존돼 리셋 위험 없음. 8042 키보드 컨트롤러 방식보다 훨씬 짧아 이 단계에선 이쪽을 씀.
- **인라인 wraparound 검증**: `0000:0500`(phys `0x000500`)과 `FFFF:0510`(phys `0x100500`)은 정확히 1MB 차이. `0000:0500`에 `0x1234`, `FFFF:0510`에 `0x4321`을 쓴 뒤 `0000:0500`을 다시 읽어 `0x4321`이면 두 주소가 같은 셀로 wrap된 것 = A20 비활성. 별도 함수·레지스터 백업 없이 한 블록으로 처리.
- **실패만 분기**: 검증 통과면 그대로 보호모드로 진입(= `A20 enabled` 메시지가 뜸). wrap이 감지되면 `a20_fail`로 가서 real mode에서 `int 0x10`으로 에러를 찍고 멈춤. QEMU는 보통 A20이 이미 켜진 채 부팅되므로 정상 경로로 흐른다.

## 명령

```bash
make            # build/boot.bin 생성
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

- `make run` 시 real mode 메시지가 잠깐 보이고, 보호모드 진입 후 좌상단에 `Hello world -- protected mode (32-bit), A20 enabled` 출력. (A20이 안 켜졌다면 보호모드로 못 가고 real mode에 `A20 enable failed ...`만 뜸)
- `build/boot.bin`이 정확히 512바이트.
  ```bash
  stat -c%s build/boot.bin   # 512
  ```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/boot.asm` | 수정 | real mode 블록에 Fast A20(포트 0x92) 활성화 + 1MB wraparound 검증 삽입 |

## 다음 단계 힌트

- `04-disk-load`: 부트섹터 512바이트로는 부족해짐. 디스크에서 추가 섹터를 읽어와(`int 0x13`, 재시도/CF 에러체크) 적재한 코드로 점프.
- `05-c-kernel`: 적재 영역을 C 커널로 교체 — `linker.ld` + asm→C 점프. GRUB(Multiboot) 위임은 `06-grub-multiboot`에서 따로 다룸.
- VGA 직접 쓰기 로직(`print_pm`)은 `07-vga-print`에서 스크롤 / 커서 / `printf`로 확장.
