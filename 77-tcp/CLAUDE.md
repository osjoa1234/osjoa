# 77 — tcp

**목표**: IP 위에 TCP의 연결 수립(3-way handshake)까지 올린다. ① `ip_rx`가 protocol 6을 `tcp_rx`로 넘기고 ② TCP 헤더 파싱·체크섬 검증 ③ TCB(`tcp_conn`)와 고정 크기 연결 테이블에서 4 tuple로 연결을 찾고(없으면 별도의 listener 테이블에서 LISTEN 중인 포트를 찾고) ④ 서버(수동 open: `LISTEN → SYN_RECEIVED → ESTABLISHED`)와 클라이언트(능동 open: `SYN_SENT → ESTABLISHED`) 양쪽 상태 전이를 구현한다. 연결이 없는 포트로 온 세그먼트에는 RST로 답한다. ESTABLISHED에 들어간 뒤의 데이터·ack·window는 78, 재전송·순서 복구는 79, FIN 종료는 80 범위라 이번엔 없다 — ESTABLISHED에서 받은 세그먼트는 RST만 처리하고 나머지는 로그만 찍고 버린다. 소켓은 81 범위라, UDP 때처럼 소켓이 없는 지금은 연결이 수립되면 부르는 콜백(`tcp_established_t`)이 그 자리를 대신한다.

## 1) 의사 헤더 공용화 (`ip.c`)

- 76 힌트대로 TCP와 UDP가 쓰는 의사 헤더(출발지 IP, 목적지 IP, 0, protocol, 길이)를 `ip_pseudo_sum(src_ip, dst_ip, protocol, length)` 하나로 올렸다. 리눅스가 `csum_tcpudp_magic`을 TCP/UDP 공용으로 쓰는 것과 같은 모양이다.
- 의사 헤더는 구조체를 만들지 않고 16비트 워드를 바로 더한다: 출발지 IP 2워드 + 목적지 IP 2워드 + `0:protocol` 1워드(= protocol 값) + 길이 1워드. 접거나 반전하지 않은 `u32` 누적 합을 돌려주고, 호출부가 이어서 `ip_checksum_add`(헤더, 데이터)와 `ip_checksum_finish`를 부른다.
- 길이는 UDP에선 UDP `length`, TCP에선 TCP 헤더+데이터 길이다(TCP 헤더에는 length 필드가 없어서 IP 페이로드 길이에서 나온다).
- `udp.c`의 `udp_pseudo_hdr`는 지우고 `udp_checksum`이 `ip_pseudo_sum`을 쓰게 했다. 동작은 76과 같다.
- `ethernet.h`에 `htonl`/`ntohl`을 `htons` 옆에 추가했다(seq/ack가 32비트).

## 2) TCP (`tcp.c`)

- `tcp_hdr`는 20바이트(포트 2개, seq, ack, `data_offset`(상위 4비트 = 4바이트 단위 헤더 길이), `flags`, `window`, `checksum`, `urgent_ptr`). 옵션은 구조체 뒤에 이어진다.
- **TCB(`tcp_conn`)**: `state`, `local_port`, `remote_port`, `remote_ip`, `snd_una`(ack 못 받은 가장 오래된 seq), `snd_nxt`(다음에 보낼 seq), `rcv_nxt`(다음에 받길 기대하는 seq = 보낼 ack), `peer_mss`, `on_established`. 이번 단계는 SYN 하나(seq 1 소비)만 오가므로 `snd_nxt = ISN + 1`로 시작한다. 송수신 버퍼는 78에서 붙는다.
- **연결 테이블 `g_conns`**: `TCP_CONN_SLOTS`(6) 고정 배열. TCB만 들어가고 `state == CLOSED`가 빈 칸이다. 키는 4 tuple이라 LISTEN 상태가 없다.
- **listener 테이블 `g_listeners`**: `TCP_LISTEN_SLOTS`(4) 고정 배열. 칸은 `tcp_listener`(`active`, `port`, `on_established`)이고 TCB가 아니다 — 상대가 정해지지 않은 "이 포트로 오는 SYN을 받겠다"는 등록이다. 맺어진 연결은 `g_conns`에 따로 생기고 listener 칸은 그대로 남아 다음 SYN을 받는다. 두 테이블은 리눅스의 established 해시(`ehash`)와 listening 해시(`lhash2`)에 해당하고, 둘 다 해시가 아니라 선형 탐색이다. 리눅스의 bind 해시(`bhash`, 포트 점유 기록)는 `bind()`가 생기는 81에서 필요해진다 — 지금 포트 충돌 검사는 listener 테이블의 `tcp_listener_open`이 한다.
- `tcp_conn_open(state, local_port, remote_ip, remote_port, handler)`: 빈 칸 탐색, 같은 4 tuple 중복 검사, 필드 채우기를 **`cli` 안에서 한 번에** 한다. 연결 생성 경로가 두 곳이기 때문이다 — 테스트 스레드(`tcp_connect`)와 수신 스레드(listen 중인 포트에 SYN이 왔을 때 자식 TCB 생성). 둘이 같은 빈 칸을 동시에 고르지 못하게 한다. 필드를 다 쓰고 컴파일러 배리어를 둔 뒤 `state`를 마지막에 올려서 반쯤 채워진 칸을 수신 스레드가 보지 못한다. 0이면 가득 참 또는 중복, 아니면 TCB 포인터.
- `tcp_conn_find(seg)`: 4 tuple이 정확히 맞는 연결을 돌려주고 없으면 0이다. LISTEN 판단은 하지 않는다.
- `tcp_listener_open(port, handler)` / `tcp_listener_find(port)`: listener 테이블에 칸을 만들고(같은 포트가 이미 있으면 0, `cli` 안에서 빈 칸 탐색·중복 검사·필드 채우기, `active`를 마지막에 올림) 포트로 찾는다. `tcp_rx`는 `tcp_conn_find`가 0일 때만 `tcp_listener_find`를 부르고, 둘 다 0이면 `tcp_rx_closed`다. 리눅스가 established 해시를 먼저 보고 listening 해시를 보는 순서와 같다.
- **ISN**: `timer_ticks() * 2500 + g_isn`, 호출마다 `g_isn += 64000`. RFC 793의 "4µs마다 1 증가하는 클록"을 PIT 10ms 틱(= 2500 × 4µs)으로 근사한 것이고, 같은 틱에 연결 둘을 열어도 ISN이 겹치지 않도록 호출마다 조금씩 더한다. 리눅스는 4 tuple 해시 + 비밀 키 + 시간을 섞는다(예측 공격 방어).
- `tcp_xmit(dst_ip, src_port, dst_port, seq, ack, flags, with_mss)`: 헤더를 스택 버퍼에 만들고 체크섬을 계산해 `ip_send`로 넘긴다. TCP는 UDP와 달리 체크섬 0을 "안 씀"으로 쓰지 않는다(0이면 `0xFFFF`로 바꾸는 규칙 없음, 그대로 보낸다). `with_mss`가 켜지면 MSS 옵션(kind 2, len 4, 값 `TCP_MSS` = 1500 − IP 20 − TCP 20 = 1460) 4바이트를 붙여 `data_offset`이 6(24바이트)이 된다. SYN과 SYN+ACK에만 켠다. window 필드는 수신 버퍼가 아직 없어 고정값 `TCP_WINDOW`(4096)을 광고하는 자리표시다.
- `tcp_parse_mss(segment, hdr_len)`: 옵션 영역을 순회(END/NOP/길이 있는 옵션)해 MSS 옵션이 있으면 값을, 없으면 RFC 기본값 536을 돌려준다. `peer_mss`에 저장만 하고 쓰는 건 78(세그먼트 분할)이다.
- `tcp_rx(src_ip, segment, len)`: `ip_rx`가 부른다. 길이·`data_offset` 검사 후 의사 헤더 포함 체크섬을 **무조건** 검증한다(결과 0이 정상, 틀리면 `tcp: rx bad checksum`으로 버림). 필드를 `tcp_seg`로 뽑아 연결을 찾고 상태별 함수로 보낸다. 연결이 없으면 `tcp_rx_closed`.
- `tcp_listen(port, handler)`: listener 테이블에 등록. 0 성공, -1 가득 참 또는 같은 포트가 이미 LISTEN.
- `tcp_connect(dst_ip, dst_port, src_port, handler)`: `SYN_SENT` TCB를 만들고 SYN을 보낸다. 0은 SYN을 접수했다는 뜻이고(블록하지 않음, `ip_send`와 같은 의미) 연결 완료는 `handler`가 불릴 때다. -1은 테이블 가득 참/중복 또는 송신 실패. 출발지 포트는 호출자가 정한다(UDP와 같음).

### 상태별 수신 처리

- **연결 없음(CLOSED)** `tcp_rx_closed`: RST면 버린다. ACK가 있으면 `RST`(seq = 받은 ack), 없으면 `RST|ACK`(seq = 0, ack = 받은 seq + 세그먼트 길이)를 돌려준다(RFC 793). 세그먼트 길이는 데이터 + SYN 1 + FIN 1.
- **LISTEN** `tcp_rx_listen(listener, seg)`: RST는 버리고, ACK가 있으면 RST(seq = ack), SYN이면 자식 TCB(`SYN_RECEIVED`)를 만들어 `rcv_nxt = 상대 seq + 1`, `peer_mss` 저장 후 `SYN|ACK`(seq = 내 ISN, ack = `rcv_nxt`, MSS 옵션)를 보낸다. 테이블이 가득하면 SYN을 조용히 버린다(리눅스도 SYN을 버리고 상대가 재전송하게 한다).
- **SYN_SENT** `tcp_rx_syn_sent`: ACK가 있는데 `ack != snd_nxt`면 RST로 답한다(RST 세그먼트면 무시). ack가 맞는데 RST면 연결 거부로 보고 CLOSED. SYN+ACK(ack 일치)면 `rcv_nxt = 상대 seq + 1`, `snd_una = ack`로 갱신하고 ACK를 보낸 뒤 ESTABLISHED, `on_established` 호출.
- **SYN_RECEIVED** `tcp_rx_syn_received`: RST(seq가 `rcv_nxt`와 같을 때만)는 CLOSED. ACK가 아니면 버리고, `ack == snd_nxt`면 ESTABLISHED + `on_established`, 아니면 RST.
- **ESTABLISHED** `tcp_rx_established`: RST면 CLOSED, 그 외는 `tcp: rx in ESTABLISHED ignored`만 찍는다(데이터는 78, 손실 복구는 79, FIN 처리는 80).
- 상태 전이는 `tcp_set_state`가 `tcp: 8080 <-> 10.0.2.2:49802 SYN_RECEIVED -> ESTABLISHED` 형태로 로그한다. 콜백은 수신 스레드 컨텍스트에서 돈다 — 잠들면 안 되고 `kmalloc`도 쓰면 안 된다(UDP 핸들러와 같음).

## 3) 테스트 (`tcp.c`의 `tcp_test` + `tools/tcp_peer.py` + `make net-check`)

QEMU user 네트워크(slirp)가 상대다. 게스트가 직접 만든 세그먼트를 직접 검증하는 순환이 아니라 독립 구현이 우리 헤더·체크섬·seq/ack를 받아주는지로 확인한다.

- **게스트가 클라이언트**: `tcp_test`가 `40001`에서 `10.0.2.2:5000`(호스트)로 `tcp_connect` → 파이썬 `serve(5000)`이 `accept`. SYN+ACK를 받아 ESTABLISHED로 가는 것이 게스트 로그에 나온다.
- **게스트가 서버**: `tcp_test`는 먼저 `tcp_listen(8080)`을 하고 나서 연결한다. QEMU에 `hostfwd=tcp::5002-:8080`을 넣었고 파이썬이 `127.0.0.1:5002`로 연결하면 slirp가 게스트 `10.0.2.15:8080`으로 SYN을 보낸다. 게스트가 `SYN_RECEIVED -> ESTABLISHED`를 로그한다. 파이썬 `connect()`는 호스트 커널 수준에서 이미 성공하므로 파이썬 로그만으로는 게스트 handshake 완료를 증명하지 못한다 — 게스트 로그가 판정 근거다.
- **닫힌 포트**: `hostfwd=tcp::5003-:9`, 게스트는 9번을 listen하지 않는다. 파이썬이 5003으로 연결하면 slirp가 게스트로 SYN을 보내고 게스트가 `RST|ACK`로 답하며, 파이썬은 연결이 닫히는 것(`recv`가 `b''`)을 본다.
- `tcp_test`는 listen → `tcp_connect` → 능동 open 완료 대기(`TCP_WAIT_MS` 3000ms) → 수동 open 완료 대기 순서다. 대기는 `udp_wait_echo`와 같은 방식(플래그 + 깨울 스레드)이고 완료 플래그만 둘로 나뉜다(`g_active_done`/`g_passive_done`).
- `make net-check`는 `udp_peer.py`, `tcp_peer.py`를 둘 다 백그라운드로 띄우고 QEMU를 띄운다. `tcp_peer.py`는 UDP 쪽과 따로 돌려서 TCP 서버가 게스트 연결 시도 전에 이미 listen하고 있게 한다(UDP 테스트가 끝나는 순간 게스트가 바로 연결하므로 한 스크립트에 이어 붙이면 레이스가 난다). 파이썬 `held` 연결은 마지막에 2초 뒤 닫힌다 — 닫을 때 slirp가 FIN을 게스트로 보내는데 80 전에는 FIN을 처리하지 않으므로 게스트 로그에 `rx in ESTABLISHED ignored, [FIN|ACK]`가 slirp 재전송만큼 반복된다.

## 검증

`make net-check`와 같은 QEMU를 `-display none -debugcon stdio`로 돌린 결과 (파이썬 로그):

```
peer: tcp listening on 127.0.0.1:5000
peer: tcp accepted from 127.0.0.1:33102
peer: tcp connected to 127.0.0.1:5002
peer: tcp connected to 127.0.0.1:5003 (no listener in guest)
peer: tcp port 5003 closed by guest (b'')
```

게스트 로그:

```
tcp: 40001 <-> 10.0.2.2:5000 CLOSED -> SYN_SENT
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [SYN] seq=229000 ack=0
tcp: rx 10.0.2.2:5000 -> 10.0.2.15:40001 [SYN|ACK] seq=64001 ack=229001
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [ACK] seq=229001 ack=64002
tcp: 40001 <-> 10.0.2.2:5000 SYN_SENT -> ESTABLISHED
tcp: rx 10.0.2.2:49802 -> 10.0.2.15:8080 [SYN] seq=128001 ack=0
tcp: tx 10.0.2.15:8080 -> 10.0.2.2:49802 [SYN|ACK] seq=295500 ack=128002
tcp: rx 10.0.2.2:54768 -> 10.0.2.15:9 [SYN] seq=192001 ack=0
tcp: rx port 9 has no connection, RST
tcp: tx 10.0.2.15:9 -> 10.0.2.2:54768 [RST|ACK] seq=0 ack=192002
tcp: rx 10.0.2.2:49802 -> 10.0.2.15:8080 [ACK] seq=128002 ack=295501
tcp: 8080 <-> 10.0.2.2:49802 SYN_RECEIVED -> ESTABLISHED
```

`ack`가 상대 seq + 1(`64001 → 64002`, `128001 → 128002`, `192001 → 192002`), 우리 ISN + 1(`229000 → 229001`, `295500 → 295501`)로 맞물리는 것이 handshake의 핵심이고, slirp가 우리 체크섬을 받아줘야 이 로그가 나온다(체크섬이 틀리면 slirp가 버려서 SYN+ACK가 오지 않는다). `grub-file --is-x86-multiboot2` exit 0. 새로 만든 `tcp.c`와 수정한 `ip.c`/`udp.c`는 경고 없음. `make run-nogui`의 UDP/NVMe/ext2/셸 출력은 76과 같다.

**검증 방법의 한계**: 수신 `bad checksum` 폐기, 연결 테이블 가득 참(`SYN dropped`), SYN_RECEIVED의 RST·잘못된 ack, SYN_SENT의 거부(RST)·잘못된 ack 경로는 호스트 소켓으로 만들 수 없어 실행하지 않았다. `tcp_connect`가 상대 없는 주소로 SYN을 보내면 응답 없이 `connect timeout`만 난다(SYN 재전송 없음).

### 기존 서브시스템과의 정합 확인

- **수신 스레드 스택**: 콜백·`tcp_xmit`(스택 24바이트)·`ip_output`(약 1500바이트)이 수신 스레드에서 돈다. 16KB 스택에서 ICMP/UDP 에코 경로와 같은 깊이라 여유가 있다.
- **kheap 무잠금**: 연결 테이블은 정적 배열이고 `kmalloc`을 부르는 곳이 없다.
- **`ip_send` 블록 금지**: SYN+ACK/ACK/RST 모두 수신 스레드에서 `ip_send`로 나가며 `ip_send`는 블록하지 않는다. ARP 캐시 miss여도 수신 스레드가 멈추지 않는다.
- **`ip_checksum` 호출부**: IP 헤더·ICMP는 그대로, UDP만 `ip_pseudo_sum`을 쓰도록 바뀌었고 UDP 에코 테스트가 76과 같은 로그로 통과한다.
- **slot 경합**: 연결 테이블 쓰기는 `tcp_conn_open`(cli 안), listener 테이블 쓰기는 `tcp_listener_open`(cli 안), 상태 변경은 수신 스레드뿐이다. `tcp_connect` 직후 테스트 스레드가 TCB를 건드리는 곳은 SYN 송신 실패 시 `state = CLOSED`로 되돌리는 한 줄뿐이다.

## 이전 단계(76) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/tcp.h` | 신규 | `tcp_conn_t`, `tcp_established_t`, `tcp_listen`, `tcp_connect`, `tcp_rx`, `tcp_test` 선언과 `TCP_HDR_LEN`/`TCP_MSS` |
| `boot/tcp.c` | 신규 | TCP 헤더, 체크섬, TCB/연결 테이블, ISN, MSS 옵션, 상태별 수신 처리, 송신, 테스트 |
| `tools/tcp_peer.py` | 신규 | 호스트 TCP 피어(게스트 연결 accept → 5002 연결 → 5003 닫힌 포트 확인) |
| `boot/ip.h` | 수정 | `ip_pseudo_sum` 추가 |
| `boot/ip.c` | 수정 | `ip_pseudo_sum` 구현, `ip_rx`에 protocol 6 → `tcp_rx` 분기 |
| `boot/udp.c` | 수정 | `udp_pseudo_hdr` 제거, `udp_checksum`이 `ip_pseudo_sum` 사용 |
| `boot/ethernet.h` | 수정 | `htonl`/`ntohl` 추가 |
| `boot/kernel.c` | 수정 | `udp_test()` 뒤에 `tcp_test()` 호출 |
| `Makefile` | 수정 | `tcp.o` 규칙과 링크 대상, 의존 헤더 추가, `QEMUNET`에 `hostfwd=tcp::5002-:8080`·`tcp::5003-:9`, `net-check`에 `tcp_peer.py` |
| 나머지 전부 | 변경 없음 | 76의 파일 그대로 |

## 완료 기준

`make net-check`에서 파이썬 로그에 `peer: tcp accepted ...`와 `peer: tcp port 5003 closed by guest`가 찍히고, 게스트 로그에 `SYN_SENT -> ESTABLISHED`, `SYN_RECEIVED -> ESTABLISHED`, 닫힌 포트의 `RST|ACK` 송신이 나와야 한다. `make run-nogui`의 UDP/NVMe/ext2/셸 출력이 76과 같고 `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **78 `78-tcp-data`**: ESTABLISHED의 데이터 경로. TCB에 송신 버퍼(ack 전까지 유지)·수신 버퍼를 붙이고 `peer_mss`와 `TCP_MSS` 중 작은 값으로 세그먼트를 분할하며, `snd_una`/`snd_nxt`/`rcv_nxt`를 실제로 움직인다. 수신 버퍼 잔량을 반영한 `window` 광고(지금은 4096 고정)가 여기서 생긴다. 손실 없이 순서대로 도착한다고 가정한 정상 경로까지가 78이다. `kheap`에 잠금이 없어 버퍼는 TCB 안의 고정 배열이 안전하다. `tcp_xmit`은 데이터 인자가 없으므로 확장이 필요하고, 그때 의사 헤더 + 헤더 + 데이터를 `ip_checksum_add`로 나눠 더하는 형태(UDP처럼)로 바꾼다.
- **79 `79-tcp-recovery`**: 재전송 타이머(RTO, 처음엔 고정값)와 순서 안 맞게 온 세그먼트 보관·이어붙이기, 중복 ack 처리. 파이썬 일반 소켓은 세그먼트를 버리거나 순서를 바꿀 수 없어서 손실/재배치를 만들어줄 검증 수단이 따로 필요하다.
- **미룬 재전송**: 이번엔 SYN/SYN+ACK 재전송이 없다. SYN이 유실되면 `connect timeout`으로 끝나고, 서버가 보낸 SYN+ACK가 유실돼 상대가 SYN을 재전송하면 SYN_RECEIVED에서 무시된다(리눅스는 SYN+ACK를 재전송한다). 79의 재전송 타이머에 SYN 계열도 얹는다. 같은 이유로 ESTABLISHED에서 상대의 SYN+ACK 재전송(우리 마지막 ACK 유실)에 ACK를 다시 보내는 것도 79 이후다.
- **80 `80-tcp-close`**: FIN/상태(`FIN_WAIT_1/2`, `CLOSE_WAIT`, `LAST_ACK`, `TIME_WAIT`)와 TIME_WAIT 타이머. 지금 FIN이 오면 로그만 찍고 버려서 상대(slirp)가 FIN을 재전송한다.
- **81 `81-socket-syscall`**: 지금은 listen 항목 하나에 `on_established` 콜백이 붙어 있다. 소켓이 생기면 listen 소켓의 accept 큐(완료된 연결 대기열)가 되고, SYN_RECEIVED 자식 TCB를 backlog와 연결하는 구조가 필요하다. 지금 `SYN_RECEIVED`는 `TCP_CONN_SLOTS` 6칸을 맺어진 연결과 같이 쓰므로 SYN을 많이 받으면 칸이 찬다(리눅스는 SYN queue와 accept queue를 분리하고 SYN cookies를 둔다).
- **임시 포트 할당 없음**: 출발지 포트를 호출자가 정한다(UDP와 같음).
- **IP 단편화·라우팅 없음, 게스트 IP 고정**: 74/75/76의 힌트 그대로다.
