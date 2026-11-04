# 76 — udp

**목표**: IP 위에 UDP를 올린다. 75의 `ip_send`(블록하지 않고 접수만 책임지는 송신)와 수신 스레드(`net_rx`) 위에서, ① `ip_rx`가 protocol 17을 `udp_rx`로 넘기고 ② `udp_rx`가 목적지 포트로 고정 크기 테이블에서 핸들러를 찾아 데이터그램을 건네며(demux) ③ `udp_send`가 UDP 헤더를 붙여 `ip_send`에 protocol 17로 넘긴다. 소켓 syscall과 수신 버퍼(소켓)는 78 범위라 이번엔 없다 — 소켓이 없는 지금은 포트별 수신 함수를 등록하는 테이블이 그 자리를 대신한다.

## 1) 체크섬 분리 (`ip.c`)

- UDP 체크섬은 pseudo-header(12바이트) + UDP 헤더 + 데이터를 같이 더하는데 이 셋은 메모리에서 연속이 아니다. 75의 `ip_checksum(data, len)`은 연속 버퍼 하나만 받으므로 둘로 쪼갰다.
- `ip_checksum_add(sum, data, len)`: 16비트 단위로 더해 누적한 `u32` 합을 돌려준다. 접거나 반전하지 않는다. 홀수 길이면 마지막 바이트를 상위 바이트로 보고 0을 채워 더하므로, **홀수 길이 조각은 마지막 호출에서만** 써야 한다(UDP에서는 데이터가 마지막이라 성립).
- `ip_checksum_finish(sum)`: 캐리를 접고 비트를 반전해 최종 `u16`을 돌려준다.
- `ip_checksum(data, len)`은 둘을 한 번씩 부르는 형태로 남겼다. IP 헤더·ICMP 호출부는 그대로다.

## 2) UDP (`udp.c`)

- `udp_hdr`(출발지 포트, 목적지 포트, 길이, 체크섬, 각 2바이트 = 8바이트)와 `udp_pseudo_hdr`(출발지 IP, 목적지 IP, 0, protocol, UDP length = 12바이트)는 `udp.c` 안에 둔다(`icmp_echo_hdr`처럼 이 파일만 쓴다).
- `udp_checksum(src_ip, dst_ip, hdr, data, data_len)`: pseudo-header를 스택에 만들고 pseudo-header → UDP 헤더 → 데이터 순으로 `ip_checksum_add`한 뒤 `ip_checksum_finish`한 값을 돌려준다. 수신 검증과 송신 계산이 같은 함수다. 수신 때는 패킷의 체크섬 필드까지 같이 더하므로 결과가 0이면 정상이다.
- `udp_bind(port, handler)`: 고정 크기 테이블(`UDP_PORT_SLOTS` 4개)에 `port`/`handler` 쌍을 등록한다. 0 성공, -1은 이미 같은 포트가 등록됨 또는 테이블 가득 참. 핸들러 시그니처는 `(src_ip, src_port, data, len)`. 수신 스레드가 읽는 슬롯은 `port`/`handler`를 다 쓰고 컴파일러 배리어를 둔 뒤 `used`를 올려서 반쯤 채워진 슬롯을 보지 못하게 한다. 해시 테이블이 아니라 배열이고 선형 탐색이다(리눅스는 해시 테이블).
- `udp_rx(src_ip, packet, len)`: `ip_rx`가 부른다. 길이가 헤더보다 짧거나 UDP `length`가 IP 페이로드 길이를 넘으면 조용히 버린다. 체크섬이 0이면 "안 씀"이라 검증하지 않고(IPv4 규칙) 0이 아니면 검증해서 틀리면 `udp: rx bad checksum`으로 버린다. 포트가 테이블에 있으면 핸들러를 부르고 없으면 `udp: rx port N not bound, dropped`로 버린다. 데이터 길이는 IP 페이로드 길이가 아니라 UDP `length - 8`을 쓴다(이더넷 최소 프레임 패딩 대비).
- `udp_send(dst_ip, dst_port, src_port, data, len)`: UDP 헤더 + 데이터를 스택 버퍼에 이어 붙이고 체크섬을 계산해(`0`이면 `0xFFFF`로 바꿈) `ip_send`로 넘긴다. 리턴은 `ip_send`의 접수 의미를 그대로 따른다(0 접수, -1 호스트 안에서 바로 아는 실패). `len` 최대는 `UDP_MAX_PAYLOAD`(= 1480 − 8 = 1472). 단편화가 없어 이를 넘으면 -1.
- 핸들러는 수신 스레드 컨텍스트에서 돈다. 잠들면 안 되고 `kmalloc`도 쓰면 안 된다. `udp_send`는 `ip_send`가 블록하지 않으므로 핸들러 안에서 불러도 된다.

## 3) 테스트 (`udp.c`의 `udp_test`)

- **송수신 한 번에**: QEMU user 네트워크의 DNS(10.0.2.3 UDP 53)에 `example.com` A 질의(29바이트)를 `40053` 포트에서 보내고 응답이 핸들러로 올라오는지 본다. DNS 메시지는 해석하지 않고 도착 여부와 크기만 확인한다. 응답 크기는 호스트의 DNS 환경에 따라 달라질 수 있다.
- **수신 경로 주입**: 75처럼 이더넷 프레임을 만들어 `net_rx_enqueue`에 넣는다(`ethernet_rx`부터 전부 실제 코드). 출발지 `10.0.2.99:5000`(ARP 캐시에 있음), 에코 포트 7에 등록한 핸들러가 받은 데이터를 그대로 돌려보낸다.
  - 올바른 체크섬 → 핸들러 호출, 에코 응답 송신
  - 체크섬 0 → 검증 없이 핸들러 호출, 에코 응답 송신
  - 체크섬 틀림 → `bad checksum`으로 폐기
  - 등록 안 된 포트 9999 → `not bound`로 폐기

## 검증

`make clean && make run-nogui`(관련 구간):

```
udp: tx 10.0.2.15:40053 -> 10.0.2.3:53 (29 bytes)
udp: dns rx from 10.0.2.3:53 (61 bytes)
udp: echo rx from 10.0.2.99:5000 (9 bytes)
udp: tx 10.0.2.15:7 -> 10.0.2.99:5000 (9 bytes)
udp: echo rx from 10.0.2.99:5000 (9 bytes)
udp: tx 10.0.2.15:7 -> 10.0.2.99:5000 (9 bytes)
udp: rx bad checksum from 10.0.2.99:5000
udp: rx port 9999 not bound, dropped (from 10.0.2.99:5000)
virtio-net: tx used idx=11 rx used idx=6
virtio-net: msix interrupts rx=6 tx=11
net_rx: dropped=0
```

`grub-file --is-x86-multiboot2` exit 0. 새로 만든 `udp.c`와 수정한 `ip.c`는 경고 없음(`kernel.c`의 `mkdir` 필드 초기화 경고는 이전 단계부터 있던 것). NVMe/ext2/셸 출력은 75와 같다.

**와이어 바이트 검증**: `-object filter-dump`로 pcap을 떠서 파이썬으로 해석했다(일회성 확인, Makefile에 없음). 와이어에 나간 UDP 4개(DNS 질의 29바이트 데이터, DNS 응답, 에코 응답 2개(9바이트, 홀수 길이))에서 pseudo-header를 붙여 재계산한 UDP 체크섬이 모두 0이고 IP 헤더 체크섬도 0이다. 홀수 길이 데이터에서도 0이 나오므로 분리한 `ip_checksum_add`의 패딩 처리가 맞다.

**검증 방법의 한계**: 테이블 가득 참/중복 bind(`udp_bind` -1), `UDP_MAX_PAYLOAD` 초과 송신(-1), UDP `length`가 IP 페이로드보다 큰 패킷 폐기, DNS 응답이 안 오는 타임아웃(`udp: no dns reply`) 경로는 실행되지 않았다. 계산 결과가 0이라 `0xFFFF`로 바꾸는 경로도 실제로 걸린 적은 없다.

### 기존 서브시스템과의 정합 확인

- **수신 스레드 스택**: 핸들러가 수신 스레드에서 돌고 에코 핸들러가 그 안에서 `udp_send`(스택 버퍼 약 1480바이트) → `ip_output`(약 1500바이트)을 부른다. 스레드 스택이 16KB(`THREAD_STACK_SIZE`)이고 ICMP 에코 reply 경로(약 1488 + 1500바이트)가 이미 같은 깊이라 여유가 있다.
- **kheap 무잠금**: 포트 테이블은 정적 배열이고 핸들러·`udp_rx`·`udp_send` 어디에서도 `kmalloc`을 부르지 않는다.
- **`ip_send` 블록 금지**: 에코 응답처럼 수신 스레드에서 UDP를 보내는 경로가 생겼다. 75에서 `ip_send`를 블록하지 않게 만들어 둔 덕에 ARP 캐시 miss여도 수신 스레드가 자기가 처리할 ARP reply를 기다리다 멈추지 않는다. `arp_resolve`/`icmp_ping`처럼 잠드는 함수는 핸들러에서 부르지 않는다.
- **`ip_checksum` 호출부**: IP 헤더, ICMP(`icmp_echo_reply`, `icmp_ping`, 주입 테스트)는 시그니처가 같은 `ip_checksum`을 그대로 쓴다.

## 이전 단계(75) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/udp.h` | 신규 | `udp_handler_t`, `udp_bind`, `udp_rx`, `udp_send`, `udp_test` 선언과 `UDP_HDR_LEN`/`UDP_MAX_PAYLOAD` |
| `boot/udp.c` | 신규 | UDP 헤더/pseudo-header, 체크섬, 포트 테이블 demux, 송신, DNS 질의·프레임 주입 테스트 |
| `boot/ip.h` | 수정 | `ip_checksum_add`, `ip_checksum_finish` 추가 |
| `boot/ip.c` | 수정 | `ip_checksum`을 누적/마무리 둘로 분리, `ip_rx`에 protocol 17 → `udp_rx` 분기 |
| `boot/kernel.c` | 수정 | `icmp_test()` 뒤에 `udp_test()` 호출 |
| `Makefile` | 수정 | `udp.o` 규칙과 링크 대상, 의존 헤더 추가 |
| 나머지 전부 | 변경 없음 | 75의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 위 검증 출력(DNS 응답 수신, 에코 핸들러 2회와 응답 송신, `bad checksum`, `not bound`, `net_rx: dropped=0`)이 나오고 NVMe/ext2/셸 출력이 75와 같아야 한다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **77 `77-tcp`**: TCP 체크섬도 pseudo-header를 쓰되 protocol이 6이고 길이 필드가 TCP 세그먼트 길이다. `udp_checksum`의 pseudo-header 조립이 UDP 전용이므로, TCP를 붙일 때 protocol과 길이를 받는 `ip` 쪽 공용 함수로 올릴지 정한다.
- **78 `78-socket-syscall`**: 지금 포트 테이블은 `port → 핸들러`다. 소켓이 생기면 `port → 소켓`(수신 큐를 가진 버퍼)이 되고, 핸들러가 소켓 큐에 넣은 뒤 프로세스를 깨우는 구조로 바뀐다. `udp_unbind`가 아직 없다(소켓 `close`에서 필요).
- **ICMP port unreachable 없음**: 바인드 안 된 포트로 온 데이터그램을 조용히 버린다. 리눅스는 ICMP destination unreachable(port unreachable)을 돌려준다. ICMP 에러 송신은 아직 없다.
- **임시 포트 할당 없음**: 출발지 포트를 호출자가 직접 정한다. 리눅스는 `bind` 없이 보내면 ephemeral 포트를 자동으로 고른다.
- **IP 단편화 미지원**: `UDP_MAX_PAYLOAD`가 1472로 고정이다. 74 힌트의 단편화 미지원이 그대로 UDP 크기 제한이 된다.
- **수신 버퍼 없음**: 핸들러가 수신 스레드에서 즉시 처리해야 한다. 느린 핸들러는 수신 큐 전체를 막는다.
- **라우팅 없음, 게스트 IP 고정, IP id 경합**: 74/75의 힌트 그대로다.
