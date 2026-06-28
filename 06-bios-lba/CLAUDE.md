# 06 — BIOS LBA

**목표**: 05의 FAT16 로더는 유지하되, 실제 BIOS 디스크 읽기 경로를 **CHS(`AH=02`)에서 EDD/LBA(`AH=42`)로 교체**한다. 이번 단계의 핵심은 파일시스템 파싱이 아니라, "볼륨 안 섹터 번호"를 더 이상 CHS로 환산하지 않고 그대로 BIOS에 넘기는 구조를 익히는 것이다.

**05에서 이어짐**: 루트 디렉토리 검색, FAT 체인 추적, `KERNEL.BIN` 적재 대상 주소, payload의 A20/GDT/보호모드 진입은 그대로 둔다. 바뀌는 부분은 `read_sectors` 하나다. 05에서는 BPB의 `SecPerTrk`/`NumHeads`를 읽어 매 호출마다 CHS 레지스터를 조립했지만, 06에서는 Disk Address Packet(DAP)에 절대 LBA와 버퍼 주소를 채워 `int 13h` 확장 읽기만 호출한다.

인사말도 `...loaded from FAT16` → `...loaded from FAT16 via BIOS LBA`로 이어진다. 이 메시지가 뜨면 = FAT16 파싱은 그대로 유지된 채, 실제 섹터 읽기만 LBA 경로로 성공했다는 뜻이다.

상위 컨텍스트(환경·툴체인·실습 진행 방식)는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
06-bios-lba/
├── boot/
│   ├── boot.asm     # FAT16 VBR: BPB + 루트 디렉토리 스캔 + FAT 체인 적재 + BIOS LBA 읽기
│   └── kernel.asm   # payload(KERNEL.BIN): 16비트 진입 → A20 → GDT/PM → 32비트 VGA 출력
├── Makefile
└── build/           # 산출물 (생성됨)
```

## 도구

이 단계도 FAT 이미지 생성·검증 도구는 05와 같다:

```bash
make tools     # sudo apt install -y mtools dosfstools
```

- `dosfstools`(`mkfs.fat`): 디스크 이미지를 FAT16으로 포맷.
- `mtools`(`mcopy`): 호스트에서 FAT 이미지 안으로 `KERNEL.BIN` 파일 복사.

## 핵심 개념

- **superfloppy FAT16 + VBR**: 파티션 없이 디스크 전체가 FAT16 볼륨 하나이므로, 볼륨 시작 섹터와 디스크 시작 섹터가 같다. 즉 05에서 계산하던 `root_dir_sector`, `data_sector`, `cluster -> sector` 값이 이번 단계에서는 그대로 절대 LBA가 된다.

- **BPB는 여전히 런타임 값**: `BytsPerSec`/`SecPerClus`/`RsvdSecCnt`/`NumFATs`/`RootEntCnt`/`FATSz16`는 계속 실제 디스크의 BPB에서 읽는다. 다만 06에서는 CHS 환산이 사라졌기 때문에 `SecPerTrk`/`NumHeads` 값에 더 이상 의존하지 않는다.

- **DAP(Disk Address Packet)**: `AH=42`는 CHS 레지스터 대신 16바이트 DAP를 `DS:SI`로 받는다. 여기에는 읽을 섹터 수, 목적지 버퍼의 `segment:offset`, 그리고 64비트 시작 LBA가 들어간다. 이번 단계는 05의 호출 구조를 유지하려고 한 번에 1섹터씩 읽는다.

- **geometry 의존성 제거**: 05에서는 BIOS가 실제로 CHS를 읽으므로 이미지 생성 시 `mkfs.fat -g 16/63`처럼 QEMU와 맞는 geometry를 강제로 적어 주는 편이 안전했다. 06에서는 BIOS가 LBA를 직접 받기 때문에 이 강제가 사라지고, Makefile에서도 `-g` 옵션을 뺐다.

- **FAT 파서 자체는 그대로**: 루트 디렉토리 스캔, `KERNEL  BIN` 비교, `fat_next`의 on-demand FAT 조회, 클러스터 체인 적재 순서는 05와 동일하다. 즉 이번 단계는 "무엇을 읽나"가 아니라 "어떻게 읽나"만 바꾼 것이다.

- **payload가 모드 전환을 소유**: 부트섹터는 적재 후 real mode에서 `jmp 0x0000:0x7E00`로 넘긴다. `kernel.asm`은 16비트로 시작해 A20 활성/검증 → GDT 로드 → `cr0.PE` → `jmp 0x08:protected_start`로 32비트 진입한 뒤 VGA에 문자열을 쓴다.

## 명령

```bash
make tools      # (최초 1회) mtools, dosfstools 설치
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

- `make run` 시 payload가 좌상단에 `Hello world -- protected mode (32-bit), loaded from FAT16 via BIOS LBA`를 출력한다.
- `build/boot.bin`이 정확히 512바이트다.

  ```bash
  stat -c%s build/boot.bin
  ```

- `mkfs.fat -g ...` 없이 만든 이미지에서도 `KERNEL.BIN` 적재가 정상 동작한다.

## 다음 단계 힌트

- `07-c-kernel`: payload(`KERNEL.BIN`)를 asm에서 C 커널로 교체한다. 이번 단계의 BIOS LBA 읽기 경로는 그대로 유지되고, 적재 대상만 freestanding 32비트 C 바이너리로 바뀐다.
- `08-grub-multiboot`: GRUB가 커널 적재를 대신 맡도록 바꿔 부트로더 구조를 단순화한다.
- `09-vga-print`: VGA 텍스트 모드 출력을 문자열 루틴과 커서/스크롤로 확장한다.
