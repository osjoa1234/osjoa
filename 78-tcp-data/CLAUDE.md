# 78 — tcp-data

**목표**: ESTABLISHED에서 정상 경로 데이터 송수신을 구현한다(손실 없음·순서대로 도착 가정). ① TCB에 송신 버퍼·수신 버퍼를 붙이고 ② 송신은 상대 `window`와 MSS 안에서 세그먼트로 분할해 내보내며 ③ ack로 `snd_una`를 전진시키고 ④ 받은 데이터는 수신 버퍼에 넣고 즉시 ack(수신 버퍼 잔량을 `window`로 광고)하며 ⑤ 애플리케이션이 읽어 버퍼가 비면 window 갱신 ACK를 보낸다. 재전송 타이머·순서 안 맞는 세그먼트 보관·중복 ack는 79, FIN 종료는 80, 소켓은 81 범위다. 소켓이 없으므로 77의 콜백 방식을 이어, 연결이 맺어지면 콜백이 `tcp_conn_t`를 넘겨주고 호출자가 `tcp_send`/`tcp_recv`를 직접 부른다(둘 다 블록하지 않는다).

## 1) TCB와 버퍼 (`tcp.c`)

- 버퍼는 TCB 안의 고정 배열이다: `snd_buf[TCP_BUF_SIZE]`, `rcv_buf[TCP_BUF_SIZE]`, 각 4096바이트. `kheap`에 잠금이 없어 동적 할당을 피한다(75/77과 같은 이유).
- **버퍼를 시퀀스 번호로 직접 인덱싱한다**: 시퀀스 번호 `s`의 바이트는 `buf[s & TCP_BUF_MASK]`에 있다(`TCP_BUF_SIZE`가 2의 거듭제곱이라 `& 4095`가 나머지 연산). 별도의 head/tail 인덱스가 없고 `snd_una`/`snd_nxt`/`write_seq`/`rcv_nxt`/`copied_seq` 같은 시퀀스 변수만으로 버퍼 상태가 정해진다. 리눅스도 `sk_write_seq`/`copied_seq`로 소켓 버퍼 위치를 시퀀스 번호로 관리한다.
- TCB에 추가된 필드:
  - `write_seq`: 애플리케이션이 `tcp_send`로 써 넣은 데이터의 끝 시퀀스. `snd_una ≤ snd_nxt ≤ write_seq`이고 `snd_una~snd_nxt`가 "보냈지만 ack 못 받은 구간", `snd_nxt~write_seq`가 "아직 안 보낸 구간"이다. ack 전까지 버리지 않으므로 79의 재전송이 이 버퍼를 그대로 쓴다.
  - `snd_wnd`: 상대가 마지막으로 알려준 window. 상대 SYN/SYN+ACK의 window로 시작해 ack를 받을 때마다 갱신한다.
  - `copied_seq`: 애플리케이션이 `tcp_recv`로 읽어간 곳. `rcv_nxt - copied_seq`가 수신 버퍼에 쌓인 양이다.
  - `rcv_wnd_adv`: 우리가 마지막으로 상대에게 광고한 window. window 갱신 ACK를 보낼지 판단하는 기준이다.
- `tcp_conn_open`이 위 필드를 초기화하고(`write_seq = snd_nxt`), `copied_seq`는 SYN을 받아 `rcv_nxt`가 정해지는 곳(`tcp_rx_listen`, `tcp_rx_syn_sent`)에서 `rcv_nxt`와 같게 맞춘다. `snd_wnd`도 같은 곳에서 상대 SYN(또는 SYN+ACK)의 window로 정하고, SYN_RECEIVED → ESTABLISHED로 가는 ACK의 window로 한 번 더 갱신한다.
- `TCP_WINDOW`(고정 4096 자리표시)는 사라지고 `TCP_BUF_SIZE`/`TCP_BUF_MASK`가 대신한다. 광고하는 window는 `tcp_rcv_window(conn) = TCP_BUF_SIZE - (rcv_nxt - copied_seq)`이다. window 필드가 16비트이고 윈도 스케일 옵션을 보내지 않으므로 상대의 window도 스케일 없이 그대로 읽는다.

## 2) 송신

- **`tcp_xmit`을 갈랐다.** 헤더 채우기(`tcp_fill_hdr`)와 체크섬·로그·`ip_send`(`tcp_emit`)를 공용으로 빼고, 연결 없는 상대에게 보내는 RST용 `tcp_xmit(dst_ip, src_port, dst_port, seq, ack, flags)`(window 0, 옵션·데이터 없음; 마지막 인자 `with_mss`는 사라짐)와 TCB를 아는 `tcp_xmit_conn(conn, seq, flags, data_len, with_mss)`로 나눴다. 후자는 `ack = rcv_nxt`, `window = tcp_rcv_window(conn)`을 TCB에서 읽고, `snd_buf`에서 `seq`부터 `data_len`바이트를 세그먼트 버퍼(스택, 헤더 20 + MSS 옵션 4 + `TCP_MSS` 1460 = 1484바이트)에 복사해 보낸다. SYN/SYN+ACK/handshake ACK도 이 경로로 나가서 RST 외 모든 세그먼트가 같은 window를 광고한다. 로그에는 데이터가 있을 때 `(N bytes)`가 붙는다.
- `tcp_ack_now(conn)`: 데이터 없는 `ACK`(seq = `snd_nxt`)를 보낸다. handshake 마지막 ACK, 받은 데이터 ack, window 갱신 ACK가 모두 이걸 쓴다.
- `tcp_output(conn)`: "보낼 수 있는 만큼 보낸다". `in_flight = snd_nxt - snd_una`, `unsent = write_seq - snd_nxt`일 때 한 번에 보낼 길이는 `min(unsent, snd_wnd - in_flight, min(peer_mss, TCP_MSS))`이고 `unsent`가 0이거나 window가 찰 때까지 반복한다. 미전송을 다 비우는 세그먼트에는 `PSH`를 붙인다. 호출 위치가 둘(`tcp_send`: 테스트 스레드, `tcp_process_ack`: 수신 스레드)이라 `snd_nxt` 갱신이 겹치지 않게 함수 전체를 `cli` 안에서 돌린다(`tcp_conn_open`과 같은 이유). `ip_send`는 블록하지 않고 내부 잠금도 `cli`라 중첩해도 안전하다.
- `tcp_send(conn, data, len)`: ESTABLISHED가 아니면 -1, 아니면 `snd_buf`의 남은 공간(`TCP_BUF_SIZE - (write_seq - snd_una)`)만큼만 복사해 `write_seq`를 올리고 `tcp_output`을 부른 뒤 **받아들인 바이트 수**를 돌려준다(0이면 버퍼 가득, 호출자가 나중에 재시도). 리눅스의 non-blocking `send`와 같은 의미다.

## 3) 수신

`tcp_rx_established`는 RST → ACK 없는 세그먼트 버림 → `tcp_process_ack` → `tcp_process_data` → FIN 로그 → 대기 스레드 깨움 순서로 처리한다.

- `tcp_process_ack(conn, seg)`: `ack`가 `snd_nxt`보다 크면(보내지 않은 것을 ack) ACK를 한 번 보내고 세그먼트를 버린다(-1). `ack`가 `snd_una`보다 작으면(옛 ack) 무시. 그 외에는 `snd_una = ack`, `snd_wnd = seg.window`로 갱신하고 `tcp_output`을 불러 window가 열린 만큼 이어서 보낸다. 시퀀스 비교는 `tcp_seq_lt`(`(int)(a - b) < 0`)로 32비트 래핑을 처리한다.
- `tcp_process_data(conn, seg)`: 데이터가 없으면 반환. `seq != rcv_nxt`면 순서가 안 맞는 세그먼트라 **버리고 지금 기대하는 ack를 다시 보낸다**(보관은 79). 맞으면 `min(data_len, tcp_rcv_window)`만큼만 `rcv_buf`에 넣고 `rcv_nxt`를 올린 뒤 즉시 ACK한다(delayed ack 없음 — 리눅스는 보통 두 세그먼트마다/최대 40ms 지연). window보다 많이 왔으면 일부만 받아들이고 ack가 그만큼만 전진해서 상대가 알게 된다.
- `tcp_recv(conn, buf, max)`: 수신 버퍼에서 최대 `max`바이트를 꺼내 `copied_seq`를 올리고 **읽은 바이트 수**(0이면 비어 있음)를 돌려준다. 읽은 뒤 광고할 window가 `rcv_wnd_adv`보다 MSS 이상 커졌으면 window 갱신 ACK를 보낸다 — 안 보내면 window를 줄여 광고한 뒤 상대가 계속 기다릴 수 있다(zero window 교착). 리눅스의 `tcp_cleanup_rbuf`와 같은 역할이다.
- FIN은 80 범위라 처리하지 않고 `tcp: rx FIN in ESTABLISHED ignored`만 찍는다. 이 줄은 slirp의 FIN|ACK 재전송만큼 반복된다.
- 수신 스레드와 읽는 스레드 사이는 `rcv_nxt`(수신 스레드만 씀)와 `copied_seq`(읽는 스레드만 씀)를 서로 다른 쪽이 쓰는 구조라 잠금이 필요 없다. 데이터를 다 쓴 뒤 컴파일러 배리어를 두고 인덱스를 올린다.

## 4) 테스트 (`tcp_test` + `tools/tcp_peer.py`)

77의 handshake 테스트 뒤에 데이터 테스트가 이어진다. 77의 `g_active_done`/`g_passive_done` 플래그는 TCB 포인터(`g_active_conn`/`g_passive_conn`)로 바뀌었고, `tcp_wait`는 플래그 포인터 대신 조건 함수(`tcp_cond_set`/`tcp_cond_readable`/`tcp_cond_acked`)를 받는다. 대기 스레드를 깨우는 곳은 handshake 완료 콜백과 `tcp_rx_established` 끝이다.

1. **게스트 → 호스트 4000바이트** (`tcp_test_send`): `'A' + i % 26` 패턴 4000바이트를 `tcp_send` 한 번으로 넣는다. 1460 / 1460 / 1080 세 세그먼트로 나가고 모두 ack되면 `4000 bytes acked`. 파이썬은 4000바이트를 모아 패턴을 비교한다.
2. **호스트 → 게스트 6000바이트** (`tcp_test_recv`): 수신 버퍼(4096)보다 큰 양이다. 게스트가 300ms 읽지 않고 쉬어 버퍼가 차게 한 뒤 512바이트씩 `tcp_recv`로 읽으며 패턴을 비교한다. 읽어서 window가 열리면 window 갱신 ACK가 나가고 나머지가 이어서 온다.
3. **서버 쪽 연결 에코** (`tcp_test_echo`): 파이썬이 5002로 `ping from host`를 보내면 게스트 8080 연결이 `tcp_recv`로 읽고 `tcp_send`로 되돌린다. 파이썬이 같은 길이를 받아 출력한다.

`tcp_peer.py`는 handshake·닫힌 포트 확인 뒤 `guest_client_data`(4000 받고 6000 보냄)와 `guest_server_data`(ping 보내고 에코 받음)를 실행한다.

## 검증

`make net-check`와 같은 QEMU를 `-display none -debugcon stdio`로 돌린 결과 (파이썬 로그):

```
peer: tcp listening on 127.0.0.1:5000
peer: tcp accepted from 127.0.0.1:34236
peer: tcp connected to 127.0.0.1:5002
peer: tcp connected to 127.0.0.1:5003 (no listener in guest)
peer: tcp port 5003 closed by guest (b'')
peer: tcp rx 4000 bytes from guest, pattern ok
peer: tcp sent 6000 bytes to guest
peer: tcp echo from guest b'ping from host'
```

게스트 로그 (handshake 이후):

```
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [ACK] seq=165001 ack=64002 (1460 bytes)
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [ACK] seq=166461 ack=64002 (1460 bytes)
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [PSH|ACK] seq=167921 ack=64002 (1080 bytes)
tcp: rx 10.0.2.2:5000 -> 10.0.2.15:40001 [ACK] seq=64002 ack=166461
tcp: rx 10.0.2.2:5000 -> 10.0.2.15:40001 [ACK] seq=64002 ack=167921
tcp: rx 10.0.2.2:5000 -> 10.0.2.15:40001 [ACK] seq=64002 ack=169001
tcp: queued 4000 bytes
tcp: 4000 bytes acked
tcp: receive buffer holds 2880 bytes, window 1216
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [ACK] seq=169001 ack=66882
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [ACK] seq=169001 ack=68322
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [ACK] seq=169001 ack=69762
tcp: rx 10.0.2.2:5000 -> 10.0.2.15:40001 [PSH|ACK] seq=69762 ack=169001
tcp: received 6000 bytes, 0 mismatched
tcp: passive rx 14 bytes "ping from host"
tcp: tx 10.0.2.15:8080 -> 10.0.2.2:38966 [PSH|ACK] seq=231501 ack=128016 (14 bytes)
tcp: rx 10.0.2.2:38966 -> 10.0.2.15:8080 [ACK] seq=128016 ack=231515
```

- 송신: 4000바이트가 1460 + 1460 + 1080으로 분할되고 seq가 165001 → 166461 → 167921로 길이만큼 전진한다. 상대 ack가 166461 → 167921 → 169001(= 165001 + 4000)로 올라가 `snd_una`가 `write_seq`에 닿으면 `4000 bytes acked`.
- 수신: 6000바이트(seq 64002 → 70002)가 모두 도착했고 패턴 불일치 0.
- window: 게스트가 읽기 전 300ms 동안 수신 버퍼에 2880바이트(1440 × 2)가 쌓였고 광고 window는 4096 − 2880 = 1216이다. 세그먼트 하나(1440)가 1216에 들어가지 않아 상대가 멈춰 있다가, 게스트가 읽은 뒤 나간 window 갱신 ACK(`ack=66882`, `68322`, `69762`)로 나머지가 이어서 왔다.
- 에코: 서버 쪽 연결에서도 `tcp_recv` → `tcp_send`로 14바이트가 되돌아가 ack된다.

`grub-file --is-x86-multiboot2` exit 0. `tcp.c`는 경고 없음. `make run-nogui`의 UDP/NVMe/ext2/셸 출력은 77과 같다.

**검증 방법의 한계**: 호스트 소켓은 세그먼트를 버리거나 순서를 바꿀 수 없어서 `seq != rcv_nxt`로 버리는 경로와 `ack`가 `snd_nxt`를 넘는 경로는 실행하지 않았다. window는 0까지 가지 않고 세그먼트가 안 들어가는 1216에서 멈췄다(0 광고 상태는 미검증). `tcp_send`가 버퍼 가득으로 일부만 받아들이는 경로도 이번 테스트(4000 ≤ 4096)에선 타지 않는다.

### 기존 서브시스템과의 정합 확인

- **스택**: `tcp_xmit_conn`이 1484바이트 세그먼트 버퍼를 스택에 둔다. 수신 스레드(16KB)에서 `tcp_rx → tcp_rx_established → tcp_process_ack → tcp_output → tcp_xmit_conn → ip_send`로 UDP 송신(`udp.c`의 1480바이트 버퍼)과 같은 구조의 깊이이고, 테스트 스레드(부팅 스택 16KB)는 `tcp_test_send`의 4000바이트 배열 + 같은 송신 경로다.
- **kheap 무잠금**: 새 코드에 `kmalloc`이 없다.
- **`cli` 구간**: `tcp_output`/`tcp_send`가 `cli` 안에서 `console_printf`와 `ip_send`를 부른다. 한 번에 몇 세그먼트(복사+체크섬)라 PIT 틱 지연은 미미하다.
- **`ip_send` 블록 금지**: `tcp_output`은 `ip_send`가 블록하지 않으므로 수신 스레드에서 불러도 멈추지 않는다.

## 이전 단계(77) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/tcp.h` | 수정 | `tcp_send`, `tcp_recv` 선언 |
| `boot/tcp.c` | 수정 | TCB에 버퍼·`write_seq`/`snd_wnd`/`copied_seq`/`rcv_wnd_adv` 추가, `tcp_xmit` 분리(`tcp_fill_hdr`/`tcp_emit`/`tcp_xmit_conn`/`tcp_ack_now`), `tcp_output`(MSS 분할·window 준수), ack/데이터 처리, `tcp_send`/`tcp_recv`, 데이터 테스트 |
| `tools/tcp_peer.py` | 수정 | 4000바이트 수신 검증, 6000바이트 송신, 서버 쪽 연결 ping/에코 |
| 나머지 전부 | 변경 없음 | 77의 파일 그대로 (`Makefile`도 변경 없음) |

## 완료 기준

`make net-check`에서 파이썬 로그에 `peer: tcp rx 4000 bytes from guest, pattern ok`, `peer: tcp sent 6000 bytes to guest`, `peer: tcp echo from guest b'ping from host'`가 찍히고, 게스트 로그에 `4000 bytes acked`, `received 6000 bytes, 0 mismatched`, `passive rx 14 bytes`가 나와야 한다. 77의 handshake·닫힌 포트 출력이 그대로이고 `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **79 `79-tcp-recovery`**: 재전송 타이머(RTO). `snd_una~snd_nxt` 구간이 `snd_buf`에 남아 있으므로 타이머가 터지면 `snd_nxt = snd_una`로 되돌려 `tcp_output`을 다시 돌리는 것이 출발점이다. 순서 안 맞는 세그먼트는 지금 버리고 중복 ack만 보내는데, 79에서 보관·이어붙이기로 바꾼다. SYN/SYN+ACK 재전송과 마지막 ACK 유실 재처리도 여기서 한다(77 힌트). 파이썬 일반 소켓은 손실/재배치를 만들 수 없어 별도 검증 수단이 필요하다.
- **미룬 검증**: `seq != rcv_nxt` 버림 경로, `ack > snd_nxt` 경로, window 0 상태, `tcp_send` 부분 수락(> 4096 송신)은 손실/재배치/대용량 검증 수단이 생기는 79에서 실행한다.
- **window 갱신 규칙**: 지금은 ack가 올 때마다 `snd_wnd`를 덮어쓴다. 옛 세그먼트가 늦게 도착해 window를 되돌리는 것을 막는 RFC 793의 `snd_wl1`/`snd_wl2` 검사는 순서가 뒤바뀐 세그먼트가 생기는 79에서 필요해진다.
- **zero window**: 상대가 window 0을 광고하면 `tcp_output`이 멈추고 window 갱신 ACK를 기다린다. 그 ACK가 유실되면 영원히 멈추므로 persist 타이머(window probe)가 필요하다 — 타이머가 생기는 79 범위다.
- **delayed ack / Nagle / silly window 회피 없음**: 모든 데이터 세그먼트에 즉시 ACK하고, 송신은 window에 들어가는 만큼 작은 세그먼트도 바로 보낸다.
- **80 `80-tcp-close`**: FIN은 지금 로그만 찍고 버린다. 수신 버퍼에 남은 데이터를 다 읽은 뒤 EOF로 알리는 것과, 송신 버퍼를 다 보낸 뒤 FIN을 붙이는 것이 80의 일이다.
- **81 `81-socket-syscall`**: `tcp_send`/`tcp_recv`는 블록하지 않는다. 소켓의 `send`/`recv`는 이 위에 wait queue(29)로 블록 동작을 얹는다. 테스트가 쓰는 전역 `g_waiter` 하나는 연결마다의 대기 큐로 바뀌어야 한다.
- **임시 포트 할당 없음, IP 단편화·라우팅 없음, 게스트 IP 고정**: 77/76의 힌트 그대로다.
