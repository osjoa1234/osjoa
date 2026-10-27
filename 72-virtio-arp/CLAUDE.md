# 72 — virtio-arp

**목표**: 71에서 `DRIVER_OK`까지 올려둔 virtio-net 장치로 처음 패킷을 주고받는다. transmitq에 virtio-net 헤더 + raw ARP 요청 프레임을 올려 QEMU user 게이트웨이(`10.0.2.2`)에 보내고, receiveq의 used ring으로 돌아온 ARP 응답을 읽는다. 이더넷/ARP 계층(구조체, ARP 캐시, 요청/응답 처리 로직)은 73에서 만들고, 이번 단계는 프레임을 바이트 단위로 박아 넣어 드라이버(송신, 수신, 인터럽트)만 검증한다.

## 1) 송신 (`virtio_send_arp_request`)

프레임 버퍼는 `page_alloc()`으로 받은 한 페이지(0으로 채움) 안에 앞에서부터 다음 순서로 쓴다.

- virtio-net 헤더 12바이트: 전부 0 (offload 없음).
- 이더넷 헤더 14바이트: 목적지 MAC `ff:ff:ff:ff:ff:ff`(브로드캐스트) 6 + 출발지 MAC(device cfg에서 읽은 것) 6 + ethertype `0x0806` 2.
- ARP 패킷 28바이트: hardware type 1(이더넷) 2 + protocol type `0x0800`(IPv4) 2 + hardware size 6 1 + protocol size 4 1 + opcode 1(요청) 2 + sender MAC 6 + sender IP `10.0.2.15` 4 + target MAC 0 6 + target IP `10.0.2.2` 4.

합 `12 + 14 + 28 = 54`바이트다. 정수 필드는 네트워크 바이트 순서(빅 엔디안)라 `put_be16`/`get_be16`으로 쓰고 읽는다. `10.0.2.15`는 QEMU user 네트워크가 게스트에게 DHCP로 주는 기본 주소인데, DHCP를 아직 안 하므로 고정으로 쓴다.

transmitq descriptor 0이 이 버퍼를 가리키고(`flags = 0`: 장치가 읽는 버퍼, 71의 receiveq는 `WRITE`), available ring에 descriptor 번호를 넣은 뒤 `avail->idx`를 올리고 `virtqueue_notify`로 doorbell을 울린다. 링 내용 → `idx` 순서를 컴파일러 배리어로 보장하는 건 71과 같다.

## 2) 수신 (`virtio_rx_irq`, `virtio_consume_rx`, `virtio_handle_rx_frame`)

드라이버가 마지막으로 처리한 used ring 위치를 `virtqueue.used_seen`(신규)으로 들고 있다가, `used->idx`와 다르면 `used->ring[used_seen % size]`의 `id`(descriptor 번호)와 `len`(장치가 쓴 바이트 수)을 읽는다. `id × 2048`이 패킷 버퍼 영역 안 오프셋이다. 앞 12바이트(virtio-net 헤더)를 건너뛴 이더넷 헤더의 ethertype이 ARP이고 opcode가 2(응답)면 sender IP/MAC을 출력한다. 처리한 descriptor는 available ring에 다시 올려(재게시) receiveq가 고갈되지 않게 하고 마지막에 한 번 notify한다. 71의 "재게시는 72에서" 미룬 항목이 이것이다.

`virtio_rx_irq`(MSI-X 엔트리 0)는 `irq_count`를 올린 뒤 `virtio_consume_rx`를 호출해 used ring을 소비한다. 그래서 `g_arp_reply_seen`은 MSI-X 핸들러 안에서만 켜진다. `virtio_tx_irq`는 `irq_count`만 올린다. 핸들러는 `virtio_consume_rx` 뒤에 정의한다.

## 3) 대기 (`virtio_net_arp_test`, `virtio_wait_arp_reply`)

`virtio_net_init`은 장치 초기화와 `DRIVER_OK`까지만 하고 `g_driver_ready`를 켠다. ARP 송신과 응답 대기는 `virtio_net_arp_test`로 분리해 `kernel.c`에서 `threads_init`/`proc_init`/`pipe_init` 뒤, `proc_spawn("/init")` 앞에 호출한다. 그 시점엔 `timer_init`(PIT 100Hz)과 `interrupts_enable()`이 끝나 IF가 켜져 있어서, 인터럽트를 잠깐 열었다 닫는 우회가 필요 없다.

`virtio_wait_arp_reply`는 `nvme_wait_irq`와 같은 `cli` → 조건 확인 → 대기 루프다. 응답 처리는 이 루프가 아니라 `virtio_rx_irq`가 한다. 종료 조건은 ARP 응답을 받았고 tx used가 따라왔고 rx/tx 인터럽트가 모두 한 번 이상 온 것이고, 타임아웃은 `timer_ticks()`가 마감(1000ms = `timer_hz()` tick 수)을 넘는 것이다.

`virtio_net_arp_test`는 `threads_init` 뒤에서만 부르므로 `thread_current()`는 항상 있다. 그래서 `nvme_wait_irq`와 달리 스레드 유무 분기 없이 `g_arp_waiter`에 현재 스레드를 적고 마감까지 남은 시간(`(deadline - timer_ticks()) * 1000 / timer_hz()` ms)만큼 `thread_sleep`한다. `virtio_rx_irq`/`virtio_tx_irq`는 `g_arp_waiter`가 있으면 `thread_unpark`로 깨운다. `thread_unpark`는 상태를 `RUNNING`으로 바꾸기만 하므로 SLEEPING인 스레드도 일찍 깨울 수 있고, 안 깨워져도 `thread_sleep`의 `wake_tick`이 타임아웃 역할을 한다. 이 두 성질 때문에 `nvme_wait_irq`처럼 `thread_park`를 쓰지 않는다: park는 `thread_unpark`로만 깨어나서 응답이 안 오면 영영 못 깨어난다. 조건 확인 뒤 `thread_sleep`에 들어가기 전에 인터럽트가 와서 깨움을 놓치는 일은 `thread_sleep`이 상태를 SLEEPING으로 바꾼 뒤에야 인터럽트를 켜고 `hlt`하는 구조라 `unpark`가 그 뒤에 와도 상태를 되돌려 막힌다.

송신 로그는 `virtqueue_notify` 앞에서 찍는다. 콘솔 출력은 재진입이 안 되는데, IF가 켜진 상태라 notify 직후 도착한 수신 인터럽트가 송신 로그 출력 도중에 끼어들면 두 줄이 섞이기 때문이다.

이전 구현(`timer_init` 전에 `sti; nop×4; cli`를 2,000,000번 반복)은 폐기했다. 인터럽트를 받는 시간 창이 짧아 비동기로 도착하는 패킷이 걸리기만을 기대하는 구조였고, `nop` 개수로 "고쳐지는" 것은 원인을 확인하지 못한 우회였다. 그 구조에서 `rx=0 tx=0`이 나왔다는 관찰 자체는 있었지만, MSI-X 테이블/컨트롤 설정은 정상이었고 원인은 확인하지 못했다.

## 검증

`make clean && make run-nogui`(관련 구간):

```
virtio-net: driver ready status=0x0F rx used idx=0 tx used idx=0
...
nvme: io completions via msix: 4 interrupt(s)
...
virtio-net: tx arp who-has 10.0.2.2 tell 10.0.2.15 (54 bytes)
virtio-net: rx arp reply 10.0.2.2 is-at 52:55:0A:00:02:02 (76 bytes)
virtio-net: tx used idx=1 rx used idx=1
virtio-net: msix interrupts rx=1 tx=1
processes: init spawned pid=0
...
shell: ext2 /disk/hello.txt: hello ext2 root fs
```

게이트웨이가 `52:55:0A:00:02:02`(QEMU slirp 고정 MAC)로 응답했고, tx/rx used idx가 각각 1이 되고, 71에서 미룬 MSI-X 인터럽트 도달(벡터 `0x32`/`0x33`)이 처음 확인됐다. `grub-file --is-x86-multiboot2 build/kernel.elf` exit 0, `virtio_net.c` 컴파일 경고 없음, 3회 연속 같은 출력(다른 파일의 경고는 이전 단계부터 있던 것). 응답 프레임 길이가 76바이트로 우리가 보낸 54바이트보다 긴 이유(응답 뒤에 붙는 바이트)는 확인하지 않았다.

### 기존 서브시스템과의 정합 확인

- **물리 할당기**: 송신 프레임 버퍼를 `page_alloc()`으로 받아 비트맵/`free_count`가 일관된다. 송신 후에도 해제하지 않고 유지한다(한 번뿐이라 누수가 아니고, 73에서 송신 버퍼 관리가 생긴다).
- **인터럽트 상태**: 대기 루프는 진입 시 `rflags`의 IF를 저장해 두고 끝나면 복원한다(`nvme_wait_irq`와 같다). IF가 켜진 채 시작하고 켜진 채 돌아온다.
- **스케줄러**: 호출 시점에 스레드는 idle(부팅 스레드)뿐이라 `thread_sleep`이 `next == prev` 갈래(`hlt` 루프)로 떨어지고, 실제 문맥 전환은 일어나지 않는다. 다른 스레드가 생겼을 때 문맥 전환이 끼는 경로는 아직 실행으로 검증되지 않았다.
- **NVMe/ext2**: I/O 완료 인터럽트 4개, ext2 출력이 71과 같다.
- **receiveq 재게시**: 처리한 버퍼를 되돌려 receiveq가 16개 버퍼를 유지한다.

## 이전 단계(71) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/virtio_net.h` | 수정 | `virtio_net_arp_test` 선언 |
| `boot/virtio_net.c` | 수정 | ARP 요청 프레임 조립과 transmitq 송신, used ring 소비와 receiveq 재게시, ARP 응답 출력, `thread_sleep` + `thread_unpark` 대기, PIT 기반 타임아웃, `virtio_net_arp_test` |
| `boot/kernel.c` | 수정 | `pipe_init()` 뒤에 `virtio_net_arp_test()` 호출 |
| 나머지 전부 | 변경 없음 | 71의 파일 그대로 |

## 완료 기준

`make clean && make run-nogui`에서 위 검증 출력(`tx arp who-has` → `rx arp reply ... is-at 52:55:0A:00:02:02`, `tx used idx=1 rx used idx=1`, `msix interrupts rx=1 tx=1`)이 나오고 NVMe/ext2/셸 출력이 71과 같아야 한다. `grub-file --is-x86-multiboot2`가 exit 0이어야 한다.

## 다음 단계 힌트

- **73 `73-ethernet-arp`**: 박아 넣은 바이트 대신 이더넷 헤더/ARP 구조체와 파싱 함수, ARP 캐시를 만든다. 지금 `virtio_handle_rx_frame`이 하는 ethertype 분기와 ARP 응답 출력이 거기로 옮겨 간다.
- **송신 버퍼 관리 없음**: transmitq descriptor 0과 버퍼 한 페이지를 한 번만 쓴다. 여러 프레임을 보내려면 free descriptor 관리와 used ring으로 반환된 버퍼 회수가 필요하다.
- **수신 처리 위치**: used ring 소비와 프레임 출력이 MSI-X 핸들러(인터럽트 컨텍스트) 안에서 일어난다. 콘솔이 재진입 불가라 핸들러 출력이 메인 흐름 출력과 섞일 수 있다(송신 로그를 notify 앞으로 옮겨 피했다). 수신 경로가 커지는 73 이후엔 핸들러는 큐에 넣기만 하고 처리는 스레드에서 하는 구조를 판단한다.
- **게스트 IP 고정**: `10.0.2.15`를 하드코딩했다. DHCP는 로드맵 범위 밖이라 계속 고정으로 간다.
