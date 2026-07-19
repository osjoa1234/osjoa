# 26 — proc-wait

**목표**: `proc_wait`을 구현해 부모 프로세스(init)가 자식(hello)의 종료를 기다리는 흐름을 완성한다.

**25에서 이어짐**: 25에서 kernel.c가 proc_spawn 후 hlt 루프 + proc_get으로 종료를 확인했다. 여기서는 `proc_wait(pid, *exit_code)`를 추가해 부모도 자식을 기다릴 수 있게 한다. init이 hello를 spawn하고 wait하는 표준 UNIX 흐름을 완성한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### proc_wait(pid, *exit_code)

```c
u32 proc_wait(u32 pid, u32 *exit_code)
{
    process_t *p = proc_get(pid);
    if (!p) return (u32)-1U;

    while (p->state != PROC_ZOMBIE) {
        thread_sleep(10U);
    }

    if (exit_code) *exit_code = p->exit_code;
    p->state = PROC_FREE;
    return 0U;
}
```

`thread_sleep(10ms)` 루프로 ZOMBIE가 될 때까지 폴링한다. 대기 중에는 CPU를 다른 스레드에 양보한다.

### 25 wake_sleeping 수정이 여기서 필요한 이유

kernel_main(idle_task)가 `proc_wait → thread_sleep(10ms)` 루프를 돈다. 23-3에서 `wake_sleeping`을 do-while로 수정해 idle_task 자신도 wake 대상에 포함시켰기 때문에, kernel_main이 잠든 뒤 타이머 인터럽트로 깨어날 수 있다.

init도 `proc_wait(hello_pid, &code)` 안에서 `thread_sleep(10ms)`를 호출한다. init의 스레드는 idle_task가 아니므로 기존 while 루프에도 포함되지만, do-while 수정이 일관성을 보장한다.

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
| `boot/process.h` | 수정 | `proc_wait(u32 pid, u32 *exit_code)` 선언 추가 |
| `boot/process.c` | 수정 | `proc_wait` 구현; `timer.h` include 추가 |
| `boot/syscall.h` | 수정 | `SYS_WAIT=5` 추가 |
| `boot/syscall.c` | 수정 | `SYS_WAIT` 핸들러 추가 |
| `user/init.c` | 수정 | `sys_spawn`/`sys_wait` 추가; hello 생성 후 wait |
| `user/hello.c` | 신규 | "Hello from hello process!" 출력 후 code=42로 exit |
| `Makefile` | 수정 | `hello` 빌드 타깃; initramfs에 hello 추가; clean에 hello 제거; timeout 5초 |

## 다음 단계 힌트

- `29-vfs`: 파일 디스크립터, 경로 해석, 단순 VFS 계층
