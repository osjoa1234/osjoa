# 29 — wait-queue

**목표**: wait queue를 연결 리스트로 교체하고, `waitpid(-1)`으로 임의 자식을 대기할 수 있게 한다.

**28에서 이어짐**: 28에서 `SYS_FORK`로 fork+exec 패턴을 완성했다. 여기서는 단일 포인터였던 `waiter` 필드를 연결 리스트 기반의 wait queue로 교체하고, `proc_wait(-1)`로 임의 자식을 대기하는 `waitpid(-1)` 의미를 추가한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### wait_queue_t — 연결 리스트 기반 대기 큐

28의 `thread_t *waiter`는 단일 스레드만 기다릴 수 있었다. 이를 `wq_node_t` 노드의 연결 리스트로 교체해 여러 스레드가 동시에 대기할 수 있게 한다.

```
wait_queue_t { head → wq_node_t { thread, next } → wq_node_t { thread, next } → NULL }
```

- `wq_add(wq, t)` — 현재 스레드를 큐에 추가
- `wq_wake_all(wq)` — 큐의 모든 스레드를 unpark하고 노드를 해제

### process_t 구조 변경

`process_t`에 두 종류의 wait queue를 추가한다.

| 필드 | 역할 |
|------|------|
| `waiters` | 이 프로세스의 종료를 기다리는 스레드 목록 (특정 pid wait) |
| `any_child_waiters` | 이 프로세스의 임의 자식 종료를 기다리는 스레드 목록 (waitpid -1) |

`parent_pid` 필드를 추가해 프로세스 간 부모-자식 관계를 기록한다. `proc_spawn`으로 생성된 프로세스는 `PROC_NO_PARENT`((u32)-1), `proc_fork`로 생성된 자식은 부모의 pid를 가진다.

### proc_exit — 두 종류 wakeup

```
1. p->state = PROC_ZOMBIE
2. wq_wake_all(&p->waiters)               → 이 pid를 기다리던 스레드 깨움
3. parent = proc_get(p->parent_pid)
4. wq_wake_all(&parent->any_child_waiters) → 부모가 waitpid(-1) 중이면 깨움
```

### proc_wait — waitpid(-1) 흐름

`pid == (u32)-1U`이면 "임의 자식 대기" 경로로 진입한다.

```
for (;;) {
    zombie = find_zombie_child(caller->pid);  // 이미 죽은 자식이 있으면 즉시 회수
    if (zombie) break;
    wq_add(&caller->any_child_waiters, current_thread);
    thread_park();                            // 자식 종료 때까지 대기
}
child_pid = zombie->pid;
zombie->state = PROC_FREE;
return child_pid;                             // 회수한 자식의 pid 반환
```

특정 pid를 기다리는 경우에도 `p->waiter` 단일 포인터 대신 `wq_add(&p->waiters, ...)`로 큐에 등록한다.

### 실행 흐름

```
processes: init spawned pid=0
init (pid=0): sys_write("init: before fork")
init (pid=0): sys_fork() → child1 pid=1 생성
  child1 (pid=1): "child1: exec hello" → sys_exec("hello")
  hello: "Hello from hello process!" → sys_exit(42)
  process 1 exited: code=42  → wq_wake_all(init->any_child_waiters)
init (pid=0): sys_fork() → child2 pid=2 생성
  child2 (pid=2): "child2: exec hello2" → sys_exec("hello2")
  hello2: "Hello from fork+exec'd hello2!" → sys_exit(0)
  process 2 exited: code=0  → wq_wake_all(init->any_child_waiters)
init (pid=0): sys_wait(-1) × 2 → 두 자식 모두 회수
init (pid=0): "init: both children done" → sys_exit(0)
process 0 exited: code=0  → wq_wake_all(pid=0->waiters) → kernel_main 깨움
processes: init exited code=0
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
init: before fork
child1: exec hello
Hello from hello process!
process 1 exited: code=42
child2: exec hello2
Hello from fork+exec'd hello2!
process 2 exited: code=0
init: both children done
process 0 exited: code=0
processes: init exited code=0
```

(child1/child2 출력 순서는 스케줄링에 따라 달라질 수 있다.)

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/wait_queue.h` | 신규 | `wq_node_t`, `wait_queue_t` 구조체 + `wq_init/add/wake_all` 선언 |
| `boot/wait_queue.c` | 신규 | `wq_add` (kmalloc 노드 prepend), `wq_wake_all` (노드 순회 unpark·kfree) |
| `boot/process.h` | 수정 | `waiter` 제거 → `waiters`/`any_child_waiters` (wait_queue_t), `parent_pid` 추가 |
| `boot/process.c` | 수정 | `proc_alloc` — wq_init 호출; `proc_fork` — `parent_pid` 기록; `proc_exit` — wq_wake_all 두 번; `proc_wait` — waitpid(-1) 분기 추가 |
| `Makefile` | 수정 | `wait_queue.o` 빌드 추가; `hello` 프로그램 빌드·initrd 포함 추가 |
| `user/init.c` | 수정 | child1(hello)/child2(hello2) 두 자식 fork, `sys_wait(-1)` 두 번으로 대기 |
| `initrd/.gitignore` | 수정 | `hello2` 추가 |

## 다음 단계 힌트

- `30-thread-clone`: `process_t` 1:N 스레드 구조, `SYS_CLONE`, 스레드별 독립 유저스택
