# 74 — ip-icmp

**목표**: 73의 이더넷/ARP 위에 IPv4 계층과 ICMP echo를 올려 QEMU user 게이트웨이(10.0.2.2)에 ping을 보내 reply를 받고, 들어오는 echo request에 reply를 보낸다. `ethernet_rx`에 ethertype `0x0800` 분기를 추가하고, 상대 MAC은 73의 `arp_resolve`로 얻는다. 단편화·옵션·라우팅·TCP/UDP는 이번 범위가 아니다.

## 1) 계층과 의존 방향

- 송신: `icmp → ip → ethernet → virtio_net`, 수신: `virtio_net → ethernet → ip → icmp`. 73과 같은 비대칭(송신은 위가 아래를 부르고 수신은 아래가 위를 부른다)이 `ethernet ↔ ip`, `ip ↔ icmp`에도 생긴다.
- `ip.[ch]`: `struct ip_hdr`(20바이트), 체크섬, `ip_rx`, `ip_send`, `ip_send_cached`. `ip_local`(10.0.2.15)/`ip_gateway`(10.0.2.2)도 여기로 옮겼다. 73의 힌트 "IP 설정의 자리"의 답이다. `arp.c`가 갖던 `ip_equal`/`print_ip`와 `g_local_ip`/`g_gateway_ip`는 `ip.c`로 가서 `arp.c`는 `ip.h`를 include해 쓴다.
- `icmp.[ch]`: `struct icmp_echo_hdr`(8바이트), `icmp_rx`, `icmp_ping`, `icmp_test`.

## 2) IP (`ip.c`)

- `ip_checksum(data, len)`: 16비트 단위(big endian)로 더하고 넘친 윗부분을 아래에 더한 뒤 반전한 값을 돌려준다. 체크섬 필드를 0으로 두고 계산하면 채울 값이고, 필드까지 포함해 계산하면 정상일 때 0이다. IP 헤더와 ICMP 메시지가 같이 쓴다. 결과는 호스트 순서이므로 필드에 넣을 땐 `htons`.
- `ip_rx(packet, len)`: 버리는 조건은 `len < 20`, version이 4가 아님, IHL×4가 20 미만이거나 `len`보다 큼, `total_len`이 헤더 길이보다 작거나 `len`보다 큼, 헤더 체크섬 불일치(로그 출력), MF 또는 fragment offset이 0이 아님(단편화 미지원), 목적지가 `ip_local`이 아님. 통과하면 protocol이 1이면 `icmp_rx`에 헤더 뒤 `total_len - IHL×4`바이트만 넘긴다(이더넷 패딩 제거는 `total_len` 기준). 그 외 protocol은 로그만 찍는다.
- `ip_send(dst_ip, protocol, payload, len)`: `arp_resolve`(최대 1초)로 MAC을 얻어 보낸다. 대기하므로 스레드 컨텍스트 전용. 0/-1.
- `ip_send_cached(...)`: `arp_cache_lookup`만 본다. 캐시에 없으면 -1이고 기다리지 않는다. MSI-X 핸들러 안(echo reply 송신)에서 쓴다.
- 헤더 필드: version_ihl `0x45`, TTL 64(리눅스 기본값), flags_frag `0x4000`(DF), id는 호출마다 1씩 증가. 목적지가 같은 서브넷인지 판정해 게이트웨이로 보내는 라우팅은 없고 next hop은 항상 목적지 IP다.
- `ip_output`은 `IP_HDR_LEN + IP_MAX_PAYLOAD`(1500)바이트 버퍼를 스택에 잡고 헤더+페이로드를 조립해 `ethernet_send`로 넘긴다. `ethernet_send`가 한 번 더 복사하므로 프레임이 두 번 복사된다.

## 3) ICMP (`icmp.c`)

- `icmp_rx(src_ip, ttl, packet, len)`: `len < 8`이면 버림, ICMP 체크섬 전체(메시지 전체 길이) 검증. type 8/code 0이면 echo request → `icmp_echo_reply`. type 0/code 0이면 echo reply → 로그를 찍고 identifier가 `ICMP_ECHO_ID`이고 sequence가 `g_ping_seq`와 같으면 `g_ping_done`을 세우고 `g_ping_waiter`를 `thread_unpark`.
- `icmp_echo_reply`: 받은 메시지를 스택 버퍼에 복사하고 type만 0으로 바꾼 뒤 체크섬을 다시 계산해 `ip_send_cached`로 보낸다. IP 계층의 src/dst 교환은 `ip_output`이 `ip_local`/`dst_ip`로 조립하므로 별도 swap이 없다. 캐시에 상대 MAC이 없으면 reply를 버리고 로그(`icmp: tx echo reply dropped`)를 찍는다. MSI-X 핸들러에서 `arp_resolve`로 잠들 수 없어서다.
- `icmp_ping(dst_ip, seq, ms)`: identifier `0x4F53`, 데이터 56바이트(`0..55`)로 echo request를 만들어 `ip_send`로 보내고, `g_ping_done`이 서거나 `ms`가 지날 때까지 기다린다. 대기 루프는 `arp_resolve`와 같은 구조(IF 저장 → `cli` → 조건 확인 → `g_ping_waiter` 등록 → `thread_sleep`). 0이면 reply 수신, -1이면 송신 실패 또는 시간 초과. 마감은 `ip_send`(ARP 대기 포함) 시작 *전에* 잡는다.
- `icmp_test`: 게이트웨이에 ping 3회. 이어서 `10.0.2.99`가 보낸 것으로 꾸민 echo request IP 패킷을 `ip_rx`에 직접 넘겨 수신 → reply 경로를 시험한다(73 `arp_test`의 주입 방식과 같다). 이 시점에 `10.0.2.99`의 MAC이 `arp_test`로 캐시에 들어 있어 reply가 나간다.

## 검증

`make clean && make run-nogui`(관련 구간):

```
icmp: tx echo request to 10.0.2.2 id=0x4F53 seq=1
icmp: rx echo reply from 10.0.2.2 id=0x4F53 seq=1 ttl=255 (64 bytes)
icmp: ping 10.0.2.2 seq=1 ok
... (seq=2, 3 동일)
icmp: rx echo request from 10.0.2.99 id=0x0099 seq=1 (64 bytes)
icmp: tx echo reply to 10.0.2.99 id=0x0099 seq=1
virtio-net: tx used idx=6 rx used idx=4
virtio-net: msix interrupts rx=4 tx=6
```

반복 실행 출력 동일, `grub-file --is-x86-multiboot2` exit 0, 새 파일과 수정 파일 컴파일 경고 없음. NVMe/ext2/셸 출력은 73과 같다.

**와이어 바이트 검증**: `-object filter-dump,id=f0,netdev=n0,file=...`로 pcap을 떠서 파이썬으로 프레임을 해석했다. 커널이 보낸 echo request 3개와 `10.0.2.99`행 echo reply 1개 모두 이더넷 98바이트, IP 헤더 체크섬 재계산 결과 0, ICMP 체크섬 재계산 결과 0, TTL 64, protocol 1이었고 reply는 type 0이었다. 73에서 못 본 "reply가 실제 와이어에 올바른 바이트로 나가는가"를 이 방법으로 확인했다. 게이트웨이의 reply는 TTL 255(slirp 값)다. pcap 확인은 Makefile에 넣지 않은 일회성 확인이다.

**검증 방법의 한계**: 들어오는 echo request 경로는 slirp가 게스트로 ping을 보낼 수 없어 주입으로만 확인했다(`ip_rx` 이후 전부 실제 코드, 이더넷·virtio 수신 구간만 우회). 시간 초과 경로(`icmp: no echo reply`)와 `icmp: tx echo reply dropped`, IP 체크섬 불일치·단편화·비-ICMP protocol 버림 경로는 실행되지 않았다.

### 기존 서브시스템과의 정합 확인

- **MSI-X 핸들러 컨텍스트**: `ip_rx → icmp_rx → icmp_echo_reply → ip_send_cached`가 핸들러 안에서 돈다. 잠드는 `arp_resolve`를 피했고, 스택 버퍼는 `ip_output` 1500 + `icmp_echo_reply` 1488바이트로 스레드 스택(16KB)에 들어간다.
- **송신 슬롯**: ping 3 + reply 1 + ARP 요청 1 + ARP reply 1로 tx 6번, 슬롯 16개 안에서 회수되며 `tx used idx=6`.
- **대기자 슬롯**: `g_arp_waiter`와 `g_ping_waiter`는 별개다. `icmp_ping`이 `ip_send` 안에서 ARP 대기를 하고 나서 ICMP 대기를 하므로 순차이고 겹치지 않는다.
- **ARP 캐시**: 73의 `arp_rx`가 요청의 sender를 캐시하기 때문에 상대가 먼저 ARP로 우리를 찾으면 `ip_send_cached`가 성공한다.

## 이전 단계(73) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/ip.h` | 신규 | `struct ip_hdr`, `IP_*` 상수, `ip_local`/`ip_gateway`, 체크섬·수신·송신 선언 |
| `boot/ip.c` | 신규 | IP 헤더 검증·조립, 인터넷 체크섬, `ip_rx`, `ip_send`/`ip_send_cached`, `ip_equal`/`ip_print` |
| `boot/icmp.h` | 신규 | `icmp_rx`, `icmp_ping`, `icmp_test` 선언 |
| `boot/icmp.c` | 신규 | `struct icmp_echo_hdr`, echo request 응답, `icmp_ping` 대기, 게이트웨이 ping + 요청 주입 테스트 |
| `boot/ethernet.h` | 수정 | `ETHERTYPE_IPV4` 추가 |
| `boot/ethernet.c` | 수정 | `ethernet_rx`에 IPv4 분기(`ip_rx`) 추가 |
| `boot/arp.c` | 수정 | IP 주소 상수와 `ip_equal`/`print_ip`를 `ip.h`의 것으로 교체 |
| `boot/kernel.c` | 수정 | `arp_test()` 뒤에 `icmp_test()` 호출 |
| `Makefile` | 수정 | `ip.o`, `icmp.o` 규칙과 링크 대상, 의존 헤더 추가 |
| 나머지 전부 | 변경 없음 | 73의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 위 검증 출력(ping 3회 `ok`, 주입 echo request → `icmp: tx echo reply`, `tx used idx=6`, `msix interrupts rx=4 tx=6`)이 나오고 NVMe/ext2/셸 출력이 73과 같아야 한다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **75 `75-net-rx-queue`**: 수신을 인터럽트 핸들러 밖(스레드)으로 넘기는 링 큐와 ARP 보류 큐를 도입하고 `ip_send`/`ip_send_cached`를 하나로 통합한다. 캐시 miss 경로를 pcap으로 검증한다.
- **76 `76-udp`**: `ip_rx`의 protocol 분기에 17(UDP)을 추가한다. UDP 체크섬은 IP 헤더의 src/dst/protocol/길이를 섞은 pseudo-header를 쓰므로 `ip_checksum`을 쪼개 쓸 수 있게 바꿔야 한다.
- **라우팅 없음**: next hop이 항상 목적지 IP다. 서브넷 밖(예: 8.8.8.8)으로 보내려면 게이트웨이 MAC을 next hop으로 쓰는 판정(넷마스크 비교)이 필요하다. 지금은 게이트웨이에만 ping해서 필요가 없었다.
- **ICMP 에러 메시지 없음**: TTL 만료(time exceeded)·목적지 도달 불가를 만들지도 받지도 않는다. 호스트만 있고 포워딩이 없어 TTL을 줄이는 곳도 없다.
- **단편화 미지원**: MF/offset이 있는 패킷은 버리고 보낼 때도 조각내지 않는다(`IP_MAX_PAYLOAD` 초과는 -1).
- **IP id 경합**: `g_ip_id`는 잠금 없이 증가한다. 핸들러와 스레드가 동시에 보내면 같은 id가 나올 수 있으나 DF 패킷이라 지금은 영향이 없다.
- **echo reply 드롭**: 상대 MAC이 캐시에 없으면 reply를 버린다. 핸들러에서 대기할 수 없어서이며, 75에서 수신을 스레드로 넘기면 `ip_send`로 바꿀 수 있다.
- **게스트 IP 고정**: `ip_local`은 상수다. DHCP 같은 동적 설정은 범위 밖.
