# 81 — socket

**목표**: 소켓 객체와 fd 백엔드. `socket(AF_INET, SOCK_STREAM, 0)`이 fd를 돌려주고, 그 fd를 `close`/`dup2`/`fork`로 다룰 수 있게 한다. 소켓은 `tcp_conn_t`(TCB)를 가리키는 포인터를 갖고, `read`/`write`/`close`는 그 연결이 있으면 `tcp_recv`/`tcp_send`/`tcp_close`로 넘긴다. 다만 이 포인터를 채우는 `connect`가 아직 없어서 이 경로는 실행되지 않는다(검증은 82로 미룸). `connect`는 82, `bind`/`listen`/`accept4`는 83이다. 80이 기능 없는 정리 단계가 되면서 로드맵의 소켓 단계가 한 칸씩 밀렸다.

## 1) 소켓 백엔드 (`boot/socket.c`)

- `socket_t`는 `conn`(`tcp_conn_t *`), `refs`, `used` 세 필드다. `SOCKET_MAX = 8`개의 고정 배열이고 `pipe.c`의 `pipes[PIPE_MAX]`와 같은 구조다. `conn`은 `socket_create`가 0으로 초기화하고, 채우는 건 `connect`(82)/`accept`(83)다.
- `socket_ops`는 `vfs_ops_t`의 기존 슬롯 중 소켓에 의미 있는 것만 채운다. 나머지는 `pipe.c`처럼 0이다.

| 슬롯 | 구현 | 이유 |
|------|------|------|
| `read` | `conn`이 없으면 `-ENOTCONN`. 있으면 `tcp_recv`, 읽은 게 없고 EOF도 아니면 `-EAGAIN` | 연결 안 된 소켓의 `read` |
| `write` | `conn`이 없으면 `-EPIPE`. 있으면 `tcp_send`, 연결이 보낼 수 없는 상태면 `-EPIPE`, 송신 버퍼가 꽉 차 0바이트면 `-EAGAIN` | 연결 안 된 소켓의 `write` |
| `close` | `refs--`, 0이면 `conn`이 있을 때 `tcp_close` 후 슬롯 해제 | 마지막 fd가 닫힐 때만 FIN을 보냄 |
| `dup` | `refs++` | `dup2`/`fcntl`/`fork`의 `vfs_dup`이 부름 |
| `mode` | `S_IFSOCK \| 0777` | `fstat`용. 리눅스 소켓 inode의 모드 |
| `open`/`size`/`getdents`/`mkdir`/`unlink`/`symlink`/`readlink`/`lstat` | 0 | 경로·크기·디렉토리 개념이 없다 |

- `-EAGAIN`은 임시 값이다. 지금은 블로킹할 수단이 없어서 "지금은 줄 데이터가 없음"을 EOF(0)와 구분해 돌려주는 것뿐이고, 82에서 `tcp_conn_t`에 알림을 달면 `wq_add` → `thread_park` 대기로 바뀐다. `write`가 일부만 보내는 경우(`tcp_send`가 `len`보다 적게 받음)는 짧은 쓰기로 그대로 돌려준다.
- `close`가 `tcp_close`의 반환값을 무시하는 건 `ESTABLISHED`/`CLOSE_WAIT`가 아닌 상태(이미 닫는 중이거나 `CLOSED`)에서는 `-1`이 돌아오는데 그때는 보낼 FIN이 없기 때문이다.
- `read`가 `ENOTCONN`, `write`가 `EPIPE`인 건 이 머신의 리눅스에서 파이썬으로 연결 안 된 `SOCK_STREAM` 소켓에 직접 호출해 확인한 값이다. 로드맵의 "둘 다 `ENOTCONN`"은 `write`가 틀렸다.
- `socket_create(domain, type, protocol, &out)`는 `vfs_file_t`를 만들어 `out`에 넣고 0, 실패하면 음수 errno를 돌려준다. 검사 순서는 리눅스 `inet_create`와 같다.
  1. `domain != AF_INET` → `-EAFNOSUPPORT`(97)
  2. `type`에 `SOCK_STREAM`(1)과 `SOCK_CLOEXEC`(0x80000) 말고 다른 비트가 있으면 → `-EINVAL`(22)
  3. 종류가 `SOCK_STREAM`이 아니면 → `-ESOCKTNOSUPPORT`(94)
  4. `protocol`이 0도 `IPPROTO_TCP`(6)도 아니면 → `-EPROTONOSUPPORT`(93)
  5. 슬롯이 없으면 → `-ENFILE`(23), `vfs_file_t` 할당 실패면 `-ENOMEM`(12)
- `SOCK_NONBLOCK`(0x800)은 `-EINVAL`로 거절한다. 논블로킹 I/O 자체가 없어서 받아들이고 무시하면 거짓말이 된다. 82에서 블로킹 `connect`가 생기면 `O_NONBLOCK`과 함께 다시 본다.

## 2) 시스콜 (`boot/syscall.c`)

- `SYS_SOCKET = 41`. `sys_socket`이 `socket_create`로 `vfs_file_t`를 만들고 `sys_open`처럼 빈 fd 슬롯에 넣는다. fd 테이블이 가득 차면 `vfs_close`로 되돌리고 `-EMFILE`(24)이다. `SOCK_CLOEXEC`이 있으면 `fd_cloexec`에 그 fd 비트를 켠다(62에서 만든 장치 그대로).
- **`SYS_READ`/`SYS_WRITE` 디스패치에 부호 확장을 넣었다.** `sys_read`/`sys_write`는 `u32`를 돌려주고 디스패치가 `frame->rax`에 그대로 넣으면 `-107`이 `0xFFFFFF95`(양수)로 유저에 보인다. `SYS_OPEN`처럼 `(u64)(long long)(int)`로 감쌌다. 기존 백엔드의 반환값은 모두 2GB 미만이라 영향이 없다.
- `boot/vfs.c`의 `vfs_read`/`vfs_write`는 반환값이 에러(`(int)n < 0`)면 `f->pos`를 올리지 않는다. 소켓이 처음으로 `read`/`write`에서 음수 errno를 돌려주는 백엔드라 위치가 `0xFFFFFF95`만큼 튀는 걸 막았다. 소켓은 위치 개념이 없지만 `dup`으로 복제된 `vfs_file_t`가 쓰는 값이라 깨끗하게 둔다.

## 검증

`make run-nogui`의 부팅 줄이 80과 같다.

```
net_rx: ring 16 slots, rx thread started
tcp: time-wait timer thread started
processes: init spawned pid=0
shell: linux-abi ready
shell: getenv PATH: OK
shell: ext2 /disk/hello.txt: hello ext2 root fs
$
```

- `rm -rf build` 뒤 전체 빌드가 끝나고 `grub-file --is-x86-multiboot2`가 exit 0이다. 경고는 80과 같은 기존 경고(`nvme.c` `-Warray-bounds`, `vfs_ops_t` 필드 초기화, `init.c` 안 쓰는 함수, RWX 세그먼트)에 `socket.c`의 `vfs_ops_t` 필드 초기화 경고 하나가 늘었을 뿐이다.
- **소켓 fd 동작(에러 값, 참조 수, `dup2`/`fork` 상속, `conn`이 있는 `read`/`write`/`close` 위임)은 이 단계에서 검증하지 않는다.** 로드맵대로 전용 검증 프로그램을 두지 않았고, 82의 echo 클라이언트에서 확인한다. 구현 중 임시 유저 프로그램으로 `conn`이 없는 경로(`ENOTCONN`/`EPIPE`, 미지원 도메인·종류·프로토콜 에러, `SOCK_CLOEXEC`, `dup2`·`fork` 상속, 슬롯 8개 고갈과 해제)를 한 번 돌려 전부 통과했고 프로그램은 지웠다.

### 기존 서브시스템과의 정합 확인

- `sys_read`/`sys_write` 부호 확장이 기존 경로를 안 깨는지 확인했다. 부팅 시 `shell: ext2 /disk/hello.txt: hello ext2 root fs`(ext2 `read`), 셸 입력(콘솔 `read`), `forkclone`(`write`·`fork`·`wait4`)이 80과 같게 동작한다.
- 프로세스 종료는 `fds[]`를 전부 `vfs_close`하고 `fork`는 `vfs_dup`으로 복제하므로, 소켓이 `vfs_ops_t`를 따르기만 하면 프로세스 코드를 고칠 필요가 없었다. 임시 프로그램으로 자식 종료 뒤 부모의 `read`가 되는 것까지 확인했다.
- `vfs_ops_t` 형제 백엔드(ext2, initrd, console, pipe)와의 의미론은 `read`/`write` 반환 규약만 다르다 — 소켓이 음수 errno를 돌려준다. 다른 백엔드가 `u32` 바이트 수만 돌려주던 규약에 에러 표현이 새로 생긴 것이라, 나머지 백엔드를 errno로 바꾸는 건 이 단계 범위가 아니다.

## 이전 단계(80) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/socket.h`, `boot/socket.c` | 신규 | `socket_t` 고정 배열(`conn` 포함), `socket_ops`(`conn`이 있으면 `tcp_*`로 위임), `socket_create`, `socket_init` |
| `boot/syscall.h`, `boot/syscall.c` | 수정 | `SYS_SOCKET`(41), `sys_socket`, `SYS_READ`/`SYS_WRITE` 반환값 부호 확장 |
| `boot/vfs.c` | 수정 | `vfs_read`/`vfs_write`가 에러 반환이면 `pos`를 올리지 않음 |
| `boot/kernel.c` | 수정 | `socket_init()` 호출 |
| `Makefile` | 수정 | `socket.o` 빌드·링크, `socket.o`/`syscall.o`/`kernel.o` 의존성 |
| 나머지 전부 | 변경 없음 | 80의 파일 그대로 |

## 완료 기준

`make run`이 빌드되고 부팅해 `$` 프롬프트까지 간다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **82 `82-socket-client`**: `connect`가 `tcp_connect`로 `socket_t.conn`을 채운다. 그 순간부터 81이 만든 `read`/`write`/`close` 위임 경로가 처음 실행되므로 echo 클라이언트로 이 경로를 검증한다. 그 외 `shutdown`, `getsockname`/`getpeername`. 소켓 전용 시스콜은 `vfs_ops_t`가 아니라 `fd → vfs_file_t → socket_t`를 따라가는 별도 경로로 들어온다(리눅스가 `file_operations`와 `proto_ops`를 따로 두는 것과 같다).
- **블로킹의 전제**: `read`가 임시로 `-EAGAIN`을 돌려주는 것을 대기로 바꾸는 일이다. 연결 상태 변화·데이터 도착·FIN을 알리는 훅이 `tcp.c`에 없다(80에서 테스트용 `g_waiter` 깨움을 지움). `connect`/`read`를 `wq_add` → `thread_park` 루프로 블로킹하려면 `tcp_conn_t`에 이 알림을 새로 달아야 한다.
- **`sys_writev`**: `sys_write`의 `u32` 반환을 합산하므로 소켓에서 에러가 나면 총합이 어긋난다. 소켓 fd에 `writev`가 가는 바이너리를 올릴 때 고친다.
- **`lseek`**: 소켓 fd에서 `lseek`이 에러(리눅스는 `ESPIPE`)가 아니라 그냥 성공한다. 파이프도 같은 상태라 소켓만 따로 고치지 않았다. `vfs_seek`에 "탐색 불가" 표시를 도입할 때 파이프와 함께 처리한다.
- **`sys_read`/`sys_write`의 잘못된 fd**: `EBADF`가 아니라 0을 돌려준다(기존 동작).
- **미룬 검증**: `conn`이 있는 `read`/`write`/`close`와 TCP 실제 연결은 82에서 호스트 파이썬 echo 서버(10.0.2.2)와의 연결로 확인한다.
- **`SOCK_NONBLOCK`/`O_NONBLOCK`**: 지금은 `-EINVAL`이다. 82~83에서 논블로킹이 필요해지는지 보고 정한다.
