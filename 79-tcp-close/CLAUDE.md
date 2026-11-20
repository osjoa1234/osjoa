# 79 — tcp-close

**목표**: FIN으로 연결을 닫는다. ① 우리가 먼저 닫는 쪽(`ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT → CLOSED`)과 ② 상대가 먼저 닫는 쪽(`ESTABLISHED → CLOSE_WAIT → LAST_ACK → CLOSED`)을 모두 구현하고, 동시에 닫는 경우의 `CLOSING`까지 상태 기계에 넣는다. 78까지 FIN은 로그만 찍고 버렸다. 손실 복구(재전송 타이머·순서 밖 세그먼트 보관·중복 ack)는 161에서 정한 대로 구현하지 않는다 — QEMU user 네트워크는 게스트 쪽 TCP 연결을 자기 안에서 끝내고 호스트와는 별도 소켓으로 중계하므로 게스트는 손실·재배치를 볼 수 없다. 그래서 FIN도 재전송하지 않고, TIME_WAIT 타이머만 새로 생긴 유일한 타이머다. 소켓 syscall은 80 범위다. 연결 종료 API도 77/78의 콜백 방식을 이어 블록하지 않는 `tcp_close`/`tcp_eof`로 둔다.

## 1) FIN은 시퀀스 번호를 하나 쓴다

- SYN처럼 FIN도 시퀀스 번호 1칸을 차지한다. 송신 쪽은 `tcp_close`가 `fin_queued`를 켜고, `tcp_output`이 **보낼 데이터를 다 내보낸 뒤**(`snd_nxt == write_seq`) FIN|ACK를 `seq = snd_nxt`로 보내고 `snd_nxt`와 `write_seq`를 1씩 올린다. FIN이 `write_seq`에 포함되므로 "FIN까지 ack됐다"는 `snd_una == write_seq`이고 이를 `tcp_fin_acked`가 `fin_sent`와 함께 확인한다. 보낼 데이터가 남아 있으면 `tcp_close`가 상태만 먼저 바꾸고 FIN은 데이터가 다 나간 뒤에 붙는다(리눅스 `tcp_send_fin`도 송신 큐 맨 뒤에 붙인다).
- 수신 쪽은 `rcv_nxt`에 FIN의 1칸을 더하지 않는다. `rcv_nxt - copied_seq`가 수신 버퍼에 쌓인 바이트 수이고 window도 이 값으로 계산하므로, FIN까지 `rcv_nxt`에 넣으면 FIN이 버퍼 안의 1바이트처럼 보인다. 대신 `rcv_fin` 플래그를 두고 상대에게 보내는 ack 값만 `tcp_rcv_ack(conn) = rcv_nxt + rcv_fin`으로 만든다. 리눅스는 `rcv_nxt`를 올리고 `tcp_recvmsg`가 FIN을 소비할 때 `copied_seq`도 하나 올려서 맞춘다 — 우리는 버퍼 표현을 안 바꾸려고 플래그로 갈랐다.
- FIN이 받아들여지는 조건은 `seg.seq + seg.data_len == rcv_nxt`다. 같은 세그먼트의 데이터가 다 `rcv_nxt`에 반영된 뒤에야 FIN 위치와 맞는다. 순서가 안 맞아 버린 세그먼트나 수신 window에 다 안 들어가 잘린 세그먼트의 FIN은 조건이 안 맞아 무시된다(손실 복구가 없으니 이 경우 상대의 재전송은 기대하지 않는다 — QEMU에서는 생기지 않는다).
- 이미 FIN을 받은 뒤 같은 FIN이 또 오면(`rcv_fin`이 켜져 있으면) ack만 다시 보내고 상태는 안 바꾼다. 77/78에서 slirp가 FIN|ACK를 반복해서 보내던 것과 같은 경우를 막는다.

## 2) 상태와 전이 (`tcp.c`)

- 상태 6개 추가: `FIN_WAIT_1`, `FIN_WAIT_2`, `CLOSE_WAIT`, `CLOSING`, `LAST_ACK`, `TIME_WAIT`. `tcp_rx_established`는 ESTABLISHED 이후 동기화된 모든 상태가 같이 쓰는 `tcp_rx_synchronized`로 이름과 역할이 바뀌었다. RST → ACK 확인 → `tcp_process_ack` → `tcp_process_data` → `tcp_process_fin` → `tcp_advance_state` 순서다.
- `tcp_advance_state(conn, fin)`: 한 세그먼트를 처리한 뒤 "내 FIN이 ack됐는가(`tcp_fin_acked`)"와 "상대 FIN을 이번에 받았는가(`fin`)" 두 값으로 전이를 정한다.

| 지금 상태 | 내 FIN이 ack됨 | 상대 FIN 받음 | 다음 상태 |
|-----------|----------------|---------------|-----------|
| `ESTABLISHED` | — | 예 | `CLOSE_WAIT` |
| `FIN_WAIT_1` | 예 | 아니오 | `FIN_WAIT_2` |
| `FIN_WAIT_1` | 예 | 예 | `TIME_WAIT` |
| `FIN_WAIT_1` | 아니오 | 예 | `CLOSING` |
| `FIN_WAIT_2` | — | 예 | `TIME_WAIT` |
| `CLOSING` | 예 | — | `TIME_WAIT` |
| `LAST_ACK` | 예 | — | `CLOSED` |

- `tcp_close(conn)`: 우리 쪽 방향만 닫는다(half-close). `ESTABLISHED`면 `FIN_WAIT_1`, `CLOSE_WAIT`면 `LAST_ACK`로 상태를 바꾸고 `tcp_output`을 불러 FIN을 내보낸다. 그 외 상태면 -1. 호출자는 **블록되지 않고** 바로 돌아온다. `FIN_WAIT_1/2`에서도 데이터를 계속 받을 수 있다(상대가 반쪽만 닫은 우리를 향해 더 보내도 `tcp_recv`로 읽힌다). `tcp_send`는 `ESTABLISHED`와 `CLOSE_WAIT`에서만 받는다 — 상대가 FIN을 보냈어도 우리가 아직 보낼 수 있다.
- `tcp_eof(conn)`: `rcv_fin`이 켜져 있고 수신 버퍼가 비었으면 1. `tcp_recv`는 읽은 바이트 수를 돌려주고 0은 "지금은 비어 있음"이라 EOF와 구분이 안 되어서 따로 뒀다. 리눅스 `recv`의 "0 반환 = EOF"와 같은 정보를 두 호출로 나눈 것이고, 소켓이 생기는 80에서 `recv`가 합친다.
- `tcp_recv`의 window 갱신 ACK는 `ESTABLISHED`만이 아니라 아직 상대 데이터를 받을 수 있는 `ESTABLISHED/FIN_WAIT_1/FIN_WAIT_2`(`tcp_may_recv`)에서 보낸다. 상대 FIN을 받은 뒤엔 보내지 않는다.
- `tcp_conn_open`이 새 필드(`rcv_fin`, `fin_queued`, `fin_sent`, `tw_at`)를 0으로 초기화한다. `TIME_WAIT`도 `CLOSED`가 아니라서 `tcp_conn_find`가 찾고 `tcp_conn_open`의 중복 검사에도 걸린다 — 같은 4-tuple로 바로 다시 연결하지 못하게 하는 것이 TIME_WAIT의 목적이다.

## 3) TIME_WAIT와 타이머 스레드

- 연결을 먼저 닫은 쪽은 마지막 ACK가 유실돼 상대가 FIN을 다시 보낼 수 있으므로 한동안 연결 항목을 남겨 둔다(`TIME_WAIT`). `TIME_WAIT`에서 FIN이 다시 오면 `tcp_rx_time_wait`가 ack하고 만료 시각을 다시 잡는다. 같은 4-tuple의 옛 세그먼트가 새 연결에 섞이지 않게 하는 효과도 있다.
- **TIME_WAIT 시간은 1000ms로 줄였다.** 리눅스는 `TCP_TIMEWAIT_LEN = 60초`(2×MSL, MSL 30초)다. 테스트가 끝나야 해서 줄였을 뿐 동작은 같다. 상수는 `TCP_TIME_WAIT_MS`.
- 만료를 보는 주체는 `tcp_init`이 만드는 커널 스레드(`tcp_timer_thread`)다. `TCP_TIMER_MS` 50ms마다 일어나 연결 테이블을 훑고, `TIME_WAIT`이면서 `tw_at`(만료 tick)이 지난 연결을 `CLOSED`로 돌린다. `timer_ticks()`는 부호 있는 차이(`(int)(now - tw_at) >= 0`)로 비교해 32비트 래핑을 처리한다. `kernel.c`에서 `net_rx_init()` 다음에 `tcp_init()`을 부른다.
- 이 스레드가 TCB를 만지므로 수신 스레드와의 경쟁이 새로 생긴다. `tcp_rx`는 78의 본문을 `tcp_rx_segment`로 이름만 바꾸고, `tcp_rx`는 그 전체를 `cli`로 감싸는 래퍼가 됐다. `tcp_close`도 `cli` 안에서 상태 변경과 `tcp_output`을 한다. 둘 다 `console_printf`와 `ip_send`를 `cli` 안에서 부르지만 78의 `tcp_output`/`tcp_send`와 같은 이유로 허용한다(한 번에 몇 세그먼트, `ip_send`는 블록하지 않음).

## 4) 테스트 (`tcp_test` + `tools/tcp_peer.py`)

78의 데이터 테스트(`tcp_test_send` → `tcp_test_recv` → `tcp_test_echo`) 뒤에 두 종료 시나리오가 이어진다. 파이썬은 연결 두 개를 열어 두고 있다가 각각 다른 방식으로 닫는다.

1. **상대가 먼저 닫는 쪽** (`tcp_test_close_passive`, 서버 쪽 연결 8080): 파이썬이 에코를 받은 뒤 `host_close`로 소켓을 닫는다 → FIN. 게스트는 `CLOSE_WAIT`가 되고 `tcp_eof`로 EOF를 확인한 뒤 `tcp_close` → `LAST_ACK`, FIN을 보내고 ack를 받으면 `CLOSED`.
2. **우리가 먼저 닫는 쪽** (`tcp_test_close_active`, 클라이언트 쪽 연결 40001): 게스트가 `tcp_close` → `FIN_WAIT_1`. 파이썬 `guest_close`는 `recv(1)`이 `b''`(EOF)로 돌아오는 걸 확인하고 `bye from host`를 보낸 뒤 소켓을 닫는다. 게스트는 FIN이 ack되어 `FIN_WAIT_2`가 된 뒤에도 `tcp_recv`로 `bye from host`를 읽고, 상대 FIN으로 `TIME_WAIT` → 1000ms 뒤 `CLOSED`. half-close에서 반대 방향이 계속 열려 있음을 확인하는 것이다.

순서는 passive 먼저, active 나중이다. 파이썬이 한 스레드로 순서대로 도는데 active 쪽 EOF를 기다리는 `guest_close`가 마지막이어야 서버 쪽 연결의 에코가 먼저 끝나기 때문이다.

## 검증

`make net-check`와 같은 QEMU를 `-display none -debugcon stdio`로 돌린 결과 (파이썬 로그):

```
peer: tcp listening on 127.0.0.1:5000
peer: tcp accepted from 127.0.0.1:58266
peer: tcp connected to 127.0.0.1:5002
peer: tcp connected to 127.0.0.1:5003 (no listener in guest)
peer: tcp port 5003 closed by guest (b'')
peer: tcp rx 4000 bytes from guest, pattern ok
peer: tcp sent 6000 bytes to guest
peer: tcp echo from guest b'ping from host'
peer: tcp closed by host
peer: tcp guest closed its side (b'')
peer: tcp sent b'bye from host' after guest FIN
```

게스트 로그 (종료 부분):

```
tcp: rx 10.0.2.2:35510 -> 10.0.2.15:8080 [FIN|ACK] seq=128016 ack=184015
tcp: tx 10.0.2.15:8080 -> 10.0.2.2:35510 [ACK] seq=184015 ack=128017
tcp: 8080 <-> 10.0.2.2:35510 ESTABLISHED -> CLOSE_WAIT
tcp: passive peer closed, EOF
tcp: 8080 <-> 10.0.2.2:35510 CLOSE_WAIT -> LAST_ACK
tcp: tx 10.0.2.15:8080 -> 10.0.2.2:35510 [FIN|ACK] seq=184015 ack=128017
tcp: rx 10.0.2.2:35510 -> 10.0.2.15:8080 [ACK] seq=128017 ack=184016
tcp: 8080 <-> 10.0.2.2:35510 LAST_ACK -> CLOSED
tcp: 40001 <-> 10.0.2.2:5000 ESTABLISHED -> FIN_WAIT_1
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [FIN|ACK] seq=121501 ack=70002
tcp: rx 10.0.2.2:5000 -> 10.0.2.15:40001 [ACK] seq=70002 ack=121502
tcp: 40001 <-> 10.0.2.2:5000 FIN_WAIT_1 -> FIN_WAIT_2
tcp: rx 10.0.2.2:5000 -> 10.0.2.15:40001 [PSH|ACK] seq=70002 ack=121502
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [ACK] seq=121502 ack=70015
tcp: rx 10.0.2.2:5000 -> 10.0.2.15:40001 [FIN|ACK] seq=70015 ack=121502
tcp: tx 10.0.2.15:40001 -> 10.0.2.2:5000 [ACK] seq=121502 ack=70016
tcp: 40001 <-> 10.0.2.2:5000 FIN_WAIT_2 -> TIME_WAIT
tcp: active rx 13 bytes "bye from host" after close
tcp: active peer closed, EOF
tcp: 40001 <-> 10.0.2.2:5000 TIME_WAIT -> CLOSED
```

- 상대가 먼저 닫는 쪽: 상대 FIN의 seq 128016에 대해 ack가 128017로 1 올라간다(FIN이 1칸). 우리 FIN(seq 184015)을 상대가 ack 184016으로 확인하면 `LAST_ACK → CLOSED`.
- 우리가 먼저 닫는 쪽: FIN의 seq 121501에 대해 상대 ack가 121502. `FIN_WAIT_2`에서 `bye from host` 13바이트를 받고(`ack=70015`) 이어서 온 FIN(seq 70015)에 ack 70016, 곧바로 `TIME_WAIT`. 1초 뒤 타이머 스레드가 `CLOSED`로 돌린다.
- `grub-file --is-x86-multiboot2` exit 0, `tcp.c` 경고 없음, 78의 handshake·데이터·닫힌 포트 출력과 `udp:`/`nvme:`/`ext2:`/셸 출력은 그대로다.

**검증 방법의 한계**: 파이썬 일반 소켓이 상대라 만들 수 있는 종료 흐름만 실행했다. 다음은 **실행하지 않았다**.
- `CLOSING`(양쪽이 동시에 FIN) — 호스트 소켓이 우리 FIN과 동시에 FIN을 보내게 만들 수 없다. 상태 기계에는 있다.
- `FIN_WAIT_1`에서 FIN과 ack가 한 세그먼트로 오는 경로(→ 바로 `TIME_WAIT`).
- `TIME_WAIT`에서 FIN을 다시 받는 경로(`tcp_rx_time_wait`).
- FIN과 데이터가 한 세그먼트에 실려 오는 경우, 보낼 데이터가 남은 채로 `tcp_close`를 부르는 경우(FIN이 데이터 뒤에 붙는 경로).
- RST로 닫히는 경로의 새 상태들.

### 기존 서브시스템과의 정합 확인

- **`tcp_rx` 전체 `cli`**: 수신 스레드가 세그먼트 하나를 처리하는 동안 인터럽트가 꺼진다. 그 안에서 `console_printf`, `ip_send`, 상태 로그를 부른다. 78에서 `tcp_output`이 이미 같은 구조였고 세그먼트 하나의 처리는 짧다. `ip_send`는 블록하지 않는다.
- **연결 테이블 크기**: `TIME_WAIT`이 연결 항목 하나를 1초 동안 차지한다. 슬롯은 6개이고 테스트는 연결 두 개를 쓰므로 부족하지 않다.
- **`kheap` 무잠금**: 새 코드에 `kmalloc`이 없다.
- **인터페이스 일관성**: `tcp_send`/`tcp_recv`/`tcp_close`/`tcp_eof` 모두 블록하지 않는다. 리눅스의 non-blocking 소켓과 같은 의미이고, 블록은 소켓이 생기는 80에서 wait queue로 얹는다.

## 이전 단계(78) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/tcp.h` | 수정 | `tcp_close`, `tcp_eof`, `tcp_init` 선언 |
| `boot/tcp.c` | 수정 | 상태 6개, TCB에 `rcv_fin`/`fin_queued`/`fin_sent`/`tw_at`, `tcp_output`의 FIN 송신, `tcp_process_fin`/`tcp_advance_state`, `tcp_rx_established` → `tcp_rx_synchronized`, `tcp_rx_time_wait`, `tcp_close`/`tcp_eof`, TIME_WAIT 타이머 스레드(`tcp_timer_thread`/`tcp_init`), `tcp_rx` cli 래퍼, 종료 테스트 |
| `boot/kernel.c` | 수정 | `net_rx_init()` 다음에 `tcp_init()` 호출 |
| `tools/tcp_peer.py` | 수정 | 서버 쪽 연결을 `host_close`로 먼저 닫고, 클라이언트 쪽은 `guest_close`로 EOF 확인 뒤 `bye from host`를 보내고 닫음. 마지막 `time.sleep` 제거 |
| 나머지 전부 | 변경 없음 | 78의 파일 그대로 (`Makefile`도 변경 없음) |

## 완료 기준

`make net-check`에서 파이썬 로그에 `peer: tcp closed by host`, `peer: tcp guest closed its side (b'')`, `peer: tcp sent b'bye from host' after guest FIN`이 찍히고, 게스트 로그에 `ESTABLISHED -> CLOSE_WAIT`, `LAST_ACK -> CLOSED`, `FIN_WAIT_1 -> FIN_WAIT_2`, `FIN_WAIT_2 -> TIME_WAIT`, `active rx 13 bytes "bye from host" after close`, `TIME_WAIT -> CLOSED`가 순서대로 나와야 한다. 78의 handshake·데이터 출력이 그대로이고 `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **80 `80-socket-syscall`**: `tcp_send`/`tcp_recv`/`tcp_close`는 블록하지 않는다. 소켓의 `send`/`recv`/`close`는 이 위에 wait queue(29)로 블록 동작을 얹는다. `recv`가 0을 돌려주는 EOF는 지금 `tcp_eof`와 `tcp_recv` 두 호출로 나뉘어 있어 여기서 합친다. 테스트가 쓰는 전역 `g_waiter` 하나는 연결마다의 대기 큐로 바뀌어야 한다. `close(2)`는 `tcp_close`이고 `shutdown(SHUT_WR)`도 같은 호출로 매핑되지만 `SHUT_RD`는 따로 필요하다.
- **미룬 검증**: `CLOSING`, `TIME_WAIT`에서의 FIN 재수신, FIN+데이터 한 세그먼트, 데이터가 남은 채 `tcp_close`는 파이썬 일반 소켓으로 만들 수 없어 실행하지 못했다. 소켓과 유저 프로그램(busybox `nc` 등)이 생기는 단계에서 다시 볼 수 있다.
- **손실 복구 없음**: 재전송 타이머·순서 밖 세그먼트 보관·중복 ack·persist 타이머·window 갱신 순서 검사(`snd_wl1`/`snd_wl2`)는 구현하지 않는다(161). FIN이 유실되면 연결이 `FIN_WAIT_1`/`LAST_ACK`에 영원히 남는다.
- **TIME_WAIT 슬롯**: 연결 항목 6개 중 하나를 `TIME_WAIT`이 점유한다. 소켓이 연결을 자주 열고 닫으면 슬롯이 모자랄 수 있다. 리눅스는 `TIME_WAIT`을 별도의 가벼운 구조(`inet_timewait_sock`)로 분리한다.
- **delayed ack / Nagle / silly window 회피 없음, 임시 포트 할당 없음, IP 단편화·라우팅 없음, 게스트 IP 고정**: 77/78의 힌트 그대로다.
- **81 `81-net-apps`**: busybox `nc`/`wget`으로 실제 네트워크 애플리케이션 실행 검증.
