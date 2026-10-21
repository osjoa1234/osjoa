# 70 — virtio-pci

**목표**: PCI 기반 virtio-net(modern, `disable-legacy=on`) 장치를 찾고, vendor capability를 순회해 common cfg / notify / device cfg 영역의 위치를 알아낸 뒤 MMIO로 매핑한다. 매핑이 실제로 읽히는지는 common cfg의 `num_queues`를 읽어서 확인한다. 장치 초기화(`device_status`), feature 협상, virtqueue, MSI-X는 71, 송신과 ARP 검증은 72에서 한다.

## 0) QEMU 옵션 (`Makefile`)

`QEMUNET := -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on`을 `run`/`run-nogui`에 추가한다. `disable-legacy=on`이면 I/O 포트 기반 legacy 인터페이스 없이 modern(MMIO + vendor capability)으로만 뜨고 device ID가 `0x1041`(`0x1040` + net 종류 번호 1)이 된다. `-netdev user`는 QEMU가 만드는 가짜 라우터(게이트웨이 `10.0.2.2`, 게스트 `10.0.2.15`)다. NIC가 PCI 슬롯 `00:03.0`을 차지해서 NVMe는 `00:02.0`으로 밀렸다 — 드라이버는 슬롯이 아니라 class/ID로 찾으므로 영향 없다.

## 1) PCI 보조 함수 (`boot/pci.h`, `boot/pci.c`)

virtio는 NVMe처럼 BAR0에 레지스터가 고정돼 있지 않아서 capability를 직접 읽어야 한다.

- `pci_config_read8/16/32`를 `static`에서 공개로 바꿨다 — virtio vendor capability 필드(`cfg_type`, `bar`, `offset`, `length`, `notify_off_multiplier`)를 읽는 데 쓴다.
- `pci_find_by_id(vendor, device, ...)`(신규): `pci_find_by_class`와 같은 스캔인데 vendor/device ID로 찾는다. virtio는 class(`0x02/00/00`)가 아니라 vendor `0x1AF4` + device `0x1041`로 고정되므로 필요하다.
- `pci_next_capability(bus, dev, fn, previous, cap_id)`(신규): `previous` 오프셋의 capability 다음부터 `cap_id`와 일치하는 것을 반환한다(`previous == 0`이면 리스트 처음부터, 없으면 `0`). virtio는 ID `0x09`(vendor specific) capability를 여러 개 달아두므로 하나 찾고 끝나는 `pci_find_capability`로는 부족하다. 기존 `pci_find_capability`는 이 함수 위에 얹었다.

## 2) `boot/virtio_net.c` — 영역 탐색과 매핑

### 2-1) 장치 찾기

`pci_find_by_id(0x1AF4, 0x1041)`로 장치를 찾고 `pci_enable_device`(memory space + bus master)를 켠다. memory space가 꺼져 있으면 BAR 영역을 읽을 수 없다. bus master는 장치가 DMA를 할 71부터 필요하지만 `pci_enable_device`가 둘을 함께 켠다.

### 2-2) capability 순회 (`virtio_find_capabilities`)

ID `0x09` capability를 `pci_next_capability`로 하나씩 따라가며 각 capability에서 `cfg_type`(+3), `bar`(+4), `offset`(+8), `length`(+12)를 읽는다. 영역의 물리 주소는 `pci_bar_address(bar) + offset`이다. `cfg_type`이 1(common cfg), 2(notify), 4(device cfg)인 것만 `virtio_map_region(vaddr, phys, length)`로 매핑하고 전역에 저장한다. 영역이 페이지 중간에서 시작하면 `length`가 0x1000이어도 두 페이지에 걸치므로, 매핑 페이지 수는 `((phys & 0xFFF) + length + 0xFFF) >> 12`로 구해 `page_map_mmio`를 페이지 수만큼 반복한다. 영역당 가상 주소 창은 4페이지(`VIRTIO_REGION_PAGES`)이고 이를 넘는 영역은 매핑 실패(`NULL`)로 처리해 해당 영역을 못 찾은 것으로 본다. notify는 `+16`의 `notify_off_multiplier`도 저장한다. ISR(3)은 INTx 전용이라 MSI-X 기반인 이 드라이버에서는 건너뛴다. 세 영역을 모두 찾지 못하면 실패(`-1`)다.

QEMU에서는 전부 BAR4(64비트 MMIO, `0xFE000000`)의 `0x0000`(common)/`0x2000`(device)/`0x3000`(notify)에 있고 길이는 모두 `0x1000`이다. 전부 페이지 정렬이라 실제로는 영역당 한 페이지만 매핑된다.

매핑에 쓰는 커널 가상 주소는 `0x41006000`/`0x4100A000`/`0x4100E000`(`KERNEL_OFFSET` 기준, 영역당 4페이지 창)이다. 기존 `0x41000000`(LAPIC) ~ `0x41005000`(MSI-X 창)과 겹치지 않는다.

### 2-3) 매핑 확인

`struct virtio_common_cfg`(common cfg 영역의 필드 배치 전체)를 선언하고 `num_queues`(오프셋 `0x12`)를 읽어서 출력한다. 값이 `3`으로 읽히면 매핑한 주소가 실제로 장치 레지스터를 가리킨다는 뜻이다(QEMU virtio-net: receiveq, transmitq, 그리고 하나 더). notify와 device cfg는 각각 71의 doorbell 쓰기, MAC 읽기에서 처음 쓰이므로 그때 확인된다.

## 검증

`make clean && make run-nogui`(관련 구간):

```
nvme: found controller at 00:02.0
virtio-net: found device at 00:03.0
virtio-net: notify bar=4 offset=0x3000 length=0x1000 multiplier=4
virtio-net: device cfg bar=4 offset=0x2000 length=0x1000
virtio-net: common cfg bar=4 offset=0x0 length=0x1000
virtio-net: common cfg readable num_queues=3
...
nvme: io completions via msix: 4 interrupt(s)
...
shell: ext2 /disk/hello.txt: hello ext2 root fs
```

NIC가 `00:03.0`, NVMe가 `00:02.0`이다. 기존 NVMe/ext2/VFS/셸 경로(`nvme: io completions via msix: 4`, `/disk/hello.txt` 읽기)는 69와 같다. `grub-file --is-x86-multiboot2 build/kernel.elf` exit 0.

### 기존 서브시스템과의 정합 확인

- **`pci_find_capability`**: `pci_next_capability(..., previous=0, ...)` 위의 얇은 함수로 바꿨다. NVMe의 MSI-X 캐퍼빌리티 탐색이 이 함수를 쓰는데 `nvme: io completions via msix: 4`가 그대로 나와 동작이 같다.
- **PCI 슬롯 순서**: NIC 추가로 NVMe가 `00:02.0`이 됐지만 NVMe는 class로 찾으므로 영향 없다.
- **MMIO 가상 주소 창**: 새로 쓰는 세 페이지가 기존 창과 겹치지 않는다.
- **`page_map_mmio`의 u32 물리 주소**: BAR4가 `0xFE000000`이라 4GB 아래에 있어서 지금은 문제없다. 64비트 BAR가 4GB 위에 배치되면 매핑이 깨진다.

## 이전 단계(69) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/virtio_net.c` | 신규 | virtio-net 장치 탐색, vendor capability 순회, common/notify/device cfg MMIO 매핑, `num_queues` 읽기 |
| `boot/virtio_net.h` | 신규 | `virtio_net_init` 선언 |
| `boot/pci.c` | 수정 | `pci_config_read*` 공개, `pci_find_by_id`, `pci_next_capability` 추가 |
| `boot/pci.h` | 수정 | 위 함수 선언 |
| `boot/kernel.c` | 수정 | `nvme_init()` 뒤 `virtio_net_init()` |
| `Makefile` | 수정 | `virtio_net.o` 규칙·링크, `QEMUNET` 옵션을 `run`/`run-nogui`에 추가 |
| 나머지 전부 | 변경 없음 | 69의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 `virtio-net: found device`, `notify/device cfg/common cfg` 세 줄, `common cfg readable num_queues=3`이 나오고 69의 NVMe/ext2/셸 출력이 그대로여야 한다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **71 `71-virtio-queue`**: 이번에 매핑만 하고 안 쓰는 `g_notify_base`/`g_notify_multiplier`/`g_device_cfg`를 쓴다. `device_status` 초기화 순서, feature 협상(`VIRTIO_F_VERSION_1`, `VIRTIO_NET_F_MAC`), receiveq/transmitq virtqueue 세팅, 큐별 MSI-X 연결, receiveq에 빈 패킷 버퍼를 올리고 `DRIVER_OK`까지 간다.
- **연속 물리 페이지 할당**: descriptor table / available ring / used ring / 패킷 버퍼는 장치가 물리 주소로 직접 읽고 쓰므로 물리적으로 연속이어야 한다. 기존 `page_alloc`은 한 페이지씩이라 71에서 `page_alloc_contig`를 추가해야 한다.
- **MSI-X 창 공용**: `pci_msix_open`이 쓰는 `PCI_MSIX_VADDR` 창이 하나뿐이라 virtio가 같은 창을 매핑하면 NVMe 테이블 매핑이 덮어써진다. NVMe는 `nvme_init`에서만 테이블을 만지고 그 뒤로 접근하지 않아서 문제없다. 71에서 MSI-X를 붙일 때 확인하고 남긴다.
- **IDT 벡터 할당**: NVMe가 `0x30`/`0x31`을 하드코딩했다. 71에서 `0x32`/`0x33`을 이어서 하드코딩하게 되며 벡터 할당기는 아직 없다.
- **notify / device cfg 확인**: 두 영역은 이번 단계에서 읽기 확인을 하지 못했다. notify는 71의 receiveq doorbell 쓰기, device cfg는 71의 MAC 읽기에서 처음 쓰이므로 그때 검증한다.
