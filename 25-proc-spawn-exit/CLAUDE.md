# 25 — proc-spawn-exit

**목표**: 프로세스 테이블(`process_t`)을 도입하고, `proc_spawn`/`proc_exit`로 프로세스가 태어나고 죽는 흐름을 구현한다.

**24에서 이어짐**: 24에서 kernel.c가 직접 `paging_clone_dir → elf_load_process → paging_set_dir → enter_user_mode`를 순서대로 호출했다. 여기서는 그 과정을 `proc_spawn(name)`으로 캡슐화하고, 프로세스 슬롯(pid, state, exit_code)을 관리하는 proc_table을 추가한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### process_t와 proc_table

```c
typedef struct {
    u32       pid;
    u32       state;       // PROC_FREE / PROC_RUNNING / PROC_ZOMBIE
    u32       exit_code;
    u32       entry;
    u32       pd_phys;
    thread_t *thread;
} process_t;

static process_t proc_table[PROC_MAX];
```

`proc_alloc()`: 첫 번째 PROC_FREE 슬롯을 PROC_RUNNING으로 바꾸고 반환. pid = 슬롯 인덱스.

### proc_spawn(name)

```
proc_alloc() → 슬롯 확보
initrd_open(name) → ELF 데이터 접근
paging_clone_dir() → 프로세스 전용 페이지 디렉토리
elf_load_process() → ELF 세그먼트를 전용 프레임에 적재
thread_create_with_data(proc_run_trampoline, p) → 스케줄러에 추가
t->pd = pd_phys → 컨텍스트 스위치 시 올바른 CR3 로드
반환: p->pid
```

### proc_run_trampoline

```c
static void proc_run_trampoline(void)
{
    process_t *p = (process_t *)thread_current()->user_data;
    enter_user_mode(p->entry, PROC_USTACK_TOP);
}
```

`thread->user_data`에서 process_t를 꺼내 `enter_user_mode`를 호출한다. activate_thread가 이미 t->pd를 CR3에 적재한 뒤 switch_context가 이 함수로 진입하므로 paging_set_dir을 별도로 호출할 필요가 없다.

### activate_thread와 TSS.esp0

모든 컨텍스트 스위치 직전에 `activate_thread(next)` 호출:
1. `gdt_set_kernel_stack(t->kstack_top)`: TSS.esp0 갱신. user mode에서 인터럽트 발생 시 CPU가 TSS.esp0으로 커널 스택을 교체하므로, 프로세스별 커널 스택으로 매 스위치마다 갱신 필요.
2. `t->pd != 0`이면 `paging_set_dir(t->pd)`, 아니면 `paging_restore_kernel_dir()`.

### proc_exit(code)

SYS_EXIT syscall이 `proc_exit(frame->ebx)`를 호출한다.

```c
void proc_exit(u32 code)
{
    process_t *p = (process_t *)thread_current()->user_data;
    p->state     = PROC_ZOMBIE;
    p->exit_code = code;
    thread_exit();
    for (;;) {}
}
```

`thread->user_data`가 process_t를 가리키므로 현재 스레드만으로 소속 프로세스를 찾는다.

### wake_sleeping do-while 수정

이전 thread.c의 `wake_sleeping`은 idle_task를 체크하지 않았다. idle_task(kernel_main)가 `thread_sleep`을 호출하면 영원히 깨어나지 못하는 잠재적 버그. do-while로 수정해 idle_task 자신도 wake 대상에 포함한다(23-4의 proc_wait에서 필요).

### kernel.c 흐름

```
threads_init((u32)&stack_top)  ← idle_task 커널 스택 등록
proc_init()                    ← proc_table 초기화
thread_create(task_a/b)        ← 커널 스레드
hlt loop (A, B 완료 대기)
proc_spawn("init")             ← 프로세스 0 생성
hlt loop (init 완료 대기)
proc_get(init_pid) → exit_code 출력
```

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 (3초 후 종료)
make clean
```

## 완료 기준

```
processes: init spawned pid=0
init: running as process 0
process 0 exited: code=0
processes: init exited code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/process.h` | 신규 | `process_t`, `PROC_*` 상수, `proc_*` 선언 |
| `boot/process.c` | 신규 | `proc_init/alloc/spawn/exit`, `proc_run_trampoline` |
| `boot/gdt.h` | 수정 | `gdt_set_kernel_stack(u32 esp0)` 선언 추가 |
| `boot/gdt.c` | 수정 | `gdt_set_kernel_stack`: `tss.esp0 = esp0` |
| `boot/thread.h` | 수정 | `kstack_top`, `pd`, `user_data` 필드; `threads_init(u32)` 서명; `thread_create_with_data` 추가 |
| `boot/thread.c` | 수정 | `activate_thread`; `wake_sleeping` do-while 수정; `thread_create_with_data` |
| `boot/syscall.h` | 수정 | `SYS_SPAWN=4` 추가 |
| `boot/syscall.c` | 수정 | `SYS_EXIT` → `proc_exit`; `SYS_SPAWN` 핸들러 추가 |
| `boot/kernel.c` | 수정 | `elf.h` 제거; `process.h` 추가; `threads_init((u32)&stack_top)`; `proc_init`; `proc_spawn/proc_get` |
| `user/init.c` | 수정 | 메시지 변경: "init: running as process 0" |
| `Makefile` | 수정 | `process.o` 추가; thread.c/syscall.c/kernel.c 의존성 갱신 |

## 다음 단계 힌트

- `26-proc-wait`: `proc_wait`, `SYS_WAIT`, 자식 프로세스(`hello`) — 부모가 자식 종료를 기다리는 흐름
