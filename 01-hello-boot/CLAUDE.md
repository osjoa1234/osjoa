# 01 — Hello Boot

**목표**: 환경이 정상인지 검증하는 가장 작은 부트섹터.
BIOS가 디스크 첫 512바이트를 `0x7C00`에 적재해서 실행한다는 사실만 이용해, BIOS 인터럽트 `int 0x10`(teletype)으로 문자열을 출력한다.

상위 컨텍스트(환경·툴체인·실습 진행 방식)는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```
01-hello-boot/
├── boot/
│   └── boot.asm     # NASM, 16비트 real mode, ORG 0x7C00, 끝에 0xAA55
├── Makefile
└── build/           # 산출물 (생성됨)
```

## 작성/실행 흐름

1. `boot/boot.asm` 작성 (real mode, 출력 루프 + 부트 시그니처 `0xAA55`)
2. `Makefile`에 `nasm -f bin` 빌드 룰 + `qemu-system-i386 -drive format=raw,file=...` 실행 룰
3. `make run` → QEMU에서 "Hello, Custom OS!" 출력 확인

## 명령

```bash
make            # build/boot.bin 생성
make run        # QEMU GUI에서 실행
make run-nogui  # 콘솔 모드 (Ctrl+A 다음 X로 종료)
make clean
```

## 완료 기준

- `make run` 시 QEMU에 문자열이 정상 출력됨.
- 빌드 산출물 `build/boot.bin`이 정확히 512바이트.
  ```bash
  stat -c%s build/boot.bin   # 512가 나와야 함
  ```

## 이전 단계 대비 변경 파일

첫 번째 단계 — 비교 대상 없음.

## 다음 단계 힌트

- `02-protected-mode`로 넘어갈 때는 이 디렉토리를 복사한 뒤 GDT 정의 + `cr0` PE 비트 셋팅을 추가.
- 부트섹터 안에 다 들어가지 않으므로 곧 "2단계 부트로더"가 필요해짐 — 추가 섹터를 디스크에서 읽어오는 코드가 들어갈 자리.
