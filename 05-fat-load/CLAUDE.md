# 05 — FAT Load

**목표**: 04의 "고정 섹터(2~9)를 통째로 읽기"를 **파일시스템 인식 적재**로 바꾼다. 디스크를 FAT16 볼륨으로 포맷하고, 부트섹터가 BPB를 읽어 루트 디렉토리에서 `KERNEL.BIN`을 찾아 클러스터 체인을 따라 적재한 뒤 그 코드로 점프한다. "어떤 코드가 실행되나(payload)"는 그대로 두고, "부트로더가 그걸 디스크에서 **어떻게 찾아 적재하나**"만 바꾸는 단계.

**04에서 이어짐**: 04는 부트섹터(sector 1)가 raw 섹터 읽기 + A20 + GDT/PM 전환까지 다 했다. 05에서는 적재를 FAT로 바꾸면서 역할을 나눈다 — **부트섹터는 "FAT 로더"만 담당**하고, **A20 활성/검증 + GDT + 보호모드 전환은 적재 대상(`KERNEL.BIN`)으로 옮긴다.** 한 섹터(510바이트)에 FAT 파싱과 모드 전환을 동시에 넣을 수 없기 때문이고, "부트섹터=로더 / 적재된 이미지=자기 환경을 스스로 세팅"이라는 구조가 더 자연스럽다(06에서 payload를 C 커널로 바꿀 때 이 진입 asm이 그대로 재사용된다).

인사말도 `...loaded from disk` → `...loaded from FAT16`로 한 단계 발전. 이 PM 메시지가 뜨면 = BPB 파싱 + 루트 디렉토리 검색 + 클러스터 체인 적재가 모두 성공했다는 증거.

상위 컨텍스트(환경·툴체인·실습 진행 방식)는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```
05-fat-load/
├── boot/
│   ├── boot.asm     # FAT16 VBR(부트섹터): BPB + 루트 디렉토리 스캔 + FAT 체인 적재 → 0x7E00로 점프
│   └── kernel.asm   # payload(KERNEL.BIN): 16비트 진입 → A20 → GDT/PM → 32비트 VGA 출력
├── Makefile
└── build/           # 산출물 (생성됨)
```

## 도구

이 단계는 FAT 이미지 생성·검증에 추가 도구가 필요하다(미설치):

```bash
make tools     # sudo apt install -y mtools dosfstools
```

- `dosfstools`(`mkfs.fat`): 디스크 이미지를 FAT16으로 포맷.
- `mtools`(`mcopy`): 호스트에서 FAT 이미지 안으로 `KERNEL.BIN` 파일 복사(마운트 없이).

## 핵심 개념

- **superfloppy FAT16 + VBR**: 파티션 테이블(MBR) 없이 디스크 전체가 하나의 FAT16 볼륨. 따라서 부트섹터 자체가 볼륨 부트 레코드(VBR)이고, 맨 앞에 **BPB**(BIOS Parameter Block)를 품는다. 오프셋 0은 `jmp short start` + `nop`(3바이트)로 BPB를 건너뛰어 코드(오프셋 `0x3E`)로 점프.

- **BPB는 런타임에 디스크에서 읽는다**: boot.asm 안의 BPB 값들은 어셈블용 placeholder다. 실제 값은 `mkfs.fat`가 디스크에 써 넣은 것을 쓴다. 부트섹터가 `0x7C00`에 올라오므로 `[bpb_*]` 레이블(= `0x7C00`+오프셋)을 읽으면 디스크의 실제 BPB가 읽힌다. 그래서 `BytsPerSec`/`SecPerClus`/`RsvdSecCnt`/`NumFATs`/`RootEntCnt`/`FATSz16`뿐 아니라 `SecPerTrk`/`NumHeads`도 코드에 박지 않고 모두 동적으로 읽는다. 05에서는 이 BPB 값으로 FAT 구조를 해석하고, 실제 BIOS 읽기는 끝까지 CHS(`AH=02`)로 유지한다.

- **BPB 보존 오버레이(Makefile의 dd)**: `mkfs.fat`가 자기 부트섹터(+유효한 BPB)를 써 놓은 이미지에, 내 부트 코드를 **BPB를 덮지 않고** 얹는다. `dd ... count=11`(오프셋 0~10: `jmp`+OEM)와 `dd ... skip=62 seek=62 count=450`(오프셋 `0x3E`~511: 코드+서명) 두 번으로 쓰고, 그 사이 `[0x0B..0x3E)`(BPB)는 mkfs 값을 그대로 둔다. 내 코드가 BPB를 동적으로 읽으므로 이 조합이 항상 일관된다.

- **FAT 레이아웃 산수(볼륨 안의 섹터 순번)**:
  - `root_dir_sector = RsvdSecCnt + NumFATs * FATSz16`
  - `root_dir_sectors = ceil(RootEntCnt * 32 / BytsPerSec)`
  - `data_sector = root_dir_sector + root_dir_sectors`
  - `cluster N → sector = data_sector + (N - 2) * SecPerClus` (데이터 영역은 클러스터 2부터)

- **루트 디렉토리 스캔(8.3 이름)**: 루트 디렉토리를 한 섹터씩 읽어 32바이트 엔트리들을 훑는다. 파일명은 11바이트 8.3 형식(이름 8 + 확장자 3, 공백 패딩) → `KERNEL.BIN`은 `"KERNEL  BIN"`. 첫 바이트 `0x00`이면 디렉토리 끝(더 없음). 일치하면 오프셋 26의 시작 클러스터(`DIR_FstClusLO`)를 얻는다.

- **FAT 체인(on-demand 조회)**: 전체 FAT를 메모리에 올리지 않고, 필요한 클러스터의 FAT 항목이 든 섹터만 그때그때 읽는다. `fat_next`: `cluster*2`를 `BytsPerSec`로 나눠 FAT 섹터/항목 오프셋을 구하고(`shl`+`rcl`로 32비트화 → `div`), 그 섹터를 읽어 16비트 다음 클러스터를 반환. `>= 0xFFF8`이면 체인 끝(EOC).

- **`int 13h` CHS 읽기(AH=02)**: 04의 BIOS 디스크 I/O 흐름을 그대로 잇기 위해 확장 읽기(AH=42)는 쓰지 않는다. 루트 디렉토리/FAT/데이터 위치를 계산한 뒤, BIOS 호출 직전마다 BPB의 `SecPerTrk`/`NumHeads`를 써서 현재 읽을 섹터를 CHS 레지스터(`CH`/`CL`/`DH`)로 맞춘다. 구현은 한 번에 1섹터씩 읽어 04와 같은 호출 형태를 유지하고, 실패 시에도 04처럼 AH=00 리셋 후 `[retries]` 카운트로 재시도한다.

- **payload가 모드 전환을 소유**: 부트섹터는 적재 후 real mode에서 `jmp 0x0000:0x7E00`로 넘긴다. `kernel.asm`은 16비트로 시작해 A20 활성/검증(03의 1MB wraparound 테스트) → GDT 로드 → `cr0.PE` → `jmp 0x08:protected_start`로 32비트 진입한 뒤 VGA에 출력한다. GDT도 payload 안에 있다.

## 명령

```bash
make tools      # (최초 1회) mtools, dosfstools 설치
make            # build/os.img 생성 (16MB FAT16, KERNEL.BIN 포함, 부트섹터 오버레이)
make run        # QEMU GUI에서 실행
make run-nogui  # 콘솔 모드 (Ctrl+A 다음 X로 종료)
make clean
```

이미지 안에 파일이 들어갔는지 확인:
```bash
mdir -i build/os.img ::      # KERNEL.BIN 이 보여야 함
```

VGA 출력은 시리얼로 안 나오므로, GUI 없이 확인하려면 QEMU 모니터로 텍스트 버퍼를 직접 읽는다(게스트 부팅 시간 확보용 `sleep`):
```bash
(sleep 2; printf 'xp /160bx 0xb8000\n'; printf 'quit\n') \
  | qemu-system-i386 -drive format=raw,file=build/os.img -display none -monitor stdio -serial none
# 0xb8000부터 [문자][속성 0x0f] 교차 → 48 'H', 65 'e', 6c 'l' ... = "Hello "
```

## 완료 기준

- `make run` 시 real mode 메시지가 잠깐 보이고, FAT16에서 적재된 payload가 좌상단에 `Hello world -- protected mode (32-bit), loaded from FAT16` 출력. (파일을 못 찾으면 real mode에 `KERNEL.BIN load failed`만 뜸)
- `build/boot.bin`이 정확히 512바이트(BPB 포함 1섹터).
  ```bash
  stat -c%s build/boot.bin   # 512
  ```
- 이미지 안에 `KERNEL.BIN`이 존재(`mdir -i build/os.img ::`).

## 다음 단계 힌트

- `06-c-kernel`: payload(`KERNEL.BIN`)를 asm에서 **C 커널**로 교체 — `linker.ld` 레이아웃 + asm 진입 stub이 A20/GDT/PM 세팅 후 `call kmain`. 부트섹터의 FAT 로더는 그대로 재사용된다(파일 이름만 같으면 내용물이 C든 asm이든 무관). 커널이 1클러스터를 넘으면 FAT 체인이 자동으로 여러 클러스터를 따라간다.
- GRUB(Multiboot) 위임은 `07-grub-multiboot`에서: GRUB가 이 단계의 "FAT에서 커널 파일을 찾아 적재"를 대신 해준다.
- VGA 직접 쓰기 로직은 `08-vga-print`에서 `print`/스크롤/커서로 확장.
