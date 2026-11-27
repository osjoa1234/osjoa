# 82 — socket client

**목표**: 능동 open 경로. `connect`가 `tcp_connect`로 `socket_t.conn`을 채우고, 81이 만든 소켓 fd의 `read`/`write`/`close` 위임 경로가 처음 실행된다. 81에서 임시 `-EAGAIN`이던 `read`/`write`를 `wq_add` → `thread_park` 블로킹으로 바꾸고, `shutdown`, `getsockname`/`getpeername`을 추가한다. 게스트 echo 클라이언트(`user/echo_client.c`)가 호스트 파이썬 echo 서버(`tools/echo_server.py`, QEMU user 네트워크의 `10.0.2.2`)에 접속해 검증한다. `sendto`/`recvfrom`은 UDP용이라 구현하지 않는다.

## 1) TCP 쪽 변경 (`boot/tcp.c`, `boot/tcp.h`)

- **알림 훅**: `tcp_conn_t`에 `wait_queue_t wait`를 달았다. 상태가 바뀌거나(`tcp_set_state`), 데이터가 `rcv_buf`에 들어오거나(`tcp_process_data`), FIN을 받거나(`tcp_process_fin`), `snd_una`가 전진해 송신 버퍼에 자리가 나면(`tcp_process_ack`) `tcp_wake`가 `wq_wake_all`을 부른다. 81 힌트에서 말한 "`tcp_conn_t`에 알림을 새로 달아야 한다"를 이 네 곳으로 채운 것이다.
- **대기 함수 3개**: `tcp_wait_connected`(`SYN_SENT`를 벗어날 때까지, 반환 0=`ESTABLISHED`/-1=그 외), `tcp_wait_readable`(받은 데이터가 있거나 FIN이 왔거나 `CLOSED`), `tcp_wait_writable`(송신 버퍼에 자리가 있거나 더 이상 보낼 수 없는 상태). 셋 다 같은 `tcp_wait(conn, ready)` 하나로 구현한다.
  - **lost wakeup 방지**: `cli` 상태에서 `ready()`를 검사하고, 아니면 `wq_add` 뒤 바로 `thread_park`한다. `thread_park`는 안에서 인터럽트를 끈 채 상태를 `PARKED`로 바꾸므로 검사~park 사이에 `tcp_rx`(마찬가지로 `cli` 구간)가 끼어들 틈이 없다. `net_rx_thread`가 쓰는 패턴과 같다.
  - `wq_add`/`wq_wake_all`이 `kmalloc`/`kfree`를 부르는데 `kheap`은 잠금이 없다. 이번 경로의 두 호출 모두 `cli` 구간 안이라 서로는 안 겹치지만, 인터럽트가 켜진 채 `kmalloc` 중인 다른 스레드가 선점되면 겹칠 수 있다(`pipe.c`도 같은 상태).
- **`tcp_connect`가 `tcp_conn_t *`를 돌려준다**(0이면 실패). 기존엔 `int`라 소켓이 연결 객체를 잡을 방법이 없었다.
- **`held` 필드와 `tcp_release`**: `socket_t`가 `tcp_conn_t *`를 들고 있는 동안 TCP 스택이 그 슬롯을 `CLOSED`로 돌려 다른 연결이 재사용하면(RST 수신, `TIME_WAIT` 만료) 소켓이 남의 연결을 읽게 된다. 리눅스는 소켓이 `struct sock`을 소유하고 참조 수로 수명을 정하는데, 우리 `g_conns` 고정 배열은 상태가 `CLOSED`면 곧바로 빈 슬롯이다. 그래서 `tcp_connect`가 `held = 1`을 걸고, `tcp_conn_open`은 `CLOSED && !held`일 때만 슬롯을 빈 것으로 본다. 소켓이 `close`에서 `tcp_close` 뒤 `tcp_release`로 풀어준다. 슬롯을 소켓 소유로 바꾸는 큰 설계 변경은 하지 않고 소유 표시만 얹었다.
- `tcp_local_port`/`tcp_remote_port`/`tcp_remote_ip`: `getsockname`/`getpeername`이 `tcp_conn_t` 내부를 직접 보지 않게 하는 읽기 함수다. 로컬 IP는 `ip_local`이다.

## 2) 소켓 백엔드 (`boot/socket.c`, `boot/socket.h`)

- `socket_t`에 `shut_rd`가 늘었다. `sockaddr_in_t`(16바이트: `family`, 네트워크 바이트 순서 `port`, `addr[4]`, `zero[8]`)와 임시 포트 카운터 `next_port`(49152부터, 0xFFFF 다음 49152로)도 추가했다.
- `read`/`write`는 81과 같은 `vfs_ops_t` 슬롯이다. 블로킹이 들어가며 의미가 바뀐 부분만 정리한다.

| 호출 | 82 동작 | 리눅스 대응 |
|------|---------|-------------|
| `read` | `conn` 없음 → `-ENOTCONN`. `len == 0`이면 0. `tcp_wait_readable` 뒤 `tcp_recv`. 읽은 게 없고 EOF도 아니면(RST로 `CLOSED`) `-ECONNRESET` | 블로킹 소켓의 `recv` |
| `write` | `conn` 없음 → `-EPIPE`. `len`을 다 보낼 때까지 `tcp_wait_writable` → `tcp_send`를 반복. 일부를 보낸 뒤 에러가 나면 보낸 만큼 돌려주고, 하나도 못 보냈으면 `-EPIPE` | 블로킹 소켓은 전부 큐에 넣고서야 돌아온다 |
| `close` | 81과 같고 `tcp_close` 뒤 `tcp_release` 추가 | |

- 81의 임시 `-EAGAIN`은 없어졌다(`EAGAIN` 정의도 삭제).
- `socket_connect(f, addr, addrlen)`: `f->ops`가 `socket_ops`가 아니면 `-ENOTSOCK`(88), `addrlen < 16`이면 `-EINVAL`, `family != AF_INET`이면 `-EAFNOSUPPORT`, 이미 `conn`이 있으면 `-EISCONN`(106). 임시 포트를 하나 골라 `tcp_connect`(슬롯이 없거나 SYN을 못 내보내면 `-ENOBUFS`)하고 `tcp_wait_connected`로 블록한다. `ESTABLISHED`가 아니면(RST) `tcp_release` 후 `conn`을 비우고 `-ECONNREFUSED`(111) — 소켓은 연결 안 된 상태로 돌아가 다시 `connect`할 수 있다.
- `socket_shutdown(f, how)`: `how > 2`면 `-EINVAL`, `conn`이 없으면 `-ENOTCONN`(검사 순서는 리눅스 `inet_shutdown`과 같다). `SHUT_WR`/`SHUT_RDWR`은 `tcp_close`로 FIN을 보내 반닫기 상태(`FIN_WAIT_1/2`)로 들어간다 — `tcp_may_recv`가 이 상태를 이미 받으므로 상대가 보내는 데이터는 계속 받는다. `SHUT_RD`/`SHUT_RDWR`은 `shut_rd`를 켠다. 그 뒤 `read`는 이미 버퍼에 있는 데이터는 주고 없으면 기다리지 않고 0이다(리눅스 `tcp_recvmsg`가 큐를 먼저 비우고 `RCV_SHUTDOWN`이면 0을 주는 순서).
- `socket_getname(f, addr, addrlen, peer)`: `addrlen`은 입출력 포인터다. 버퍼가 16바이트보다 작으면 앞부분만 복사하고 `*addrlen`은 실제 크기 16을 돌려준다(리눅스와 같다). 연결 안 된 소켓의 `getsockname`은 `0.0.0.0:0`, `getpeername`은 `-ENOTCONN`.

## 3) 시스콜 (`boot/syscall.c`, `boot/syscall.h`)

- `SYS_CONNECT = 42`, `SYS_SHUTDOWN = 48`, `SYS_GETSOCKNAME = 51`, `SYS_GETPEERNAME = 52`. 모두 `sys_fd_file(fd)`로 `fds[]`에서 `vfs_file_t`를 꺼내 `socket_*`에 넘기고 음수 errno를 `(u64)(long long)`으로 부호 확장해 돌려준다. fd가 없으면 `-EBADF`(9). 소켓 전용 시스콜은 `vfs_ops_t`를 거치지 않고 `fd → vfs_file_t → socket_t`를 따라간다(81 힌트의 예고대로).

## 4) 검증 프로그램

- `user/echo_client.c`(initrd의 `echoclient`): `46-musl-hello`처럼 musl-static으로 빌드한 평범한 리눅스 C 프로그램이다. `socket`/`connect`/`getsockname`/`getpeername`/`write`/`read`/`shutdown`/`close`를 libc로 호출해 `10.0.2.2:7000`(인자로 포트 변경)에 접속하고 다음을 `printf`로 출력한다 — 연결 전 `getpeername`(`ENOTCONN`), `connect`, 두 번째 `connect`(`EISCONN`), 로컬/상대 주소, 짧은 메시지 왕복, 송신 버퍼(4096)보다 큰 6000바이트 `write` 한 번 + `read` 왕복(내용 비교), `shutdown(SHUT_WR)` 뒤 서버가 닫아 `read`가 0, 아무도 안 듣는 포트(7999)로의 `connect`(`ECONNREFUSED`). 이 프로그램의 목적은 소켓·TCP 구현이 얼마나 견고한지 재는 것이 아니라, 리눅스 프로그램이 쓰는 호출 흐름이 이 OS에서 그대로 도는 것을 보이는 것이다.
- `tools/echo_server.py`: 받은 걸 그대로 돌려주고 상대 EOF에서 닫는다. 호스트에서 `python3 tools/echo_server.py`로 띄운다.

## 검증

호스트에서 `python3 tools/echo_server.py`를 띄우고 `make run`으로 부팅해 셸에서 `echoclient`를 입력한다. 부팅 줄은 81과 같다.

```
$ echoclient
echo: socket fd=3
echo: getpeername before connect: Socket not connected
echo: connect 10.0.2.2:7000
echo: connect ok
echo: connect again: Socket is connected
echo: local 10.0.2.15:49152 len=16
echo: peer  10.0.2.2:7000 len=16
echo: sent 13 bytes
echo: recv 13 bytes: hello socket
echo: big write=6000
echo: big read=6000 OK
echo: read after peer close=0
echo: connect to closed port: Connection refused
process 1 exited: code=0
```

- 서버 쪽은 `EOF after 6013 bytes`(13 + 6000)를 찍는다. 같은 실행을 연달아 두 번 해도 둘 다 통과하고 두 번째 로컬 포트가 49154(사이의 거절된 `connect`가 49153을 썼다)라, `held`/`release`로 슬롯이 정리되는 것도 확인됐다.
- `rm -rf build` 뒤 전체 빌드가 끝나고 `grub-file --is-x86-multiboot2`가 exit 0이다. 경고는 81과 같은 기존 경고뿐이다.
- 이 환경에서는 QEMU 모니터 `sendkey`(`-monitor unix:...`)로 셸에 입력해 자동화해 확인했다.

### 기존 서브시스템과의 정합 확인

- **TCB 슬롯 수명**: 위 `held`. 81이 만든 `socket_t.conn`이 처음으로 실제 슬롯을 가리키게 되며 생긴 전제 변화를 이 단계에서 처리했다.
- **셸/ext2 경로**: 셸에서 `cat /disk/hello.txt`, `ls /disk/sub`가 81과 같이 동작한다(`unimplemented rax=40`/`228`은 이전 단계부터 있던 출력).
- **`vfs_ops_t` 형제 백엔드**: 파이프와 같은 `wq_add` → `thread_park` 블로킹을 쓴다. 소켓 `write`가 `len`을 다 쓸 때까지 블록하는 건 리눅스 소켓 쓰기와 같고, 파이프의 `write`도 전부 쓸 때까지 루프한다.

## 이전 단계(81) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/tcp.h`, `boot/tcp.c` | 수정 | `tcp_connect`가 `tcp_conn_t *` 반환, `held`/`tcp_release`, `wait` 큐와 `tcp_wake`, `tcp_wait_*`, 포트/IP 접근 함수 |
| `boot/socket.h`, `boot/socket.c` | 수정 | `read`/`write` 블로킹, `shut_rd`, `socket_connect`/`socket_shutdown`/`socket_getname`, `sockaddr_in_t`, 임시 포트 |
| `boot/syscall.h`, `boot/syscall.c` | 수정 | `SYS_CONNECT`/`SYS_SHUTDOWN`/`SYS_GETSOCKNAME`/`SYS_GETPEERNAME`, `sys_fd_file` |
| `user/echo_client.c` | 신규 | musl-static C echo 클라이언트(검증용) |
| `tools/echo_server.py` | 신규 | 호스트 echo 서버 |
| `Makefile` | 수정 | `echoclient`를 `musl-gcc -static`으로 빌드·initrd 복사·clean, 의존성 |
| `initrd/.gitignore` | 수정 | `echoclient` 추가 |
| 나머지 전부 | 변경 없음 | 81의 파일 그대로 |

## 완료 기준

`make run`이 빌드되고 부팅해 `$` 프롬프트까지 가며, 호스트 echo 서버를 띄운 상태에서 `echoclient`가 위 출력으로 끝난다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **83 `83-socket-server`**: `bind`/`listen`/`accept4`/`setsockopt(SO_REUSEADDR)`. `accept` 대기는 이 단계의 `tcp_wait` 패턴을 리스너에 적용한다 — 지금 `tcp_listen`은 콜백(`on_established`)만 받고 `tcp_conn_t`는 `tcp_rx_listen`이 `g_conns`에서 만들어 콜백에 넘긴다. 서버 쪽 `tcp_conn_t`도 `held`로 소켓이 잡아야 한다.
- **미룬 것(이 단계 범위 밖)**:
  - `busybox wget`은 82 커널에서 `socket`/`connect`/`write`/`read`까지는 가서 호스트 HTTP 서버가 `200`을 찍지만, `setitimer`(38)와 `clock_gettime`(228)이 없어 `wget: clock_gettime(MONOTONIC) failed`로 끝난다. 두 시스콜을 채운 뒤 `wget`으로 재검증한다.
  - SYN/데이터 재전송이 없어(79에서 의도적으로 제외) 패킷이 사라지면 `connect`/`read`/`write`가 영원히 블록된다.
  - 블록 중 시그널(Ctrl+C)로 깨어나 `EINTR`을 돌려주는 경로가 없다.
  - 블록 중인 스레드가 죽어도 `tcp_conn_t.wait`에 노드가 남고, `wq_wake_all`이 `thread_unpark`로 죽은 스레드 상태를 `RUNNING`으로 되돌릴 수 있다(파이프 대기와 같은 한계).
  - `SYN_SENT` 중에 `close`되면 `tcp_close`가 `-1`을 돌려 FIN이 안 나가고, 나중에 `ESTABLISHED`가 돼도 주인 없는 슬롯으로 남는다. 단일 스레드 블로킹 `connect`에서는 도달할 수 없다.
  - `SIGPIPE`, `SOCK_NONBLOCK`/`O_NONBLOCK`(여전히 `-EINVAL`), `sendto`/`recvfrom`/`sendmsg`/`recvmsg`(musl `send`/`recv`를 쓰는 바이너리를 올릴 때 필요), `sys_writev`의 에러 합산, `lseek`의 `ESPIPE`는 81 힌트 그대로다.
