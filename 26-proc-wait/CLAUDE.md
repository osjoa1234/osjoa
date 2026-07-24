# 26 — proc-wait

**목표**: `proc_wait`을 구현해 부모 프로세스(init)가 자식(hello)의 종료를 기다리는 흐름을 완성한다.

**25에서 이어짐**: 25에서 kernel.c가 proc_spawn 후 hlt 루프 + proc_get으로 종료를 확인했다. 여기서는 `proc_wait(pid, *exit_code)`를 추가해 부모도 자식을 기다릴 수 있게 한다. init이 hello를 spawn하고 wait하는 표준 UNIX 흐름을 완성한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### thread_park / thread_unpark

```c
void thread_park(void)   // 현재 스레드를 THREAD_PARKED 상태로 전환 후 양보
void thread_unpark(thread_t *t)  // t->state = THREAD_RUNNING
```

`THREAD_PARKED`는 `wake_sleeping`(타이머 기반)이 건드리지 않는 별도 상태다. 오직 `thread_unpark` 호출로만 깨어난다.

### proc_wait(pid, *exit_code)

```c
u32 proc_wait(u32 pid, u32 *exit_code)
{
    process_t *p = proc_get(pid);
    if (!p) return (u32)-1U;

    p->waiter = thread_current();
    if (p->state != PROC_ZOMBIE)
        thread_park();

    if (exit_code) *exit_code = p->exit_code;
    p->state  = PROC_FREE;
    p->waiter = 0;
    return 0U;
}
```

waiter를 먼저 등록한 뒤 상태를 확인한다. 자식이 이미 ZOMBIE면 park 없이 바로 회수한다.

### proc_exit에서의 thread_unpark

```c
p->state     = PROC_ZOMBIE;
p->exit_code = code;
if (p->waiter)
    thread_unpark(p->waiter);
thread_exit();
```

ZOMBIE로 전환 직후 waiter를 깨운다. waiter가 아직 `thread_park`에 진입하지 않았더라도(race), waiter는 park 직전에 이미 ZOMBIE를 확인하고 park를 건너뛴다. 인터럽트가 비활성화된 구간에서 `thread_park`가 state를 PARKED로 바꾸므로 양쪽 경로 모두 안전하다.

### 실행 흐름

```
kernel_main: proc_wait(init_pid)
  → init 실행: sys_spawn("hello") → proc_spawn("hello") → pid=1
  → init: sys_wait(1, &code) → proc_wait(1)
    → hello 실행: "Hello from hello process!" → sys_exit(42)
    → proc_exit(42): hello ZOMBIE, exit_code=42
    → proc_wait(1) 반환, hello PROC_FREE
  → init: "init: hello done" → sys_exit(0)
  → proc_exit(0): init ZOMBIE, exit_code=0
kernel_main proc_wait(init_pid): init ZOMBIE → exit_code=0, init PROC_FREE
"processes: init exited code=0"
```

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 (5초 후 종료)
make clean
```

## 완료 기준

```
processes: init spawned pid=0
init: spawning hello
init: waiting for hello
Hello from hello process!
process 1 exited: code=42
init: hello done
process 0 exited: code=0
processes: init exited code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/thread.h` | 수정 | `THREAD_PARKED` 상수; `thread_park`/`thread_unpark` 선언 |
| `boot/thread.c` | 수정 | `thread_park`/`thread_unpark` 구현 |
| `boot/process.h` | 수정 | `process_t`에 `waiter` 필드; `proc_wait` 선언 추가 |
| `boot/process.c` | 수정 | `proc_alloc`에서 waiter 초기화; `proc_wait` park/unpark 방식으로 구현; `proc_exit`에서 `thread_unpark` 호출 |
| `boot/syscall.h` | 수정 | `SYS_WAIT=5` 추가 |
| `boot/syscall.c` | 수정 | `SYS_WAIT` 핸들러 추가 |
| `user/init.c` | 수정 | `sys_spawn`/`sys_wait` 추가; hello 생성 후 wait |
| `user/hello.c` | 신규 | "Hello from hello process!" 출력 후 code=42로 exit |
| `Makefile` | 수정 | `hello` 빌드 타깃; initramfs에 hello 추가; clean에 hello 제거; timeout 5초 |

## 다음 단계 힌트

- `29-vfs`: 파일 디스크립터, 경로 해석, 단순 VFS 계층
