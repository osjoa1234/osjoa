# 69 — msi-x-nvme-io

**목표**: `68-msi-x`에서 admin CQ만 MSI-X로 바꿨던 것을 I/O CQ(QID 1)까지 확장한다. Create I/O CQ의 CDW11에서 인터럽트를 켜고 IV 1(MSI-X 테이블 엔트리 1번)을 지정하며, 큐 구조체의 폴링 경로(`use_irq` 분기)를 완전히 없앤다. 그 결과 `52`~`62`의 ext2/VFS/셸 스택이 전부 인터럽트 완료 경로로 동작한다. 새 디바이스 드라이버는 추가하지 않는다.

## 0) I/O CQ 인터럽트 활성화 (`boot/nvme.c`)

Create I/O Completion Queue(opcode `0x05`)의 CDW11은 다음 필드로 이루어진다.

- 비트 0 (PC): physically contiguous. 68까지는 값 `1U` 하나로 이걸 켜고 있었다.
- 비트 1 (IEN): interrupt enable. 이걸 켜야 이 CQ에 완료가 쓰일 때 장치가 MSI-X 메시지를 쏜다. 꺼져 있으면 CQE는 써지지만 인터럽트는 오지 않는다.
- 비트 31:16 (IV): interrupt vector. MSI-X 테이블의 몇 번째 엔트리로 메시지를 보낼지의 인덱스.

`NVME_CQ_PHYS_CONTIG | NVME_CQ_IRQ_ENABLE | (NVME_IO_MSIX_ENTRY << 16)`으로 채운다. admin CQ는 Create 커맨드가 없고 IV 0으로 고정이므로 엔트리 0, I/O CQ는 엔트리 1이다. 엔트리 1은 vector `0x31`을 쓴다(`0x30`은 admin).

`nvme_init`은 MSI-X 준비 단계에서 엔트리 1도 `pci_msix_set_entry`로 채우고 `interrupts_register_handler(0x31, nvme_io_irq)`를 등록한 뒤 `pci_msix_start`로 function mask를 끈다. 테이블은 `table_size=65`라 엔트리 1이 이미 존재한다.

## 1) 핸들러 공용화와 폴링 제거

- `nvme_irq(q)`: 68의 `nvme_admin_irq` 본문을 큐 인자를 받는 형태로 일반화했다. phase 확인 → `nvme_reap` → `q->result` 저장 → `q->irq_count++` → `q->done = 1`. `interrupts_register_handler`가 받는 핸들러는 인자 없는 `void(void)`라서 `nvme_admin_irq`/`nvme_io_irq` 얇은 래퍼 두 개가 각각 `g_admin`/`g_io`를 넘긴다(리눅스의 `request_irq(..., dev_id)`에 해당하는 컨텍스트 인자는 아직 없음).
- `struct nvme_queue`에서 `use_irq`를 지우고, 전역 `g_admin_irq_count` 대신 큐별 `irq_count`를 둔다. `nvme_submit`의 phase 폴링 분기는 삭제했다 — admin/I/O 어느 큐든 doorbell 뒤에는 항상 `nvme_wait_irq`다.
- `nvme_report()`(신규, `boot/nvme.h`에 선언): 커널이 `ext2_init()` 직후 부르면 `io completions via msix: N interrupt(s)`를 출력한다. I/O CQ가 정말 인터럽트로 완료됐는지 보는 카운터다.

## 2) `nvme_wait_irq` 정리

68의 `nvme_wait_irq`는 `sti` 후 스핀 상한만큼 도는 우회였다. I/O 경로는 `interrupts_enable()` 이후(IF=1)에도 호출되고 `nvme_init`/`ext2_init` 시점에는 IF=0으로 호출되므로 두 경우를 한 구현으로 처리해야 한다.

```
flags = RFLAGS
loop:
    cli
    if done: break
    sti; hlt
if flags.IF: sti
```

- `cli` 후에 `done`을 확인하고, 아직이면 `sti; hlt`로 잔다. `sti`는 바로 다음 명령(`hlt`)이 끝날 때까지 인터럽트를 미루므로 "확인한 뒤 `hlt` 사이에 인터럽트가 끼어 깨우기를 놓치는" 경합이 없다. IF=1로 호출돼도 같은 이유로 `cli`로 시작한다.
- 함수가 끝나면 호출 시점의 IF를 복원한다(IF=0이었으면 `cli` 상태 그대로).
- 스핀 상한은 없어졌다 — CPU를 계속 돌리는 대신 `hlt`로 쉬므로 의미가 없고, 인터럽트가 영영 안 오면 그대로 멈춘다(실패 모드가 "무한 대기 대신 폴링 타임아웃"에서 "hang"으로 바뀜). 타임아웃은 tick이 있어야 의미가 있는데 `nvme_init` 시점엔 타이머가 아직 없어서 다음 단계 이후로 둔다.
- 동시에 두 스레드가 `nvme_submit`을 호출하면 `done`/`result`가 섞일 수 있지만, 디스크 I/O 경로는 `61-io-lock`의 `g_ext2_lock`(wait queue 기반 잠금)으로 이미 직렬화돼 있어 큐당 outstanding 커맨드는 항상 1개다.

## 검증

`make clean && make run-nogui`(관련 구간):

```
pci: 00:03.0 MSI-X cap=0x40 table_size=65 bir=0 offset=0x2000
nvme: msix entry 0 -> vector 0x30 apic id=0
nvme: msix entry 1 -> vector 0x31 apic id=0
nvme: BAR0 phys=0x00000000FEBD4000 mqes=2048 dstrd=0 stride=4
nvme: admin queue ready (64 entries)
nvme: identify controller model="QEMU NVMe Ctrl" serial="deadbeef" fw="8.2.2"
nvme: identify namespace nsid=1 nsze=32768 blocks lba_size=512
nvme: admin completions via msix: 4 interrupt(s)
nvme: io queue ready (qid=1, 64 entries)
ext2: superblock magic=0xEF53 rev=1 block_size=1024 blocks=16384 inodes=4096
ext2: group 0: inode_table=5 block_bitmap=3 inode_bitmap=4 free_blocks=6482 free_inodes=2020
nvme: io completions via msix: 4 interrupt(s)
initramfs: 13 file(s) found
vfs: initrd mounted at /
vfs: ext2 mounted at /disk/
...
shell: ext2 /disk/hello.txt: hello ext2 root fs
```

`ext2_init`이 끝난 시점(IF=0)에 I/O 완료 인터럽트가 4개 센 것으로, 폴링이 아니라 핸들러를 거쳤음을 확인한다. 이후 `interrupts_enable()`을 거친 뒤 init 프로세스(IF=1)가 `/disk/hello.txt`를 읽는 것도 같은 경로로 정상 동작한다. `grub-file --is-x86-multiboot2 build/kernel.elf` exit 0.

## 이전 단계(68) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/nvme.c` | 수정 | I/O CQ IEN+IV 1, 엔트리 1/vector `0x31` 등록, `nvme_irq`/`nvme_io_irq`, `use_irq`·폴링 제거, `hlt` 기반 `nvme_wait_irq`, 큐별 `irq_count`, `nvme_report` |
| `boot/nvme.h` | 수정 | `nvme_report` 선언 |
| `boot/kernel.c` | 수정 | `ext2_init()` 뒤 `nvme_report()` 호출 |
| 나머지 전부 | 변경 없음 | 68의 파일 그대로 (`Makefile` 포함 — 의존성이 이미 충분) |

## 완료 기준

`make clean && make run-nogui`에서 `msix entry 1 -> vector 0x31`, `io completions via msix: 4 interrupt(s)`가 나오고 `ext2`/`vfs`/셸의 `/disk/hello.txt` 읽기가 68과 같아야 한다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **복수 outstanding 커맨드는 아직 없음**: 큐당 커맨드 1개만 제출하고 기다린다. 인터럽트 기반이라 CPU는 놀지만(`hlt`) 스레드를 양보하지는 않는다. 진짜 비동기(cid별 완료 추적 + wait queue로 `thread_park`)는 NVMe가 더 필요한 단계에서 다룬다.
- **`nvme_wait_irq` 타임아웃 없음**: 인터럽트가 누락되면 hang. 타이머 tick 기반 타임아웃은 `nvme_init` 이후 구간에서만 가능하다.
- **핸들러 컨텍스트 인자 없음**: `nvme_admin_irq`/`nvme_io_irq` 래퍼는 큐가 전역 하나씩이라 성립한다. 컨트롤러가 둘 이상이면 `request_irq`처럼 `dev_id`를 넘겨야 한다.
- **MSI-X vector `0x30`/`0x31` 하드코딩, 벡터 할당기 없음**: 70 rtl8139는 MSI 미지원이라 legacy INTx(IOAPIC 경유)를 쓰는 사례로 이어진다.
- **MSI-X 인터럽트 CPU 고정**: 모든 엔트리가 `apic_id()`(BSP)로 향한다. SMP/CPU별 큐는 범위 밖.
