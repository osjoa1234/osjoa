# 47 — signal

**목표**: 프로세스마다 시그널 핸들러 테이블을 두고, 유저 공간 트램폴린 + `sigreturn`으로 핸들러를 실행한 뒤 원래 실행 흐름으로 복귀한다. 첫 시그널로 키보드 `Ctrl+C`를 `SIGINT`로 전달한다.

**46에서 이어짐**: 46까지는 프로세스가 어떤 syscall을 부르든 커널이 응답만 했다 — 커널이 프로세스 실행 흐름에 먼저 끼어드는 경우는 없었다. 시그널은 처음으로 그 방향이 뒤집히는 지점이다: 커널이 (키 입력 같은) 비동기 이벤트를 계기로 유저 코드의 `rip`를 강제로 바꿔치기하고, 유저 코드가 다시 커널에 "이제 원래대로 되돌려줘"(`rt_sigreturn`)라고 요청하면 그 흐름을 되돌린다.

## 핵심 개념

### 시그널 배달 지점: "링3로 돌아가기 직전"

인터럽트/syscall 핸들러는 항상 `struct interrupt_frame`을 스택에 만들어 두고 그걸 고쳐서 `iretq`로 되돌아간다(`interrupts.asm`/`syscall_entry.asm` 둘 다 44에서부터 동일한 레이아웃을 씀). 시그널을 배달한다는 건 결국 이 `frame->rip`/`frame->user_rsp`/`frame->rdi`를 핸들러 쪽으로 바꿔치기하는 것뿐이다. 그래서 배달 지점은 프레임을 마지막으로 만지고 `iretq`하기 직전, 딱 두 곳이다:

- `syscall_dispatch`의 스위치문이 끝난 직후 (`syscall.c`) — syscall이 끝나고 유저로 복귀하는 길목.
- `interrupt_dispatch`가 끝나는 지점 (`interrupts.c`) — IRQ(키보드/타이머)나 예외가 끝나고 유저로 복귀하는 길목.

`signal_deliver_pending(frame)`는 `frame->cs == 0x1B`(링3로 복귀하는 프레임)일 때만 동작한다. 커널 코드 실행 중(`cs == 0x08`)에 시그널이 pending 상태가 되어도 즉시 배달하지 않고 미뤄둔다 — 예를 들어 `sys_read`가 `con_read`의 `hlt` 루프 안에서 실제 문자를 기다리는 도중에 키보드 IRQ가 중첩으로 걸려 `Ctrl+C`가 눌려도, 그 중첩 IRQ 프레임의 `cs`는 커널 세그먼트라 배달이 미뤄지고 `sig_pending` 비트만 켜진다. 이건 이 구조의 알려진 한계다 — 진짜 블로킹 syscall 도중의 `Ctrl+C`는 그 syscall이 실제로 끝나야(문자를 실제로 입력해야) 배달된다. `47`의 검증 프로그램은 그래서 `sys_read`로 블록하는 대신 유저 공간에서 도는 바쁜 루프로 시그널을 맞는다 — 이 루프 안에서는 `Ctrl+C`가 곧장 링3 실행 중인 IRQ 프레임(`cs=0x1B`)으로 들어오므로 즉시 배달된다.

### "현재 실행 중인 스레드"가 곧 "포그라운드 프로세스"

작업 제어(job control)나 포그라운드 프로세스 그룹 개념은 아직 없다. 대신 `signal_raise_current`는 그 순간 CPU를 점유하고 있는 스레드(`thread_current()->user_data`)에 시그널을 건다. 이 커널은 부모가 자식을 기다릴 때 `thread_park()`로 완전히 스케줄에서 빠지므로(`proc_wait`), 셸이 자식을 기다리는 동안 `Ctrl+C`를 누르면 항상 실행 중인 자식이 맞는다 — 셸 자신이 `read`로 입력을 기다리는 중이면 셸이 맞는다. 단일 포그라운드 프로세스만 있는 지금 셸 모델에서는 이 단순한 규칙으로 충분하다.

### 유저 공간 트램폴린: 스택이 아니라 고정 페이지

핸들러가 끝나고(`ret`) 커널로 돌아올 진입점이 필요하다. 유저 스택 위에 트램폴린 코드를 얹는 전통적인 방식 대신, `PROC_USTACK_TOP` 바로 위에 전용 페이지 하나를 매핑해서 그 안에 고정 코드를 심었다(`signal_setup_trampoline`, `proc_spawn`/`proc_exec`에서 호출):

```
B8 0F 00 00 00    mov eax, 15   ; SYS_RT_SIGRETURN
0F 05             syscall
```

이 페이지는 NX 비트를 켜지 않은 현재 페이징 구조(`paging.c`가 `PTE_P|PTE_RW|PTE_US`만 씀) 덕에 실행 가능하다. `fork`는 `paging_copy_user_pages`가 매핑된 페이지를 통째로 복사하므로 트램폴린도 자동으로 따라간다 — 별도 처리가 필요 없다.

### 시그널 프레임: `frame->user_rsp`가 곧 컨텍스트 포인터

핸들러를 호출하기 직전 유저 스택에 저장하는 내용(`sigctx_t`, `signal.c`)은 `interrupt_frame`의 부분집합(범용 레지스터 15개 + `rip`/`rflags`/`user_rsp`, 18개 필드 × 8바이트 = 144바이트, 16바이트 정렬)이다. 스택에 쌓는 순서는:

```
[낮은 주소]                          [핸들러 진입 시 rsp, %16==8]
sigctx_t (144B, 16-정렬)  <- frame->user_rsp (배달 시점)
트램폴린 주소 (8B, "리턴 주소")      <- 핸들러 rsp
[높은 주소]
```

핸들러가 평범하게 `ret`하면 CPU가 트램폴린 주소를 팝하면서 그리로 점프하는데, 그 순간 rsp는 정확히 `sigctx_t`의 시작 주소로 돌아와 있다. 그래서 `sys_rt_sigreturn`은 별도의 포인터를 어딘가에 저장해 둘 필요 없이 `frame->user_rsp`를 그대로 `sigctx_t*`로 캐스팅해서 읽으면 된다 — syscall 진입 시 `syscall_entry.asm`이 항상 그 순간의 실제 유저 rsp를 `frame->user_rsp`에 담아주기 때문이다(44). 이 자기 서술적 레이아웃이 이번 구현에서 가장 중요한 트릭이다.

`sys_rt_sigreturn`은 다른 syscall과 달리 `syscall_dispatch`에서 `frame->rax = ...`를 쓰지 않는다 — `sigctx_t`가 이미 원래 `rax`까지 포함해서 프레임 전체를 되돌리므로, 그 위에 리턴값을 덮어쓰면 안 되기 때문이다.

### CR3를 안 바꾸는 동안은 유저 포인터가 곧 커널 포인터

`thread.c`의 `activate_thread`는 스레드가 스케줄될 때만 `cr3`를 바꾼다. syscall이나 IRQ 핸들러가 도는 동안은 그 스레드가 마지막에 로드한 프로세스의 페이지 디렉터리가 그대로 유지된다. 그래서 `sys_rt_sigaction`이 `frame->rsi`(유저의 `sigaction*`)를 그냥 역참조해도, `deliver_signal`이 `frame->user_rsp` 근방에 `sigctx_t`를 그냥 써도 안전하다 — 별도의 유저 가상주소→커널 포인터 변환이 필요 없다. 기존 `sys_write`/`sys_read`가 `frame->rdi`/`frame->rsi`를 그대로 포인터로 캐스팅해 쓰던 것과 같은 전제다.

### `rt_sigaction`/`rt_sigprocmask`: 커널 ABI 그대로

`sigaction_t{handler, flags, restorer, mask}` 32바이트, `sigsetsize`로 넘어오는 시그널 마스크는 8바이트(x86_64 커널 sigset, `_NSIG_WORDS=1`) — 이건 리눅스 x86_64 syscall ABI 그대로다. `sa_restorer`/`sa_flags`는 읽기만 하고 무시한다 — 커널이 어차피 자기 트램폴린 주소를 강제로 쓰기 때문에, `SA_RESTORER` 플래그나 사용자가 넘긴 restorer 값이 맞든 틀리든 상관없다. `sa_mask`(핸들러 실행 중 자동 블록)도 지금은 반영하지 않는다 — `sig_blocked`는 오직 `rt_sigprocmask`로만 바뀐다. 다음 단계 힌트에 남겨둔다.

### `proc_exec`은 핸들러를 초기화하고, `proc_fork`는 물려받는다

POSIX 규칙을 최소한으로 따랐다: `execve`는 캐치하던 핸들러를 전부 `SIG_DFL`로 되돌린다(옛 핸들러 코드가 새 이미지엔 없으므로) — `signal_reset_handlers`. `fork`는 핸들러 테이블과 `sig_blocked`를 그대로 복사하되, `sig_pending`은 새로 0에서 시작한다(부모의 미처리 시그널을 자식이 물려받지 않음). `proc_alloc`이 모든 새 `process_t`의 시그널 상태를 일단 0으로 밀어준 뒤, `proc_fork`가 그 위에 필요한 것만 덮어쓴다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 이전과 동일하게 `$` 프롬프트에서 멈춘다(`initramfs: N file(s) found`의 파일 수만 10→11로 늘어난다). GUI(`make run`)에서:

```
$ signal
signal: installing SIGINT handler
signal: looping -- press Ctrl+C three times
```

이후 `Ctrl+C`를 세 번 누르면 매번:

```
signal: SIGINT caught
```

이 찍히고(핸들러가 실행되고 `sigreturn`으로 루프에 정확히 복귀한다는 뜻), 세 번째 이후:

```
signal: caught 3 SIGINT, exiting
process 1 exited: code=0
$
```

로 정상 종료한다. `hello`/`hello2`/`brk`/`mmap`/`tls`/`syscall64`/`musl_hello`도 그대로 동작해야 한다(회귀 없음) — QEMU 모니터의 `sendkey`(HMP)를 스크립트로 순서대로 넣어 확인했다(`signal` 3회 `ctrl-c` 포함).

## 이전 단계(46) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/signal.h` | 신규 | `SIGINT`/`SIG_DFL`/`SIG_IGN`/`SIG_BLOCK`류 상수, `sigaction_t`, 시그널 관련 함수 선언 |
| `boot/signal.c` | 신규 | 트램폴린 페이지 설치(`signal_setup_trampoline`), pending 표시(`signal_raise_current`), 배달(`signal_deliver_pending`/`deliver_signal`), `sys_rt_sigaction`/`sys_rt_sigprocmask`/`sys_rt_sigreturn` |
| `boot/process.h` | 수정 | `NSIG` 정의, `process_t`에 `sig_handler[NSIG]`/`sig_pending`/`sig_blocked` 추가 |
| `boot/process.c` | 수정 | `#include "signal.h"`; `proc_alloc`에서 시그널 상태 초기화; `proc_spawn`/`proc_exec`에서 트램폴린 설치(`proc_exec`은 핸들러도 리셋); `proc_fork`에서 핸들러 테이블/`sig_blocked` 상속 |
| `boot/syscall.h` | 수정 | `SYS_RT_SIGACTION`(13)/`SYS_RT_SIGPROCMASK`(14)/`SYS_RT_SIGRETURN`(15) 추가 |
| `boot/syscall.c` | 수정 | `#include "signal.h"`; 세 syscall 디스패치 케이스 추가; `syscall_dispatch` 끝에서 `signal_deliver_pending` 호출 |
| `boot/interrupts.c` | 수정 | `#include "signal.h"`; `interrupt_dispatch` 끝에서 `signal_deliver_pending` 호출하도록 제어 흐름 재구성 |
| `boot/keyboard.c` | 수정 | `#include "signal.h"`; `ctrl_held` 상태 추가, 스캔코드 `0x1D`/`0x9D`로 Ctrl 추적, `Ctrl` 눌린 채 `c`(`0x2E`) 감지 시 `signal_raise_current(SIGINT)` |
| `user/signal.c` | 신규 | `rt_sigaction`으로 `SIGINT` 핸들러 설치 후 바쁜 루프를 돌며 `Ctrl+C` 3회를 맞고(`sigreturn`으로 매번 루프 복귀) 정상 종료하는 검증 프로그램 |
| `Makefile` | 수정 | `SIGNALOBJ`(`boot/signal.o`) 빌드·링크 추가, 관련 `.c` 룰에 `boot/signal.h` 의존성 반영, `USERSIGNAL`(`user/signal.c`→`build/signal`) 빌드·링크·initramfs 포함·clean 반영 |
| `initrd/.gitignore` | 수정 | `signal` 패턴 추가 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `sa_mask`(핸들러 실행 중 다른 시그널 자동 블록)를 반영하지 않는다 — `sig_handler[]`와 별도로 시그널별 마스크를 저장하고, `deliver_signal`에서 `sig_blocked`에 OR, `sys_rt_sigreturn`에서 원래 값으로 복원해야 한다.
- 진짜 블로킹 syscall(`sys_read`가 `con_read`의 `hlt` 루프 안에 있을 때) 도중에는 `Ctrl+C`가 그 syscall이 실제로 끝나야(EOF든 다른 입력이든) 배달된다 — 실제 유닉스처럼 블로킹 syscall을 `EINTR`로 즉시 깨우려면 `con_read`가 자기 자신도 `sig_pending`을 매 루프마다 확인하고 조기 리턴하도록 고쳐야 한다. `48-pipe`에서 파이프 읽기도 비슷한 블로킹 지점이 생기므로 그때 같이 손볼 수 있다.
- 시그널 번호는 `NSIG=32`(비트마스크 1워드)로 제한돼 있다 — 리얼타임 시그널(32번 이상)이 필요해지면 `sig_pending`/`sig_blocked`를 `u64[2]`로 늘려야 한다.
- `SIGKILL`은 사용자가 임의로 무시/블록하지 못하게 강제해야 하는 게 원칙(POSIX)인데, 지금은 다른 시그널과 동일하게 취급된다 — 아직은 아무 프로그램도 강제 종료가 필요한 상황을 만들지 않아서 미룬 것.
- 여러 스레드(`proc_clone`)가 있는 프로세스에서 시그널은 "그 순간 실행 중이던 아무 스레드"에게 배달된다 — 실제로는 시그널마스크가 스레드별로 다를 수 있고 배달 대상 선택도 더 정교해야 하지만, 지금은 스레드 전체가 `process_t`를 공유하는 것 이상으로 손대지 않았다.
