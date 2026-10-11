# 70 — nic-virtio

**목표**: PCI 기반 virtio-net(modern, `disable-legacy=on`) NIC 드라이버를 붙인다. vendor capability로 common/notify/device 영역을 찾고, feature 협상 후 receiveq/transmitq 두 virtqueue를 세팅하며, 큐별 MSI-X 인터럽트로 완료를 받는다. loopback이 없으므로 박아둔 raw ARP 요청 프레임을 보내 QEMU user 게이트웨이(10.0.2.2)의 응답을 받는 것으로 드라이버만 검증한다. 이더넷/ARP 프로토콜 처리는 하지 않는다(71 이후).

## 0) QEMU 옵션 (`Makefile`)

`QEMUNET := -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on`을 `run`/`run-nogui`에 추가한다. `disable-legacy=on`이면 I/O 포트 기반 legacy 인터페이스 없이 modern(MMIO + vendor capability)으로만 뜨고 device ID가 `0x1041`(`0x1040` + net 종류 번호 1)이 된다. `-netdev user`는 QEMU가 만드는 가짜 라우터(게이트웨이 `10.0.2.2`, 게스트 `10.0.2.15`)다. NIC가 PCI 슬롯 `00:03.0`을 차지해서 NVMe는 `00:02.0`으로 밀렸다 — 드라이버는 슬롯이 아니라 class/ID로 찾으므로 영향 없다.

## 1) PCI 보조 함수 (`boot/pci.h`, `boot/pci.c`)

virtio는 NVMe처럼 BAR0에 레지스터가 고정돼 있지 않아서 capability를 직접 읽어야 한다.

- `pci_config_read8/16/32`를 `static`에서 공개로 바꿨다 — virtio vendor capability 필드(`cfg_type`, `bar`, `offset`, `length`, `notify_off_multiplier`)를 읽는 데 쓴다.
- `pci_find_by_id(vendor, device, ...)`(신규): `pci_find_by_class`와 같은 스캔인데 vendor/device ID로 찾는다. virtio는 class(`0x02/00/00`)가 아니라 vendor `0x1AF4` + device `0x1041`로 고정되므로 필요하다.
- `pci_next_capability(bus, dev, fn, previous, cap_id)`(신규): `previous` 오프셋의 capability 다음부터 `cap_id`와 일치하는 것을 반환한다(`previous == 0`이면 리스트 처음부터, 없으면 `0`). virtio는 ID `0x09`(vendor specific) capability를 여러 개 달아두므로 하나 찾고 끝나는 `pci_find_capability`로는 부족하다. 기존 `pci_find_capability`는 이 함수 위에 얹었다.

## 2) 연속 물리 페이지 할당 (`boot/phys_mem.c`)

`page_alloc_contig(count)`(신규): 비트맵에서 연속으로 비어 있는 `count`개 페이지를 찾아 사용 중으로 표시하고 시작 물리 주소를 반환한다(실패 시 `0`). 패킷 버퍼가 `1526 * N`바이트(N=256이면 약 390KB, 96페이지)를 descriptor `addr` 계산(`시작 주소 + i * 1526`)으로 쓰기 때문에 물리적으로 연속이어야 한다.

## 3) `boot/virtio_net.c` — 드라이버

### 3-1) 장치와 영역 찾기

`pci_find_by_id(0x1AF4, 0x1041)`로 장치를 찾고 `pci_enable_device`(memory space + bus master)를 켠다. 장치가 DMA로 우리 메모리에 직접 쓰므로 bus master가 필수다. capability 리스트에서 ID `0x09`를 순회하며 `cfg_type` 1(common cfg), 2(notify), 4(device cfg)를 찾아 `bar`/`offset`으로 물리 주소를 구하고 페이지 단위로 `page_map_mmio`한다. notify는 추가로 `+0x10`의 `notify_off_multiplier`를 저장한다. ISR(3)은 INTx 전용이라 MSI-X 기반인 이 드라이버에서는 건너뛴다. QEMU에서는 전부 BAR4(64비트 MMIO, `0xFE000000`)의 `0x0000`/`0x2000`/`0x3000`에 있다.

### 3-2) 초기화 순서 (`device_status`)

`0`(리셋) → `ACKNOWLEDGE` → `DRIVER` → feature 협상 → `FEATURES_OK` → 상태 재확인 → 큐 설정 → `DRIVER_OK`. 각 단계는 앞에서 켠 비트를 유지한 채 누적해서 쓴다(`0x01` → `0x03` → `0x0B` → `0x0F`). feature는 `device_feature_select`로 32비트씩 창을 바꿔 읽는다 — `VIRTIO_F_VERSION_1`(비트 32, select 1의 비트 0)이 없으면 modern이 아니므로 실패, `VIRTIO_NET_F_MAC`(비트 5, select 0)이 없으면 MAC을 읽을 수 없으므로 실패한다. 이 둘만 `driver_feature`에 쓴다.

### 3-3) virtqueue 세팅 (`virtq_setup`)

큐마다(receiveq=0, transmitq=1):

1. `queue_select`에 큐 번호를 쓰고 `queue_size`를 읽어 N을 확인한다(QEMU는 256).
2. 영역 4개를 각각 0으로 채워 할당한다 — descriptor table(`16 * N`), available ring(`6 + 2 * N`), used ring(`6 + 8 * N`), 패킷 버퍼(`1526 * N`, virtio-net 헤더 12 + 이더넷 프레임 최대 1514).
3. 앞 3개의 물리 주소를 `queue_desc`/`queue_driver`/`queue_device`에 쓰고(64비트 필드는 lo/hi 32비트 두 번), `queue_notify_off`를 읽어 doorbell 주소(`notify 베이스 + queue_notify_off * notify_off_multiplier`)를 계산해 저장하고, `queue_enable = 1`.
4. `queue_msix_vector`에 MSI-X 테이블 엔트리 번호(receiveq=0, transmitq=1)를 쓰고 다시 읽어 `0xFFFF`(no vector)가 아닌지 확인한다. 엔트리 0/1은 vector `0x32`/`0x33`에 연결한다(`msix_config`는 `0xFFFF`로 둠 — 설정 변경 인터럽트는 안 씀).

### 3-4) 링 준비 (`virtq_fill_ring`)

모든 descriptor `i`에 `addr = buf_phys + i * 1526`, `len = 1526`을 쓰고 available ring `ring[i] = i`로 **한 번만** 고정한다. 이후 데이터가 오갈 때는 available ring의 `idx`만 올린다 — descriptor를 항상 같은 순서(0, 1, …, N−1, 0, …)로 재사용하기 때문에 가능하다. receiveq는 `flags = WRITE`에 `avail->idx = N`(전부 올림), transmitq는 `flags = 0`에 `avail->idx = 0`.

### 3-5) 수신/송신

- **송신**(`virtio_net_send`): 슬롯 = `avail->idx % N`. 그 슬롯의 버퍼 앞 12바이트(virtio-net 헤더)를 0으로 채우고 뒤에 이더넷 프레임을 복사, `desc[slot].len = 12 + 프레임 길이`, `avail->idx++`, doorbell에 큐 번호를 쓴다.
- **수신 인터럽트**(`virtio_net_rx_irq`): 내가 마지막으로 처리한 `last_used`와 `used->idx`가 다르면 새 완료다. 차이만큼 `used->ring[last_used % N]`에서 `id`/`len`을 꺼내 `id` 번째 버퍼의 12바이트 뒤부터 `len - 12`바이트를 이더넷 프레임으로 본다. 처리 후 `avail->idx++`로 버퍼를 되돌리고 doorbell을 울린다.
- **송신 완료 인터럽트**: `last_used`를 따라잡으며 완료 개수만 센다.
- `virtio_notify`는 `DRIVER_OK` 이후에만 호출한다 — 스펙상 `DRIVER_OK` 전에 장치에 알리면 안 되므로 receiveq 버퍼를 올리는 건 큐 설정 때, 첫 doorbell은 `DRIVER_OK` 뒤에 한다.
- 완료를 기다릴 때(`virtio_wait`)는 `cli`로 플래그를 확인하고 `sti; hlt`로 쉰다 — `nvme_wait_irq`의 스레드가 없는 부팅 초기 경로와 같다.

### 3-6) 검증용 ARP 프로브 (`virtio_net_arp_probe`)

device cfg 앞 6바이트에서 읽은 MAC을 출발지로, `10.0.2.15`가 `10.0.2.2`를 who-has 하는 42바이트 ARP 요청 프레임을 브로드캐스트로 만들어 보낸다. 송신 완료 인터럽트와 수신 인터럽트를 기다린 뒤 수신 프레임이 ARP 응답(이더넷 타입 `0x0806`, opcode 2)이면 sender MAC/IP를 출력한다. 프로토콜 해석은 이 검증용 최소 파싱뿐이고 71에서 정식 이더넷/ARP 계층으로 바꾼다.

## 검증

`make clean && make run-nogui`(관련 구간):

```
virtio-net: found device at 00:03.0
virtio-net: notify bar=4 offset=0x3000 length=0x1000 multiplier=4
virtio-net: device cfg bar=4 offset=0x2000 length=0x1000
virtio-net: common cfg bar=4 offset=0x0 length=0x1000
pci: 00:03.0 MSI-X cap=0x98 table_size=4 bir=1 offset=0x0
virtio-net: msix entry 0 -> vector 0x32, entry 1 -> vector 0x33 apic id=0
virtio-net: device features low=0x30BF8024 high=0x00000101
virtio-net: features ok (VERSION_1 + MAC) status=0x0B num_queues=3
virtio-net: queue 0 size=256 desc=0x0000A000 avail=0x0000B000 used=0x0000C000 buf=0x0000D000 notify_off=0 msix entry=0
virtio-net: queue 1 size=256 desc=0x0006D000 avail=0x0006E000 used=0x0006F000 buf=0x00289000 notify_off=1 msix entry=1
virtio-net: mac=52:54:00:12:34:56
virtio-net: driver ok status=0x0F, rxq posted 256 buffers
virtio-net: tx arp who-has 10.0.2.2 tell 10.0.2.15
virtio-net: rx arp reply 10.0.2.2 is-at 52:55:0A:00:02:02 (64 bytes)
...
virtio-net: completions via msix: rx 1 interrupt(s) (1 frame(s)), tx 1 interrupt(s)
...
shell: ext2 /disk/hello.txt: hello ext2 root fs
```

게이트웨이의 MAC `52:55:0A:00:02:02`는 QEMU user 네트워크가 쓰는 고정값이다. 송신·수신 완료가 모두 MSI-X 인터럽트로 들어왔고(각 1회), 기존 NVMe/ext2/VFS/셸 경로(`nvme: io completions via msix: 4`, `/disk/hello.txt` 읽기)는 69와 같다. `grub-file --is-x86-multiboot2 build/kernel.elf` exit 0.

### 기존 서브시스템과의 정합 확인

- **물리 할당기**: 새로 연속 할당 함수를 추가했고 두 큐가 약 780KB(약 190페이지)를 낮은 주소부터 차지한다. `phys_mem_reserve`로 막아 둔 커널/initrd 구간은 비트맵에서 이미 빠져 있어 겹치지 않는다.
- **MSI-X 테이블 창**: `pci_msix_open`이 쓰는 `PCI_MSIX_VADDR` 창은 하나뿐이라, virtio가 같은 창을 다시 매핑하면서 NVMe가 쓰던 테이블 매핑이 덮어써진다. NVMe는 `nvme_init`에서만 테이블을 만지고 그 뒤로는 접근하지 않아서 문제없다. 장치별 창은 컨트롤러가 둘 이상일 때 필요하다.
- **IDT 벡터**: `0x30`/`0x31`(NVMe)에 이어 `0x32`/`0x33`을 하드코딩했다. 벡터 할당기는 아직 없다.
- **`page_map_mmio`의 u32 물리 주소**: BAR4가 `0xFE000000`이라 4GB 아래에 있어 지금은 문제없다. 64비트 BAR가 4GB 위에 배치되면 매핑이 깨진다.

## 이전 단계(69) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/virtio_net.c` | 신규 | virtio-net 드라이버 전체(캡 탐색, 초기화, virtqueue, MSI-X 핸들러, ARP 프로브) |
| `boot/virtio_net.h` | 신규 | `virtio_net_init`/`virtio_net_report` 선언 |
| `boot/pci.c` | 수정 | `pci_config_read*` 공개, `pci_find_by_id`, `pci_next_capability` 추가 |
| `boot/pci.h` | 수정 | 위 함수 선언 |
| `boot/phys_mem.c` | 수정 | `page_alloc_contig` 추가 |
| `boot/phys_mem.h` | 수정 | 선언 |
| `boot/kernel.c` | 수정 | `nvme_init()` 뒤 `virtio_net_init()`, `nvme_report()` 뒤 `virtio_net_report()` |
| `Makefile` | 수정 | `virtio_net.o` 규칙·링크, `QEMUNET` 옵션을 `run`/`run-nogui`에 추가 |
| 나머지 전부 | 변경 없음 | 69의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 `virtio-net: features ok`, `queue 0/1 ... msix entry`, `mac=...`, `driver ok status=0x0F`, `rx arp reply 10.0.2.2 is-at 52:55:0A:00:02:02`, `completions via msix: rx 1 ... tx 1`이 나오고 69의 NVMe/ext2/셸 출력이 그대로여야 한다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **71 이더넷/ARP**: 지금 ARP 프레임은 바이트 배열에 직접 채우고 응답은 첫 필드만 해석한다. 71에서 이더넷 헤더 구조체, ARP 메시지 구조체, ARP 캐시로 바꾸고 `virtio_net_send`를 NIC 송신 인터페이스로, `virtio_net_rx_irq`의 프레임 처리를 수신 콜백으로 분리한다.
- **수신 순서 의존**: `ring[i] = i` 고정과 `avail->idx++`만으로 버퍼를 돌려주는 방식은 장치가 버퍼를 올린 순서대로 돌려준다는 가정에 의존한다. QEMU virtio-net은 그렇게 동작하지만, 순서가 바뀌는 장치라면 `used->ring`의 `id`를 `avail->ring`에 다시 써야 한다.
- **송신 버퍼 해제 시점**: transmitq는 완료 개수만 센다. 지금은 프로브 한 번뿐이라 슬롯 재사용 충돌이 없지만, 연속 송신에서는 `avail->idx - last_used < N`으로만 막고 있다.
- **완료 대기 타임아웃 없음**: `virtio_wait`는 응답이 안 오면 hang한다. 타이머 tick이 있어야 의미 있는 타임아웃이 가능하다(`nvme_wait_irq`와 같은 한계).
- **MSI-X 벡터/창 하드코딩**: NVMe(`0x30`/`0x31`)에 이어 `0x32`/`0x33`, `PCI_MSIX_VADDR` 창 공용. 장치가 더 늘면 벡터 할당기와 장치별 창이 필요하다.
- **물리 연속 `1526 * N` 할당**: N이 크면 연속 구간 확보가 어려워질 수 있다. 큐 크기를 줄여 쓰거나(`queue_size`에 더 작은 값을 쓰기) 프레임 단위로 나누는 방식이 대안이다.
- **미사용 feature**: 체크섬 offload, TSO, mergeable rx buffer(`VIRTIO_NET_F_MRG_RXBUF`), 다중 큐는 협상하지 않았다.
