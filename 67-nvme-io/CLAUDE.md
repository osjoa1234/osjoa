# 67 — nvme-io

**목표**: `66-nvme-admin`이 admin 큐로 Identify까지만 하던 컨트롤러에 **I/O SQ/CQ 한 쌍(QID 1)** 을 만들고, NVM 커맨드 Read/Write로 실제 섹터를 읽고 쓴다. 그리고 `51-ata-pio`가 ext2에 제공하던 `ata_read_sector`/`ata_write_sector` 자리를 같은 모양의 `nvme_read_sector`/`nvme_write_sector`로 대체해, `52`~`62`의 ext2/VFS/셸 스택이 NVMe 경유로 동작하는지 재검증한다. 완료 통지는 여전히 CQ phase bit polling이다(MSI-X는 `68-msi-x`).

## 0) 큐를 구조체로 묶고 `nvme_submit`으로 일반화

66의 `nvme_identify`는 admin 큐 상태(`sq_tail`/`cq_head`/`phase`)를 인자 여섯 개로 넘겼다. I/O 큐가 생기면 같은 제출/폴링 로직을 두 큐가 공유해야 해서 `struct nvme_queue`(sq/cq 포인터, qid, entries, tail/head/phase)로 묶고, 커맨드 조립 + doorbell + phase 폴링을 `nvme_submit(queue, opcode, nsid, prp1, prp2, cdw10~12)` 하나로 뽑았다. doorbell 인덱스는 SQ tail `2*qid`, CQ head `2*qid+1`이다(stride 4 기준 admin `0x1000/0x1004`, I/O `0x1008/0x100C` — 66에서 매핑해둔 두 번째 페이지 안에 그대로 들어온다). `nvme_identify`는 이 위의 얇은 래퍼가 됐다.

- **CID 검증 추가**: 제출마다 CID를 증가시켜 넣고, 완료 엔트리 dword3 하위 16비트(CID)가 같은지 확인한다(phase는 dword3 bit16, status는 상위 15비트). 처음에 CID를 dword2 상위로 착각해서 모든 커맨드가 -1로 실패했다 — CQE는 dword2 = SQ Head Pointer(15:0)+SQ ID(31:16), dword3 = CID(15:0)+Phase(16)+Status(31:17)다.
- doorbell 쓰기 직전에 컴파일러 배리어(`asm volatile("" ::: "memory")`)를 하나 넣어, SQE 필드 쓰기가 doorbell 쓰기 뒤로 재배치되지 않게 했다. SQE 필드 쓰기는 일반 메모리 쓰기이고 doorbell과 값 의존성이 없어서, 컴파일러가 순서를 바꿔도 규칙 위반이 아니기 때문이다. 순서가 바뀌면 컨트롤러가 벨 직후 덜 채워진 슬롯을 읽는다. 완료 폴링 쪽에는 배리어를 두지 않았다 — 폴링 대상인 `cqe->status`가 `volatile`이라 루프 안 읽기와 그 뒤 `status` 읽기의 순서는 이미 보장되고, 데이터 버퍼를 읽는 코드는 `nvme_submit` 바깥(호출한 쪽)에 있어 이 루프 앞으로 올라올 일이 없다. x86은 CPU가 쓰기/읽기 순서를 하드웨어로 유지해서 컴파일러 배리어만으로 충분하다. 완료 폴링 뒤에 읽기 배리어가 필요해지는 경우(`nvme_submit` 인라인 방식 변경, 약한 메모리 모델 CPU로 이식 — 리눅스는 이 자리에 `dma_rmb()`를 둔다)는 그때 추가한다.

## 1) I/O 큐 생성 (admin 커맨드)

`nvme_create_io_queues()`: CQ를 **먼저** 만들어야 한다(SQ 생성 커맨드가 연결할 CQID를 요구한다).

- Create I/O CQ (opcode `0x05`): CDW10 = `(entries-1)<<16 | QID`, CDW11 = PC(bit0)=1, IEN(bit1)=0(인터럽트 없음), PRP1 = CQ 물리주소.
- Create I/O SQ (opcode `0x01`): CDW10 동일, CDW11 = PC=1 | `CQID<<16`, PRP1 = SQ 물리주소.

큐는 각각 `page_alloc()` 한 페이지(64엔트리: SQ 64×64B=4096, CQ 64×16B=1024)다. 이 큐들의 CC.IOSQES/IOCQES(6/4)는 66에서 이미 넣어둔 값이 그대로 유효하다.

## 2) Read/Write와 PRP

I/O 큐 opcode는 admin과 별개 번호공간이다: Write `0x01`, Read `0x02`. CDW10/11 = 시작 LBA 하/상위 32비트, CDW12[15:0] = 섹터 수 − 1.

**섹터 크기는 Identify Namespace에서 읽는다**: FLBAS[3:0]이 가리키는 LBAF 엔트리(offset `128 + 4*idx`)의 LBADS(bit 23:16) → `1 << LBADS`. QEMU는 512이고, 이 단계의 ext2 연결은 512를 전제하므로 다르면 I/O 큐 생성 전에 중단한다. NSZE도 저장해 `nvme_transfer`에서 LBA 범위를 검증한다.

`nvme_transfer(opcode, lba, sectors, pages[])`가 PRP 규칙을 구현한다(`pages[]`는 4KB 페이지 물리주소 배열):

| 전송 크기 | PRP1 | PRP2 |
|---|---|---|
| 1페이지 | `pages[0]` | 0 |
| 2페이지 | `pages[0]` | `pages[1]` |
| 3페이지 이상 | `pages[0]` | PRP 리스트 페이지 물리주소 (`pages[1..]` 8바이트 엔트리 배열) |

PRP 리스트 페이지는 init에서 한 번 할당해 재사용한다(최대 512엔트리). 데이터 페이지들은 물리적으로 연속일 필요가 없다는 점이 PRP 리스트의 핵심이라, 자가 테스트도 `page_alloc()`을 페이지별로 호출한 비연속 페이지로 돌린다.

## 3) `nvme_read_sector`/`nvme_write_sector` (ata와 같은 모양)

`int nvme_read_sector(u32 lba, u8 *buf)` / `int nvme_write_sector(u32 lba, const u8 *buf)` — 성공 0, 실패 -1. ext2가 넘기는 `buf`는 커널 스택/정적/힙 어디든 될 수 있어 물리주소를 바로 알 수 없으므로, 드라이버가 소유한 **bounce 페이지 하나**(init에서 할당)를 PRP1로 쓰고 512바이트를 복사한다. ATA PIO가 `insw`/`outsw`로 한 섹터씩 옮기던 것과 같은 단위다. 동시 접근은 `61-io-lock`의 ext2 락이 직렬화한다(bounce 페이지·I/O 큐 상태는 락 없이 단일 소비자 전제).

## 4) ext2를 ATA에서 NVMe로 교체

- `boot/ext2.c`: `ata_read_sector`/`ata_write_sector` → `nvme_*`, `ATA_SECTOR_SIZE` → `NVME_SECTOR_SIZE`(`nvme.h`, 512), `#include "ata.h"` → `"nvme.h"`.
- `boot/ata.c`, `boot/ata.h` **삭제**, `kernel.c`에서 `ata_init()` 호출 제거, Makefile에서 `ATAOBJ` 제거. ext2의 유일한 블록 디바이스가 NVMe가 됐으므로 죽은 코드를 남기지 않았다.
- `nvme_init()`(I/O 큐까지 완성)은 `kheap_init()` 앞, `ext2_init()` 앞이라 ext2가 초기화될 때 이미 `g_io_ready`다. `page_alloc()`만 쓰므로 kheap이 필요 없다.

## 5) Makefile / QEMU / 디스크 이미지

- 66의 `-device piix3-ide` + `ide-hd`(disk.img) 와 별도 빈 `nvme.img`를 **하나로 합쳤다**: `build/disk.img`를 `-device nvme`의 백엔드로 붙이고 `nvme.img` 타겟/변수는 삭제.
- 자가 테스트가 쓸 영역을 ext2와 분리하려고 `disk.img`를 **18MB**로 만들고 `mkfs.ext2 ... 16384`로 파일시스템 블록 수를 16MB(1K 블록 16384개)에 고정했다. 뒤 2MB(LBA 32768~)가 ext2 바깥 테스트 영역이다.

## 검증

`make clean && make run-nogui`(관련 구간):

```
nvme: found controller at 00:03.0
nvme: admin queue ready (64 entries)
nvme: identify controller model="QEMU NVMe Ctrl" serial="deadbeef" fw="8.2.2"
nvme: identify namespace nsid=1 nsze=36864 blocks lba_size=512
nvme: io queue ready (qid=1, 64 entries)
nvme: io prp1 write/read lba=32768 sectors=8 ok
nvme: io prp1+prp2 write/read lba=32768 sectors=16 ok
nvme: io prp-list write/read lba=32768 sectors=40 ok
nvme: nvme_write_sector/nvme_read_sector lba=32769 ok
ext2: superblock magic=0xEF53 rev=1 block_size=1024 blocks=16384 inodes=4096
ext2: group 0: inode_table=5 block_bitmap=3 inode_bitmap=4 free_blocks=6482 free_inodes=2020
vfs: ext2 mounted at /disk/
shell: ext2 /disk/hello.txt: hello ext2 root fs
```

세 PRP 경로(1페이지/2페이지/PRP 리스트 5페이지)에서 패턴을 쓰고 읽어 바이트 단위로 비교하고, 섹터 API 왕복도 확인한다. ext2 마운트/읽기/셸 흐름이 ATA 없이 NVMe만으로 살아있다. (PCI 슬롯이 66의 `00:04.0`에서 `00:03.0`으로 바뀐 건 `piix3-ide` 디스크 제거에 따른 QEMU 자동 배치 결과다.)

`e2fsck -f -n build/disk.img` exit 0, `grub-file --is-x86-multiboot2 build/kernel.elf` exit 0.

## 이전 단계(66) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/nvme.c`, `boot/nvme.h` | 수정 | `struct nvme_queue`/`nvme_submit`(CID 검증 포함)로 admin/I/O 큐 공용화, Create I/O CQ/SQ, LBADS 파싱, PRP1/PRP2/PRP 리스트 `nvme_transfer`, bounce 기반 `nvme_read_sector`/`nvme_write_sector`, 부팅 시 자가 테스트 |
| `boot/ext2.c` | 수정 | 블록 I/O를 `ata_*`에서 `nvme_*`로 교체 |
| `boot/kernel.c` | 수정 | `ata.h` include와 `ata_init()` 호출 제거 |
| `boot/ata.c`, `boot/ata.h` | 삭제 | ext2의 블록 디바이스가 NVMe로 대체됨 |
| `Makefile` | 수정 | ATA 오브젝트·`piix3-ide`·`nvme.img` 제거, `disk.img`(18MB, ext2 16384블록)를 NVMe 백엔드로 연결, 의존성 갱신 |
| 나머지 전부 | 변경 없음 | 66의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 위 "검증" 블록의 `nvme:` 줄 전부와 `ext2:`/`vfs:`/`shell:` 흐름이 나와야 하고, `e2fsck -f -n build/disk.img`와 `grub-file --is-x86-multiboot2 build/kernel.elf`가 exit 0이어야 한다.

## 다음 단계 힌트

- **쓰기 경로의 ext2 연동은 부팅 로그에 안 드러난다**: 부팅 시 셸은 읽기만 한다. `nvme_write_sector`는 raw 자가 테스트로 검증했고 `55`~`60`의 ext2 쓰기/`>` 리다이렉션은 구조상 같은 함수를 지나지만, 대화형 셸에서 실제 쓰기를 돌려 확인한 건 아니다 — `68-msi-x`로 완료 경로를 바꾼 뒤 같이 재검증한다.
- **완료 통지는 polling이다**: `68-msi-x`에서 같은 컨트롤러·큐를 재사용해 CDW11의 IEN/IV와 MSI-X 테이블 프로그래밍으로 바꾼다.
- **섹터 크기 512 고정 전제**: LBADS가 다르면 init을 중단한다. 4KB 섹터 장치는 `ext2`의 `NVME_SECTOR_SIZE` 상수를 런타임 값으로 바꿔야 한다.
- **한 커맨드 in-flight, 큐 깊이 64 미사용**: 제출 즉시 완료를 기다리므로 큐가 64개여도 1개만 쓴다. 비동기/다중 outstanding은 인터럽트 도입 뒤에 의미가 있다.
- **MDTS(최대 전송 크기) 미확인**: 현재 최대 전송은 자가 테스트의 5페이지뿐이라 문제없지만, 큰 전송을 쓰게 되면 Identify Controller의 MDTS로 상한을 걸어야 한다.
