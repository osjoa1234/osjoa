# 71 — virtio-queue

**목표**: 70에서 매핑한 virtio-net 장치를 실제로 초기화한다. `device_status` 초기화 순서 → feature 협상(`VERSION_1`, `MAC`) → receiveq/transmitq virtqueue 세팅(연속 물리 페이지) → 큐별 MSI-X 연결 → receiveq에 빈 패킷 버퍼를 올리고 notify → `DRIVER_OK`까지 간다. 송신(transmitq에 프레임 올리기)과 ARP 응답 수신은 72에서 한다.

## 1) 연속 물리 페이지 할당 (`boot/phys_mem.h`, `boot/phys_mem.c`)

`u32 page_alloc_contig(u32 count)`(신규): 비트맵에서 `count`개 연속 빈 페이지를 처음부터 훑어 찾아(first-fit) 모두 사용 중으로 표시하고 첫 페이지의 물리 주소를 반환한다. 못 찾으면 `0`이다. virtqueue 세 영역(descriptor table / available ring / used ring)과 패킷 버퍼는 장치가 물리 주소로 직접 읽고 쓰는데, 영역 안에서는 가상·물리가 이어져야 하므로 한 페이지씩 주는 `page_alloc`으로는 부족하다.

## 2) `boot/virtio_net.c` — 초기화 순서

70의 `virtio_net_init()` 끝(`num_queues` 읽기) 뒤에 이어 붙었다.

### 2-1) MSI-X 준비

`pci_msix_open` → 핸들러 등록 → `pci_msix_set_entry` 두 번 → `pci_msix_start`. 테이블 엔트리 0은 receiveq(벡터 `0x32`), 엔트리 1은 transmitq(벡터 `0x33`)다. NVMe가 `0x30`/`0x31`을 쓰고 있어서 이어서 하드코딩했다. 핸들러(`virtio_rx_irq`/`virtio_tx_irq`)는 아직 `irq_count`만 올린다. 패킷 처리는 72 이후다.

### 2-2) `device_status` 순서와 feature 협상

1. `device_status = 0`(reset) 후 0으로 읽힐 때까지 기다린다(`virtio_wait_status`).
2. `ACKNOWLEDGE`(1) → `DRIVER`(2)를 차례로 OR한다(`virtio_add_status`).
3. `device_feature_select`를 0/1로 바꿔가며 장치 feature 64비트를 32비트씩 읽는다(`low=0x30BF8024 high=0x00000101`). `VIRTIO_NET_F_MAC`(비트 5, low)와 `VIRTIO_F_VERSION_1`(비트 32, high의 비트 0)이 둘 다 있어야 하고, 이 둘만 `driver_feature`로 되돌려 쓴다. 나머지(checksum offload, TSO, mergeable rx buffer 등)는 아직 쓸 이유가 없어 협상하지 않는다.
4. `FEATURES_OK`(8)를 OR한 뒤 다시 읽어서 장치가 비트를 유지하는지 확인한다(`status=0x0B`). 지워져 있으면 장치가 feature 조합을 거부한 것이라 `FAILED`(0x80)를 쓰고 중단한다.
5. 큐를 모두 세팅한 뒤 마지막에 `DRIVER_OK`(4)를 OR해 `status=0x0F`가 된다.

`msix_config`(설정 변경 알림용 벡터)는 `0xFFFF`(NO_VECTOR)로 두어 쓰지 않는다.

### 2-3) virtqueue 세팅 (`virtqueue_setup`)

`queue_select`로 큐를 고르고 `queue_size`를 읽는다(QEMU는 256). 영역 배치는 하나의 연속 할당 안에서 다음과 같이 잡는다.

- descriptor table: 오프셋 0, 크기 `16 × size` = 4096바이트(descriptor 하나는 `addr` 8 + `len` 4 + `flags` 2 + `next` 2 = 16바이트).
- available ring: descriptor table 바로 뒤(오프셋 4096), 크기 `flags` 2 + `idx` 2 + `ring` 2×256 + `used_event` 2 = 518바이트.
- used ring: available ring 끝을 페이지 경계로 올림한 오프셋(8192), 크기 `flags` 2 + `idx` 2 + `ring` 8×256 + `avail_event` 2 = 2054바이트.
- 총 `8192 + 2054 = 10246`바이트 → 3페이지.

modern virtio는 세 영역이 따로 주소를 받으므로(`queue_desc`/`queue_driver`/`queue_device`) 붙어 있을 필요는 없지만, 한 번에 할당하는 편이 단순해서 이렇게 했다. 각 주소의 상위 32비트는 0으로 쓴다(물리 주소가 4GB 아래라는 가정 — `page_map_mmio`와 같다).

그 다음 `queue_msix_vector`에 테이블 엔트리 번호를 쓰고 다시 읽어 장치가 받아들였는지 확인한다(거부하면 `0xFFFF`가 읽힌다). `queue_notify_off`를 읽어 저장하고 `queue_enable = 1`로 켠다. 큐 인덱스 0이 receiveq, 1이 transmitq다.

### 2-4) receiveq에 패킷 버퍼 올리기 (`virtio_post_rx_buffers`)

패킷 버퍼 하나는 2048바이트다. virtio-net 헤더 12바이트(`VERSION_1`이면 `num_buffers`가 붙은 12바이트) + 이더넷 프레임 최대 1514바이트(이더넷 헤더 14 + 페이로드 1500) = 1526바이트가 들어가야 해서 2048로 올림했다. 16개를 `page_alloc_contig(8)`(16 × 2048 / 4096)로 한 번에 받아 0으로 채우고, descriptor 0~15가 각각 버퍼 하나를 가리키게 한다(`flags = WRITE`: 장치가 쓰는 버퍼). available ring에 descriptor 번호를 넣고 `avail->idx`를 16으로 올린다. 링 내용을 쓴 뒤에 `idx`를 쓰도록 컴파일러 배리어를 둔다(x86은 쓰기 순서가 유지된다).

마지막으로 notify 영역의 `notify_base + queue_notify_off × notify_off_multiplier`(= `0 × 4`)에 큐 인덱스(16비트)를 써서 장치에 알린다.

### 2-5) MAC 읽기

`VIRTIO_NET_F_MAC`을 협상했으므로 device cfg 영역 오프셋 0부터 6바이트가 MAC이다(`52:54:00:12:34:56`).

## 검증

`make clean && make run-nogui`(관련 구간):

```
virtio-net: common cfg readable num_queues=3
pci: 00:03.0 MSI-X cap=0x98 table_size=4 bir=1 offset=0x0
virtio-net: msix entry 0 -> vector 0x32 apic id=0
virtio-net: msix entry 1 -> vector 0x33 apic id=0
virtio-net: device features low=0x30BF8024 high=0x00000101
virtio-net: features negotiated VERSION_1 + MAC status=0x0B
virtio-net: queue 0 size=256 pages=3 desc=0xA000 avail=0xB000 used=0xC000 notify_off=0 msix entry=0
virtio-net: queue 1 size=256 pages=3 desc=0xD000 avail=0xE000 used=0xF000 notify_off=1 msix entry=1
virtio-net: receiveq posted 16 packet buffers of 2048 bytes at 0x10000
virtio-net: mac 52:54:00:12:34:56
virtio-net: driver ready status=0x0F rx used idx=0 tx used idx=0
...
nvme: io completions via msix: 4 interrupt(s)
shell: ext2 /disk/hello.txt: hello ext2 root fs
```

`status`가 `0x0B`(ACK+DRIVER+FEATURES_OK)에서 `0x0F`(+DRIVER_OK)가 되고, 큐 `msix_vector` 쓰기 확인이 통과하고, device cfg 영역에서 QEMU 기본 MAC이 읽힌다 — 70에서 확인하지 못한 device cfg 매핑이 이로써 검증됐다. `grub-file --is-x86-multiboot2 build/kernel.elf` exit 0, 컴파일 경고 없음.

### 기존 서브시스템과의 정합 확인

- **물리 할당기**: `page_alloc_contig`는 기존 비트맵과 `free_count`를 그대로 갱신하므로 `page_alloc`/`page_free`와 섞어 써도 일관된다. 큐와 버퍼는 이미 디렉터리에 매핑된 RAM 안이라 `KERNEL_OFFSET + 물리 주소`로 바로 접근한다.
- **MSI-X 창 공용**: `pci_msix_open`이 같은 가상 주소 창(`PCI_MSIX_VADDR`)을 다시 매핑해 NVMe 테이블 매핑을 덮어쓴다. NVMe는 `nvme_init`에서만 테이블을 만지고 이후 인터럽트는 장치가 이미 프로그램된 주소/데이터로 보내므로 영향이 없다 — I/O 완료 인터럽트 4개가 그대로 나온다.
- **IDT 벡터**: `0x32`/`0x33`은 NVMe(`0x30`/`0x31`)와 겹치지 않는다.
- **BAR4 영역 재사용 없음**: 이번에 새로 매핑한 MMIO가 없다(70의 세 창과 MSI-X 창만 사용).

## 이전 단계(70) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/virtio_net.c` | 수정 | MSI-X 연결, `device_status` 순서, feature 협상, virtqueue 세팅, receiveq 버퍼 게시와 notify, MAC 읽기 |
| `boot/phys_mem.c` | 수정 | `page_alloc_contig` 추가 |
| `boot/phys_mem.h` | 수정 | `page_alloc_contig` 선언 |
| `Makefile` | 수정 | `virtio_net.o` 의존성에 `apic.h`/`interrupts.h`/`phys_mem.h` 추가 |
| 나머지 전부 | 변경 없음 | 70의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 위 검증 출력(`status=0x0B` → `driver ready status=0x0F`, MAC, receiveq 16개 게시)이 나오고 NVMe/ext2/셸 출력이 70과 같아야 한다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **72 `72-virtio-arp`**: transmitq에 virtio-net 헤더 + raw ARP 요청 프레임을 올리고 `queue_notify`로 알린다. 게이트웨이(`10.0.2.2`) ARP 응답이 receiveq의 used ring으로 돌아와야 한다.
- **미룬 검증 — MSI-X 인터럽트 실제 발생**: 지금은 트래픽이 없어 `virtio_rx_irq`/`virtio_tx_irq`가 한 번도 불리지 않고, `queue_msix_vector` 쓰기 확인과 `status=0x0F`까지만 검증했다. 장치→벡터 `0x32`/`0x33` 도달은 72에서 송신 완료/ARP 응답 수신 때 `irq_count`로 처음 확인된다. 같은 이유로 receiveq doorbell(notify 영역) 쓰기의 장치 쪽 효과(버퍼를 가져가는지)도 72에서 확인된다.
- **핸들러 후속 작업**: used ring을 소비하고 버퍼를 receiveq에 되돌려 올리는 일(재게시)은 72에서 한다. 지금 핸들러는 카운트만 한다.
- **IDT 벡터 할당기 없음**: 벡터를 계속 하드코딩하고 있다.
- **4GB 이상 물리 주소**: `queue_*_hi`를 0으로 쓰고 `page_alloc_contig`도 `u32` 주소다.
