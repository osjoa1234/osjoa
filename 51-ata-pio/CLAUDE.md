# 51 — ata-pio

**목표**: ATA PIO(Programmed I/O)로 IDE 하드디스크의 LBA 섹터를 read/write한다. FS 파싱은 전혀 하지 않고, 드라이버 자체(포트 I/O, 명령 시퀀스, 폴링)만 raw sector dump로 검증한다.

**50에서 이어짐**: 지금까지 이 커널이 다루는 저장소는 GRUB이 물리 메모리에 올려준 `initramfs.cpio`(`initrd.c`) 하나뿐이었다 — 부팅 시 통째로 RAM에 있고, `initrd_open`은 그 안에서 이름을 선형 탐색할 뿐 디스크 I/O가 전혀 없다. `51`은 처음으로 **진짜 블록 디바이스**(포트 I/O로 매 섹터를 요청하고 폴링으로 완료를 기다려야 하는 하드웨어)를 붙이는 자리다. `52-ext2-probe`가 이 위에 슈퍼블록/블록 그룹 파싱을 얹고, `53-vfs-ext2-read`가 기존 `vfs_ops_t`에 연결한다 — 이번 단계는 그 세 단계 중 첫 번째로, "섹터 하나를 정확히 읽고 쓸 수 있는가"만 확정한다.

## 새 디바이스: QEMU 프라이머리 마스터 IDE 드라이브

지금까지 `run`/`run-nogui`는 `-cdrom $(ISO)` 하나였다. `-cdrom`은 QEMU 기본 머신(`piix3-ide`)에서 **세컨더리 마스터**(`ide1`, GRUB이 여기서 커널을 부팅)로 붙는다. 이번 단계부터 `-drive file=build/disk.img,format=raw,if=ide,index=0`을 추가로 붙이는데, 이건 **프라이머리 마스터**(`ide0`, 포트 `0x1F0`~`0x1F7`)로 잡힌다 — 우리 드라이버가 말 거는 대상이 GRUB이 부팅에 쓰는 디바이스와 물리적으로 분리되어 있다는 뜻이다. `disk.img`는 `dd if=/dev/zero bs=1M count=8`로 만드는 8MB 빈 이미지이고(파티션 테이블도, 파일시스템도 없는 완전한 raw 블록), `Makefile`의 `$(DISKIMG)`가 `build/` 밑에서 한 번만 생성되고 이후 실행에서는 재사용된다 — 즉 `make run`을 여러 번 돌려도 이전 실행에서 쓴 섹터가 그대로 남는, 진짜 디스크처럼 동작한다.

## ATA PIO 레지스터 맵 (프라이머리 버스)

| 포트 | 이름 | 방향 | 용도 |
|------|------|------|------|
| 0x1F0 | Data | R/W (16비트) | 섹터 데이터 256워드 전송 |
| 0x1F2 | Sector Count | W | 전송할 섹터 수(여기선 항상 1) |
| 0x1F3 | LBA lo | W | LBA[7:0] |
| 0x1F4 | LBA mid | W | LBA[15:8] |
| 0x1F5 | LBA hi | W | LBA[23:16] |
| 0x1F6 | Drive/Head | W | bit7=1,bit6=1(LBA모드),bit5=1(예약),bit4=드라이브(0=마스터),bit3:0=LBA[27:24] |
| 0x1F7 | Status(R)/Command(W) | R/W | 상태 폴링 또는 명령 발행 |
| 0x3F6 | Alternate Status/Control | R/W | 상태 재확인(인터럽트 유발 없음), 400ns 지연용 |

Status 레지스터(0x1F7 읽기) 비트:

| 비트 | 이름 | 의미 |
|------|------|------|
| 7 | BSY | 드라이브가 명령 처리 중 — 이 비트가 꺼질 때까지 다른 레지스터를 건드리면 안 됨 |
| 6 | DRDY | 드라이브 준비됨 |
| 5 | DF | 드라이브 폴트 |
| 3 | DRQ | 데이터 전송 준비됨 — read면 지금 Data 포트에서 읽어갈 수 있음, write면 지금 Data 포트에 써넣어야 함 |
| 0 | ERR | 이전 명령 에러 |

28비트 LBA만 쓴다(2^28섹터 × 512바이트 = 128GB까지 CHS 변환 없이 접근) — `disk.img`가 8MB라 이 한계는 관계없고, `06-bios-lba`에서 부트로더가 BIOS `int 0x13` 확장으로 익힌 "CHS 대신 LBA로 섹터를 말한다"는 감각을 커널 쪽 실제 하드웨어 명령어 레벨에서 다시 확인하는 셈이다.

## 폴링만 쓴다 (IRQ14 없음)

ATA는 명령 완료를 IRQ14(프라이머리)/IRQ15(세컨더리)로도 알릴 수 있지만, 이번 단계는 `ata_read_sector`/`ata_write_sector`가 각각 자신을 호출한 코드(지금은 `kernel_main`)를 **동기적으로 블록**하는 방식만 구현한다. `ata_wait_ready`(BSY=0 대기)와 `ata_wait_drq`(DRQ=1 또는 ERR 대기)가 각각 최대 100만 회 스핀하고 못 넘기면 -1을 리턴한다 — 실패를 무한 대기 대신 명시적 에러로 관측 가능하게 하려는 상한선일 뿐, 실측된 사이클 수 기반 타이밍은 아니다. `10-interrupts`에서 PIC를 리맵하고 `interrupts_unmask_irq`로 IRQ를 개별 활성화하는 인프라는 이미 있지만, IRQ14 핸들러를 새로 등록하는 일은 이번 스코프 밖이다 — 지금은 커널 부팅 시퀀스 안에서 한 번 read/write를 검증하는 게 목적이라 동기 폴링으로 충분하고, 나중에 파일시스템이 스케줄러 위에서 비동기로 디스크를 기다려야 할 때 다시 볼 만한 지점이다.

`ata_select_lba`가 드라이브/LBA 레지스터를 채운 직후 `ata_io_wait`(alternate status를 4번 읽어 약 400ns 태움)를 호출한다 — 드라이브가 방금 선택된 직후 즉시 status를 읽으면 이전 드라이브의 상태가 남아있을 수 있다는 OSDev 관례를 따른 것이다.

## 검증: FS 없이 raw sector dump

`kernel_main`이 `kheap_init` 직후(초기 부트 스택 위에서, 512바이트 정적 버퍼 두 개로) 다음을 수행한다:

1. `ata_read_sector(0, ...)`로 LBA 0을 읽어 첫 16바이트를 hex로 찍는다 — `disk.img`가 `dd if=/dev/zero`로 만들어졌으므로 항상 전부 `00`이다.
2. 고정 패턴 문자열을 512바이트에 반복해 채운 뒤 `ata_write_sector(5, ...)`로 LBA 5에 쓰고, 곧바로 `ata_read_sector(5, ...)`로 다시 읽어 원본과 바이트 단위로 비교해 `OK`/`FAIL`을 찍는다.

LBA 0이 아니라 5에 쓰는 이유는 단순히 "쓰기 대상과 읽기 확인 대상을 분리해 두 경로를 각각 보여주기" 위함이고, 이 이미지에 부트 섹터나 파티션 테이블 같은 의미가 있는 건 아니다(`52`부터 ext2 슈퍼블록이 바이트 오프셋 1024, 즉 LBA 2에 놓인다 — FAT16 BPB처럼 LBA 0이 아니다).

## 명령

```bash
make            # build/os.iso, build/disk.img(8MB, 최초 1회 생성 후 재사용) 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean      # build/ 전체 삭제 (disk.img도 함께 삭제됨 — 다음 make에서 새로 생성)
```

## 완료 기준

`make run-nogui`에서 `initramfs` 로그 이전에 다음이 보이면 성공이다(이후 `$` 프롬프트까지는 50과 동일):

```
heap: kernel dir adopted, window at 0xFFFFFFFFC0400000 (mapped=128MB)
ata: primary master ready (0x1F0-0x1F7, ctrl=0x3F6)
ata: read lba=0 first16=00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
ata: write/read lba=5 verify=OK
initramfs: 13 file(s) found
```

## 이전 단계(50) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/ata.h` | 신규 | `ata_init`/`ata_read_sector`/`ata_write_sector` 선언, `ATA_SECTOR_SIZE`(512) |
| `boot/ata.c` | 신규 | 프라이머리 버스 포트 I/O(`inb`/`outb`/`inw`/`outw` 로컬 헬퍼), LBA 선택, BSY/DRQ 폴링, READ SECTORS(0x20)/WRITE SECTORS(0x30)/CACHE FLUSH(0xE7) 명령 시퀀스 |
| `boot/kernel.c` | 수정 | `ata.h` include; `kheap_init` 직후 `ata_init()` + LBA 0 read dump + LBA 5 write/read 검증 블록 추가 |
| `Makefile` | 수정 | `ATAOBJ` 컴파일·링크 반영; `DISKIMG`(`build/disk.img`, `dd`로 8MB 생성) 타깃 추가; `run`/`run-nogui`에 `-drive file=...,format=raw,if=ide,index=0` 추가 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `52-ext2-probe`가 지금 확정한 `ata_read_sector`/`ata_write_sector` 위에 ext2 슈퍼블록/블록 그룹 디스크립터 파싱과 루트 디렉토리 나열을 얹는다 — 이번 단계에서 임의로 LBA 5에 써둔 테스트 패턴은 `disk.img`가 다음 단계에서 새로 만들어질(mkfs.ext2 등으로 포맷될) 것이므로 남겨둘 이유가 없다.
- IRQ14 기반 비동기 완료 통지는 여전히 미구현이다 — 지금은 동기 폴링으로 충분하지만, 디스크 I/O가 스케줄러가 도는 유저 프로세스 경로(`vfs_ops_t.read`)에 들어가는 `53`부터는 매 섹터마다 커널 전체가 블로킹된다는 점을 염두에 둘 만하다.
- 8비트 섹터 카운트(`0x1F2`에 항상 `1`)만 쓰고 있어 한 번에 여러 섹터를 전송하는 경로는 없다 — 멀티섹터 전송이 필요해지면(파일 하나가 여러 클러스터에 걸칠 때 성능이 문제되면) 그때 `ata_read_sector`를 확장할 지점이다.
