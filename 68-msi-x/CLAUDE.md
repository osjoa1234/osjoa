# 68 — msi-x

**목표**: `67-nvme-io`의 NVMe 컨트롤러를 그대로 재사용해, PCI capability list에서 MSI-X capability(ID `0x11`)를 찾아 파싱·프로그래밍하고 IDT에 핸들러를 등록해서 **admin CQ 하나만** CQ phase bit 폴링 대신 MSI-X 인터럽트로 완료를 통지받게 한다. I/O 큐(QID 1)는 이번 단계에서 건드리지 않아 폴링 그대로다(`69-msi-x-nvme-io`). 새 디바이스 드라이버는 추가하지 않는다.

## 0) PCI: MSI-X capability 파싱 + 테이블 프로그래밍 (`boot/pci.c`, `boot/pci.h`)

`pci_msix_open(bus, device, function, &msix)`는 capability list(`0x34`의 포인터를 따라 `next`를 순회)에서 ID `0x11`을 찾고 성공 0 / 없으면 -1을 돌려준다. 찾으면:

- offset `+2` message control에서 테이블 크기(비트 10:0, 0 기반 → +1)를 읽는다.
- offset `+4` Table Offset/BIR에서 하위 3비트(BIR)가 가리키는 BAR의 물리주소 + 나머지 비트(오프셋)로 테이블 물리주소를 구하고, 그 페이지를 `page_map_mmio`로 매핑한다(`PCI_MSIX_VADDR = KERNEL_OFFSET + 0x41005000`, 64의 LAPIC/IOAPIC, 65의 ECAM, 66의 NVMe 매핑과 겹치지 않는 슬롯).
- message control에 **function mask(비트 14) + enable(비트 15)를 같이 켠다**. 테이블 엔트리를 채우는 동안 장치가 반쯤 쓰인 엔트리로 메시지를 쏘지 않게 하기 위해서다.

`pci_msix_set_entry(&msix, index, vector, apic_id)`는 엔트리 하나(16바이트: address low/high, data, vector control)를 쓴다. 먼저 per-vector mask(vector control 비트 0)를 걸고 채운 뒤 푼다. address = `0xFEE00000 | (apic_id << 12)`(destination ID, 물리 모드), data = vector(전달 모드 fixed, edge). `pci_msix_start(&msix)`는 function mask만 끈다 — 이 시점부터 인터럽트가 들어온다.

## 1) IDT 핸들러 등록 (`boot/interrupts.c`, `boot/interrupts.h`)

`interrupts_register_handler(vector, handler)`: `vector_handlers[256]` 테이블에 함수 포인터를 넣는다. `interrupt_dispatch`는 exception(<32), PIC 범위 IRQ(<48) 뒤에서 등록된 핸들러가 있으면 EOI를 보내고 핸들러를 부른다. IRQ0/1용 `handle_irq`와 같이 EOI를 디스패치 쪽에서 먼저 보낸다. MSI-X vector는 `0x30`(48)부터 쓰기로 해서 IOAPIC이 쓰는 `0x20~0x2F`와 분리했다.

## 2) NVMe admin CQ를 인터럽트로 (`boot/nvme.c`)

- `nvme_init`이 컨트롤러를 enable하기 전에 `pci_msix_open` → `interrupts_register_handler(0x30, nvme_admin_irq)` → `pci_msix_set_entry(entry 0, vector 0x30, apic_id())` → `pci_msix_start` 순서로 MSI-X를 준비한다. 엔트리 0은 NVMe 스펙상 admin CQ의 인터럽트 벡터(IV) 0이 가리키는 자리다(admin CQ는 Create 커맨드 없이 IV 0 고정).
- `nvme_reap(q)`: 현재 head의 CQE status를 읽고 head 증가(+phase 반전) + CQ head doorbell 쓰기까지 하는 공용 함수. 폴링 경로와 핸들러가 같이 쓴다.
- `nvme_admin_irq()`: phase 비트를 확인(어긋나면 무시) → `nvme_reap` → `q->result`에 status 저장 → `g_admin_irq_count` 증가 → `q->done = 1`. EOI는 디스패치에서 이미 보냈다.
- `nvme_submit`: `q->use_irq`이면 doorbell을 친 뒤 `nvme_wait_irq`로 `done`을 기다리고, 아니면 67의 phase 폴링 그대로다. admin 큐는 컨트롤러 enable 직후 `use_irq = 1`, I/O 큐는 0이다.
- `nvme_wait_irq`: `nvme_init`이 `interrupts_enable()` 이전에 실행되므로(IF=0) 대기 동안만 `sti`로 인터럽트를 열고 끝나면 원래 IF로 되돌린다(`pushfq`로 저장). 이 시점엔 IOAPIC 항목이 전부 masked이고 타이머/키보드도 초기화 전이라 `sti`로 열어도 MSI-X 외에 들어올 인터럽트가 없다. `done`이 안 세워지는 경우를 위해 스핀 상한을 둬 무한 대기를 피한다.

## 검증

`make clean && make run-nogui`(관련 구간):

```
pci: 00:03.0 MSI-X cap=0x40 table_size=65 bir=0 offset=0x2000
nvme: msix entry 0 -> vector 0x30 apic id=0
nvme: BAR0 phys=0x00000000FEBD4000 mqes=2048 dstrd=0 stride=4
nvme: admin queue ready (64 entries)
nvme: identify controller model="QEMU NVMe Ctrl" serial="deadbeef" fw="8.2.2"
nvme: identify namespace nsid=1 nsze=32768 blocks lba_size=512
nvme: admin completions via msix: 4 interrupt(s)
nvme: io queue ready (qid=1, 64 entries)
ext2: superblock magic=0xEF53 rev=1 block_size=1024 blocks=16384 inodes=4096
```

QEMU NVMe는 MSI-X 테이블을 BAR0 안(오프셋 `0x2000`)에 두므로 BIR=0이다. admin 커맨드는 Identify Controller, Identify Namespace, Create I/O CQ, Create I/O SQ 4개이고, 인터럽트 카운터도 4다 — 4개 전부가 폴링이 아니라 핸들러 경유로 완료됐다는 뜻이다. 그 뒤 ext2 마운트와 `/disk/hello.txt` 읽기는 67과 같이 I/O 큐 폴링으로 동작한다. `grub-file --is-x86-multiboot2 build/kernel.elf` exit 0.

## 이전 단계(67) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/pci.c`, `boot/pci.h` | 수정 | `struct pci_msix`, `pci_msix_open`(capability 탐색 + 테이블 매핑 + enable/function mask), `pci_msix_set_entry`, `pci_msix_start` |
| `boot/interrupts.c`, `boot/interrupts.h` | 수정 | `interrupts_register_handler` + 벡터별 핸들러 테이블, 48 이상 벡터에서 핸들러 디스패치 |
| `boot/nvme.c` | 수정 | MSI-X 준비, `nvme_reap`/`nvme_admin_irq`/`nvme_wait_irq`, `nvme_queue`에 `use_irq`/`done`/`result`, admin 큐만 인터럽트 대기 |
| `Makefile` | 수정 | `nvme.o` 의존성에 `apic.h`, `interrupts.h` 추가 |
| 나머지 전부 | 변경 없음 | 67의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 위 "검증" 블록의 `pci: ... MSI-X`, `nvme: msix entry 0 -> vector 0x30`, `admin completions via msix: 4 interrupt(s)`가 나오고 이후 ext2/vfs/shell 흐름이 67과 같아야 한다. `grub-file --is-x86-multiboot2 build/kernel.elf`가 exit 0이어야 한다.

## 다음 단계 힌트

- **I/O CQ는 아직 폴링**: `69-msi-x-nvme-io`에서 Create I/O CQ의 CDW11에 IEN(비트 1) + IV(상위 16비트)=1을 넣고, 테이블 엔트리 1번을 같은 방식으로 채우며, 폴링 경로를 제거한다. 테이블은 `table_size=65`라 엔트리 1번도 이미 존재한다.
- **`nvme_wait_irq`의 `sti`는 `nvme_init` 시점(IF=0) 전용 우회다**: I/O 경로로 가면 `interrupts_enable()` 이후라 IF=1 상태에서 호출되고, 그때는 `hlt` 기반 대기나 wait queue로 CPU를 양보하는 쪽이 맞다. 스핀 상한 대기는 69에서 정리한다.
- **핸들러가 `g_admin`을 직접 참조**: 큐/컨트롤러가 하나라 문제없지만, 장치가 늘면 핸들러에 컨텍스트 인자를 넘겨야 한다(리눅스의 `request_irq(..., dev_id)`).
- **테이블 한 페이지만 매핑**: 엔트리 0,1은 페이지 안에 들어오지만, 테이블이 페이지 경계를 넘는 장치/엔트리 수에서는 여러 페이지를 매핑해야 한다. PBA(pending bit array)는 파싱·사용하지 않는다.
- **MSI-X vector 0x30 하드코딩**: 벡터 할당기는 없다. 장치가 늘 때(70 virtio-net) 필요해진다.
