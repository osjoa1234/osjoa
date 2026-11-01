# 75 — net-rx-queue

**목표**: 74의 `ip_send`/`ip_send_cached` 이중 구조를 없앤다. 두 함수가 갈라진 이유는 MSI-X 핸들러 안에서 `ethernet_rx` 이후 전부를 돌렸기 때문(핸들러에서는 `arp_resolve`로 잠들 수 없음)이었다. 리눅스처럼 ① 핸들러는 프레임을 링 큐에 넣고 수신 스레드를 깨우기만 하고(hardirq→softirq 분리), ② MAC을 모르는 송신 패킷은 대기 슬롯에 맡겨 두고 ARP 요청만 보낸 뒤 reply가 오면 내보낸다(neighbour `arp_queue`). UDP/TCP는 이번 범위가 아니다.

## 1) 수신 링 큐와 수신 스레드 (`net_rx.c`)

- `net_rx_enqueue(frame, len)`: 이더넷 프레임(이더넷 헤더부터)을 링 슬롯에 복사하고 `g_head`를 올린 뒤 수신 스레드를 `thread_unpark`. 슬롯 16개, 슬롯당 `ETH_HDR_LEN + ETH_MTU`(14+1500=1514)바이트. 가득 차거나 프레임이 더 길면 `g_dropped`만 올리고 버린다. 0/-1.
- 생산자는 MSI-X 핸들러 하나, 소비자는 수신 스레드 하나(SPSC)라서 `g_head`는 생산자만, `g_tail`은 소비자만 쓴다. 따라서 락이 없다. 데이터를 쓴 뒤 컴파일러 배리어를 두고 `g_head`를 올린다.
- 수신 스레드: `interrupts_disable` 상태에서 큐가 비었는지 확인하고 비었으면 `thread_park`. 확인과 park 사이에 인터럽트가 끼지 못하므로 wakeup을 놓치지 않는다(`arp_resolve`/`icmp_ping`의 대기 루프와 같은 패턴). 슬롯을 `ethernet_rx`로 처리한 뒤에야 `g_tail`을 올려서, 처리 중에는 그 슬롯이 덮어써지지 않는다.
- `virtio_handle_rx_frame`이 `ethernet_rx` 대신 `net_rx_enqueue`를 부른다. RX 버퍼는 `net_rx_enqueue`가 복사한 즉시 디스크립터에 되돌려진다.
- `net_rx_init()`은 `threads_init` 이후(`kernel.c`에서 `pipe_init()` 뒤)에 스레드를 만든다. 그 전에 도착한 프레임은 큐에 쌓였다가 스레드가 뜨면 처리된다.

## 2) ARP 대기 큐와 `ip_send` 통합 (`ip.c`)

- `ip_send(dst_ip, protocol, payload, len)`: 이제 블록하지 않는다. ARP 캐시에 있으면 바로 `ip_output`. 없으면 IP 페이로드를 대기 슬롯(4개, 슬롯당 `IP_MAX_PAYLOAD` 1480바이트 고정 배열)에 맡기고 같은 목적지로 이미 요청 중이 아니면 ARP 요청을 보낸 뒤 0을 돌려준다. 슬롯이 없으면 -1. `ip_send_cached`는 삭제. ARP 요청 전송이 실패하면(송신 슬롯 없음) 방금 넣은 대기 슬롯을 비우고 -1을 돌려준다. 슬롯을 남긴 채 -1을 주면 호출한 쪽은 실패로 아는데 패킷이 큐에 남고, 같은 IP의 다음 `ip_send`가 요청이 나간 줄 알고 ARP 요청을 건너뛴다. 요청 전송은 슬롯을 채운 `cli` 구간 안에서 한다(구간 밖이면 그 사이 다른 `ip_send`가 이 슬롯을 보고 요청이 나갔다고 판단할 수 있다).
- `ip_send`의 리턴 의미: 0은 "보냈다"가 아니라 "접수했다"(TX 큐에 넣었거나 ARP 대기 큐에 맡김)이고, -1은 페이로드 초과·송신 슬롯 부족·대기 큐 가득 참처럼 호스트 안에서 바로 알 수 있는 실패뿐이다. 접수 뒤의 만료·유실은 알리지 않는다. IP가 전달을 보장하지 않는 프로토콜이라 도착 확인은 위 계층(ICMP는 reply 대기, TCP는 ACK 재전송)이 한다.
- 슬롯 확인·캐시 재확인·슬롯 채우기는 한 번의 `cli` 구간에서 한다. 캐시 확인 직후 ARP reply가 처리돼 ARP 대기 큐를 비우고 가면, 뒤늦게 슬롯에 넣은 패킷이 아무도 안 깨우는 채로 남기 때문이다.
- `ip_arp_resolved(ip, mac)`: `arp_rx`가 `cache_update` 직후 부른다(reply와 request 둘 다 MAC을 배우므로). 그 IP로 가는 대기 슬롯을 하나씩 스택으로 꺼내(`cli` 구간) 슬롯을 비운 뒤 구간 밖에서 `ip_output`으로 보낸다. 전송 중에 인터럽트를 막지 않는다.
- 대기 패킷 만료: `IP_ARP_WAIT_MAX_MS`(1000ms)가 지난 슬롯은 다음 `ip_send` 때 로그(`ip: arp wait packet to ... expired`)와 함께 비운다. 타이머가 따로 없어 만료는 새 송신이 올 때만 정리된다.
- `arp_resolve`는 `arp_test`가 직접 쓰므로 남긴다. `ip_send`는 더 이상 부르지 않는다. 이전 `IP_ARP_WAIT_MS`는 삭제.
- `icmp_echo_reply`는 `ip_send`를 쓴다. 캐시에 상대 MAC이 없을 때 reply를 버리던 경로(`icmp: tx echo reply dropped`)가 사라졌다.

## 3) 테스트 주입 (`icmp.c`)

- 74는 `ip_rx`에 IP 패킷을 직접 넘겼다. 75는 이더넷 프레임을 만들어 `net_rx_enqueue`에 넣는다(`ethernet_rx`부터 전부 실제 코드, virtio 수신 구간만 우회).
- 요청 1: 출발지 `10.0.2.99`(ARP 캐시에 있음, `arp_test`가 넣어 둠) → 캐시 hit 경로.
- 요청 2: 출발지 `10.0.2.3`(QEMU slirp DNS, 캐시에 없음, slirp가 ARP에 응답함) → 캐시 miss 경로. reply가 대기 큐에 들어가고 ARP 요청이 나가며, slirp의 ARP reply가 오면 대기 패킷이 나간다.

## 검증

`make clean && make run-nogui`(관련 구간):

```
net_rx: ring 16 slots, rx thread started
arp: tx request who-has 10.0.2.2 tell 10.0.2.15
...
icmp: rx echo request from 10.0.2.99 id=0x0099 seq=1 (64 bytes)
icmp: tx echo reply to 10.0.2.99 id=0x0099 seq=1
icmp: rx echo request from 10.0.2.3 id=0x0003 seq=1 (64 bytes)
icmp: tx echo reply to 10.0.2.3 id=0x0003 seq=1
arp: tx request who-has 10.0.2.3 tell 10.0.2.15
arp: rx reply 10.0.2.3 is-at 52:55:0A:00:02:03
arp: cache 10.0.2.3 -> 52:55:0A:00:02:03
ip: arp wait packet to 10.0.2.3 released
virtio-net: tx used idx=8 rx used idx=5
virtio-net: msix interrupts rx=5 tx=8
net_rx: dropped=0
```

반복 실행 출력 동일, `grub-file --is-x86-multiboot2` exit 0, 새 파일과 수정 파일 컴파일 경고 없음. NVMe/ext2/셸 출력은 74와 같고, 달라진 건 `phys mem` 남은 페이지 수(정적 링·ARP 대기 큐 약 30KB, 7페이지 만큼)뿐이다.

**와이어 바이트 검증**: `-object filter-dump`로 pcap을 떠서 파이썬으로 해석했다(일회성 확인, Makefile에 없음). 순서는 ARP 3개(요청·reply·우리가 보낸 reply) → ping 3쌍 → `10.0.2.99`행 echo reply(MAC `02:00:00:00:00:99`) → ARP 요청(`who-has 10.0.2.3`) → slirp reply → `10.0.2.3`행 echo reply(MAC `52:55:0a:00:02:03`). IP 헤더·ICMP 체크섬 재계산 결과 모두 0, TTL 64. 대기 패킷이 ARP reply 이후에야 와이어에 나가는 것을 확인했다.

**검증 방법의 한계**: 링 큐 가득 참(`net_rx: dropped`), ARP 대기 큐 가득 참(`ip: arp wait queue full`), 대기 만료(`expired`), ARP 요청 전송 실패 시 슬롯 회수 경로는 실행되지 않았다.

### 기존 서브시스템과의 정합 확인

- **송신 경로 동시성**: 이제 `ethernet_send`를 메인 스레드(ping, ARP 요청)와 수신 스레드(reply, 대기 패킷 방출, ARP reply)가 선점 속에서 동시에 부른다. `virtio_net_tx_alloc`/`tx_submit`이 각각 `cli` 구간이고 버퍼가 호출마다 다르므로 문제없다. 74의 "핸들러가 스레드를 끼어든다"와 같은 구도였고 달라지지 않았다.
- **`thread_unpark`**: 74는 핸들러가, 75는 수신 스레드가 `g_ping_waiter`/`g_arp_waiter`를 깨운다. 단일 CPU에서 대기 쪽이 `cli` 확인 → waiter 등록 → `thread_sleep`을 한 구간으로 하므로 wakeup을 놓치지 않는다.
- **수신 스레드가 블록하는 경우**: 수신 스레드에서 `ip_send`가 잠들면 자기가 처리할 ARP reply를 기다리는 데드락이 된다. 그래서 `ip_send`를 블록하지 않게 만든 것이 이 단계의 핵심이다. `arp_resolve`는 수신 스레드에서 부르면 안 된다(현재 호출자는 메인 스레드의 `arp_test`뿐).
- **kheap 무잠금**: 링 큐와 ARP 대기 큐는 정적 배열이다(링 약 24KB, ARP 대기 큐 약 6KB). 핸들러·스레드 어디에서도 `kmalloc`을 부르지 않는다.
- **ARP 캐시**: `cache_store`는 기존처럼 `cli` 구간 안이다. `ip_arp_resolved`는 그 구간 밖에서 부른다.

## 이전 단계(74) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/net_rx.h` | 신규 | `net_rx_init`, `net_rx_enqueue`, `net_rx_dropped` 선언 |
| `boot/net_rx.c` | 신규 | 프레임 링 큐(16슬롯)와 수신 스레드 |
| `boot/virtio_net.c` | 수정 | 수신 프레임을 `ethernet_rx` 대신 `net_rx_enqueue`로 넘김, 통계에 `net_rx: dropped` 추가 |
| `boot/ip.h` | 수정 | `ip_send_cached` 삭제, `ip_arp_resolved` 추가 |
| `boot/ip.c` | 수정 | ARP 대기 큐, 블록하지 않는 `ip_send`, `ip_arp_resolved` |
| `boot/arp.c` | 수정 | `arp_rx`가 캐시 갱신 뒤 `ip_arp_resolved` 호출 |
| `boot/icmp.c` | 수정 | `ip_send_cached` → `ip_send`, 테스트가 이더넷 프레임을 링 큐로 주입하고 캐시 miss 케이스(`10.0.2.3`) 추가 |
| `boot/kernel.c` | 수정 | `net_rx_init()` 호출 |
| `Makefile` | 수정 | `net_rx.o` 규칙과 링크 대상, 의존 헤더 추가 |
| 나머지 전부 | 변경 없음 | 74의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 위 검증 출력(캐시 hit reply, 캐시 miss reply가 ARP 교환 뒤 `released`, `tx used idx=8`, `msix interrupts rx=5 tx=8`, `net_rx: dropped=0`)이 나오고 NVMe/ext2/셸 출력이 74와 같아야 한다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **76 `76-udp`**: `ip_rx`의 protocol 분기에 17(UDP)을 추가한다. UDP 체크섬은 IP 헤더의 src/dst/protocol/길이를 섞은 pseudo-header를 쓰므로 `ip_checksum`을 쪼개 쓸 수 있게 바꿔야 한다. 수신 스레드 컨텍스트라 UDP 수신 큐에 `kmalloc` 없이 고정 배열을 쓰는 제약은 이어진다.
- **ARP 재전송 없음**: 요청 1번이 유실되면 대기 패킷은 1초 뒤 새 송신이 올 때 정리될 뿐 요청을 다시 보내지 않는다. 리눅스는 neighbour 타이머로 재전송한다.
- **대기 만료 타이머 없음**: 만료 정리가 새 `ip_send`에 얹혀 있다. 송신이 없으면 슬롯이 남는다.
- **링 큐 오버플로 정책**: 가득 차면 새 프레임을 버린다(tail drop). 리눅스는 backlog 한도와 drop 통계를 둔다.
- **`arp_resolve`는 수신 스레드 금지**: 호출자가 늘면 막을 장치가 없다.
- **라우팅 없음, ICMP 에러 없음, 단편화 미지원, IP id 경합, 게스트 IP 고정**: 74의 힌트 그대로다. IP id 경합은 수신 스레드와 메인 스레드가 동시에 보낼 수 있게 되어 실제로 가능해졌다(DF라 영향은 없음).
