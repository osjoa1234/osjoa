# 73 — ethernet-arp

**목표**: 72에서 바이트를 박아 넣어 검증한 virtio-net 송수신 위에 이더넷 계층과 ARP 계층을 올려 ARP를 완성한다. 이더넷 헤더/ARP 패킷을 구조체로 정의하고, `virtio_net.c`에 섞여 있던 ethertype 분기와 ARP 처리를 `ethernet.[ch]`/`arp.[ch]`로 옮기고, ARP 캐시(IP → MAC)와 "내 IP를 묻는 요청에 reply 송신"을 만든다. reply를 보내려면 요청과 reply를 번갈아 송신해야 하므로 transmitq 송신 버퍼 관리도 같이 만든다.

## 1) 계층 분리

- `virtio_net.c`: 장치 초기화, virtqueue, MSI-X 핸들러, 송신 버퍼 슬롯 관리만 맡는다. 수신한 프레임에서 virtio-net 헤더 12바이트를 떼고 `ethernet_rx`로 올린다. 위 계층은 `virtio_net_tx_alloc`/`virtio_net_tx_submit`으로 송신하고 `virtio_net_mac`으로 MAC을 얻는다.
- `ethernet.[ch]`: `struct eth_hdr`(dst 6 + src 6 + ethertype 2 = 14바이트), `htons`/`ntohs`, 브로드캐스트 MAC(`ethernet_broadcast_mac`), `ethernet_rx`(길이와 목적지 MAC 검사 후 ethertype으로 분기), `ethernet_send`(헤더를 쓰고 페이로드를 복사해 `virtio_net_tx_submit`). 목적지 MAC이 내 MAC도 브로드캐스트도 아니면 버린다(리눅스가 `eth_type_trans`에서 패킷을 호스트 대상으로 분류하는 것과 같은 역할).
- `arp.[ch]`: `struct arp_packet`(28바이트), ARP 캐시, 요청/reply 송신, `arp_rx`, `arp_test`.

`virtio_handle_rx_frame`에 있던 ARP 응답 출력은 `arp_rx`로 갔고, 72의 `g_arp_reply_seen` 플래그는 없어졌다. 대신 대기 조건이 "게이트웨이가 ARP 캐시에 들어왔는가"가 됐다.

## 2) ARP (`arp.c`)

- `arp_rx`: 길이 28 미만, hardware type이 이더넷(1)이 아니거나 protocol type이 IPv4(`0x0800`)가 아니거나 hardware size가 6이 아니거나 protocol size가 4가 아니면 버린다. target IP가 내 IP(`10.0.2.15`)가 아니어도 버린다. 그 외에는 sender IP/MAC을 캐시에 넣고, opcode가 요청(1)이면 reply를 보낸다. RFC 826의 "target이 나일 때만 캐시를 갱신한다(merge flag)"를 따른 것이다.
- ARP 캐시: 8칸 배열. 같은 IP가 있으면 MAC만 갱신(MAC이 같으면 아무것도 안 한다), 없으면 빈 칸, 꽉 찼으면 라운드 로빈으로 덮어쓴다. 만료는 없다.
- `arp_cache_lookup`: IP로 찾아 MAC을 출력 인자에 복사하고 0/-1을 리턴한다. 캐시는 MSI-X 핸들러(수신)와 메인 스레드(테스트)가 모두 만지므로 `cache_update`/`arp_cache_lookup`은 진입 시 `rflags`를 저장하고 `cli`, 끝나면 IF를 복원한다.
- `arp_send_request(target_ip)`: 72의 게이트웨이 고정 요청을 target IP 인자를 받도록 일반화했다. 목적지 MAC은 브로드캐스트, target MAC은 0이다. 로그는 `ethernet_send`를 부르기 *전에* 찍는다(72의 "doorbell 전에 로그" 규칙을 이어서, 송신 직후 도착한 수신 인터럽트가 로그 중간에 끼어드는 걸 막는다).
- reply: 목적지는 요청 보낸 쪽의 MAC(유니캐스트), sender는 나, target은 요청자다.

## 3) 송신 버퍼 관리 (`virtio_net.c`)

72는 descriptor 0과 한 페이지를 한 번만 썼다. 73은 receiveq와 대칭으로 `page_alloc_contig(8)`로 2048바이트 슬롯 16개(`16 × 2048 / 4096 = 8` 페이지)를 잡고, `desc[i]`가 슬롯 `i`를 영구히 가리키게 한다. 슬롯 점유는 `g_tx_busy` 비트마스크(16비트 사용)로 관리한다.

- `virtio_net_tx_alloc`: used ring에 올라온 항목(장치가 다 읽은 슬롯)의 `id`로 점유 비트를 지운 뒤(`virtio_reclaim_tx`), 비어 있는 가장 낮은 슬롯을 점유하고 그 슬롯의 virtio-net 헤더(12바이트, 초기화 때 0으로 채워둔 채 아무도 안 건드림) 다음 주소를 돌려준다. 빈 슬롯이 없으면 0.
- `virtio_net_tx_submit`: 포인터에서 슬롯 번호를 역산해 `desc[id].len = 12 + 프레임 길이`로 정하고 available ring에 넣은 뒤 `idx` 갱신, doorbell. 인터럽트 컨텍스트(reply)와 메인 스레드(요청)가 모두 부르므로 `avail_idx`/ring 갱신은 `cli` 아래에서 한다.

슬롯을 `alloc`으로 받은 뒤 `submit`하기 전까지는 호출자가 그 슬롯을 단독 소유하므로 프레임 작성(`ethernet_send`의 헤더/페이로드 복사)은 잠금 없이 한다.

## 4) 대기 (`virtio_net_wait`)

72의 `virtio_wait_arp_reply`를 조건 함수를 받는 `virtio_net_wait(done, ms)`로 일반화했다. `cli` → `done()` → 마감 확인 → `g_waiter = thread_current(); thread_sleep(남은 ms); g_waiter = 0` 루프 구조와 IF 저장/복원은 72와 같고(`MSI-X` 핸들러가 `g_waiter`를 `thread_unpark`로 깨운다), 종료 조건만 호출자가 정한다. `virtio_net_idle`은 "보낸 건 장치가 다 가져갔고(`tx used idx == avail_idx`), tx 인터럽트가 송신 횟수만큼 왔고(`g_tx.irq_count == avail_idx`), rx 인터럽트가 한 번 이상 왔다"이다.

## 검증

`make clean && make run-nogui`(관련 구간):

```
virtio-net: transmitq prepared 16 packet buffers of 2048 bytes at 0x18000
virtio-net: mac 52:54:00:12:34:56
virtio-net: driver ready status=0x0F rx used idx=0 tx used idx=0
arp: tx request who-has 10.0.2.2 tell 10.0.2.15
arp: rx reply 10.0.2.2 is-at 52:55:0A:00:02:02
arp: cache 10.0.2.2 -> 52:55:0A:00:02:02
arp: lookup 10.0.2.2 -> 52:55:0A:00:02:02
arp: rx request who-has 10.0.2.15 tell 10.0.2.99
arp: cache 10.0.2.99 -> 02:00:00:00:00:99
arp: tx reply 10.0.2.15 is-at 52:54:00:12:34:56 to 10.0.2.99
arp: lookup 10.0.2.99 -> 02:00:00:00:00:99
virtio-net: tx used idx=2 rx used idx=1
virtio-net: msix interrupts rx=1 tx=2
```

3회 연속 같은 출력, `grub-file --is-x86-multiboot2 build/kernel.elf` exit 0, 새 파일(`ethernet.c`, `arp.c`)과 수정한 `virtio_net.c`에 컴파일 경고 없음. NVMe 완료 인터럽트(admin 4, io 4)와 ext2 셸 출력은 72와 같다.

**검증 방법의 한계**: 첫 구간(요청 → 게이트웨이 reply → 캐시)은 실제 장치를 거친 경로다. 둘째 구간(요청 수신 → 캐시 → reply 송신)은 QEMU user 네트워크(slirp)가 게스트에게 먼저 ARP 요청을 보내지 않아서 실제 도착 경로를 검증하지 못했다. 대신 `arp_test`가 `10.0.2.99`가 보낸 것으로 꾸민 ARP 요청 구조체를 `arp_rx`에 직접 넘긴다. 이 방법으로 확인되는 것은 ARP 요청 파싱, 캐시 갱신, reply 구조체 조립, 송신 슬롯 할당·제출·장치 소비(`tx used idx=2`)까지고, 확인 못 한 것은 reply 프레임이 실제 와이어에서 올바른 바이트로 나가는지다. 둘째 reply는 slirp가 무시하는 `02:00:00:00:00:99`로 가서 수신 쪽 관찰도 없다.

### 기존 서브시스템과의 정합 확인

- **물리 할당기**: 송신 슬롯 8페이지를 `page_alloc_contig`로 받는다. 72에서 단발 `page_alloc()`로 받던 한 페이지 송신 버퍼는 없어졌다(한 번뿐이라 해제하지 않던 것을 슬롯 재사용으로 대체).
- **인터럽트 상태**: `virtio_net_wait`는 72와 같이 진입 시 IF를 저장하고 끝나면 복원한다. `virtio_net_tx_alloc`/`tx_submit`/`cache_update`/`arp_cache_lookup`도 같은 패턴으로 IF 상태를 보존한다(IRQ 핸들러 안에서 불려도 IF를 켜지 않는다).
- **스케줄러**: 72와 같이 호출 시점에 스레드는 idle뿐이라 `thread_sleep`이 `hlt` 루프로 떨어지고 실제 문맥 전환은 일어나지 않는다. 다른 스레드가 있을 때의 경로는 여전히 실행으로 검증되지 않았다.
- **NVMe/ext2**: I/O 완료 인터럽트와 ext2 출력이 72와 같다.
- **receiveq 재게시**: 72와 같이 처리한 버퍼를 되돌려 16개를 유지한다.
- **송신 슬롯 고갈**: 슬롯 16개를 다 쓰는 경로(`virtio_net_tx_alloc`이 0을 리턴)는 이번 테스트가 송신 2번뿐이라 실행되지 않았다.

## 이전 단계(72) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/ethernet.h` | 신규 | `struct eth_hdr`, `htons`/`ntohs`, `ethernet_rx`/`ethernet_send` 등 선언 |
| `boot/ethernet.c` | 신규 | 이더넷 수신(길이·목적지 MAC 검사, ethertype 분기)과 송신(헤더 조립), MAC 비교/출력 |
| `boot/arp.h` | 신규 | `arp_rx`, `arp_send_request`, `arp_cache_lookup`, `arp_test` 선언 |
| `boot/arp.c` | 신규 | `struct arp_packet`, ARP 캐시, 요청/reply 송신, `arp_rx`, 게이트웨이 해석 + 요청 주입 테스트 |
| `boot/virtio_net.h` | 수정 | `virtio_net_arp_test` 제거, 송신 슬롯/대기/MAC/상태 인터페이스 추가 |
| `boot/virtio_net.c` | 수정 | ARP 프레임 조립·ethertype 분기·`g_arp_reply_seen` 제거, 송신 슬롯 16개 관리, 수신 프레임을 `ethernet_rx`로 전달, 조건 함수를 받는 `virtio_net_wait` |
| `boot/kernel.c` | 수정 | `virtio_net_arp_test()` 호출을 `arp_test()`로 교체 |
| `Makefile` | 수정 | `ethernet.o`, `arp.o` 규칙과 링크 대상 추가 |
| 나머지 전부 | 변경 없음 | 72의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 위 검증 출력(게이트웨이 요청 → reply → `arp: cache`/`arp: lookup`, 주입 요청 → 캐시 → `arp: tx reply`, `tx used idx=2`, `msix interrupts rx=1 tx=2`)이 나오고 NVMe/ext2/셸 출력이 72와 같아야 한다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **74 `74-ip-icmp`**: IPv4 헤더와 ICMP echo. `ethernet_rx`에 ethertype `0x0800`(IPv4) 분기를 추가하고, 상대 MAC은 ARP 캐시 `arp_cache_lookup`으로 찾는다. 캐시에 없으면 요청을 보내고 기다려야 하는데 지금은 그 경로가 없다(`arp_send_request` + `virtio_net_wait`로 이어 붙이면 된다).
- **ARP 요청 실수신 검증 미룸**: 와이어로 들어오는 ARP 요청에 대한 reply는 QEMU user 네트워크가 게스트에게 요청을 먼저 보내지 않아 검증하지 못했다. 74에서 ICMP echo를 주고받을 때 상대가 우리 MAC을 모르면 ARP 요청을 보내므로 그때 실제 경로를 확인한다.
- **ARP 캐시 만료·재요청 없음**: 항목이 영원히 남고, 응답이 없으면 재시도하지 않는다. 필요해지면 만료 시각(`timer_ticks`)을 항목에 추가한다.
- **송신 슬롯 고갈 처리**: `virtio_net_tx_alloc`이 0을 돌려주면 호출자가 그냥 실패 처리한다. 대기하다 재시도하는 경로는 송신량이 늘어나는 UDP/TCP 단계에서 판단한다.
- **수신 처리 위치**: 수신한 프레임의 파싱과 reply 송신이 여전히 MSI-X 핸들러(인터럽트 컨텍스트) 안에서 일어난다. 콘솔이 재진입 불가라 핸들러 출력이 메인 흐름 출력과 섞일 수 있다. IP/ICMP 이후 수신 경로가 더 커지면 핸들러는 큐에 넣기만 하고 스레드에서 처리하는 구조를 판단한다.
- **게스트 IP 고정**: `10.0.2.15`를 `arp.c`에 하드코딩했다. IP 계층이 생기는 74에서 IP 설정의 자리를 판단한다.
