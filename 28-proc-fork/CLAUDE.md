# 28 — proc-fork

**목표**: `SYS_FORK`를 구현해 부모의 주소 공간을 복제한 자식을 생성한다. 자식은 fork 반환값 0을 받아 `exec`으로 이어지는 전통적인 fork+exec 패턴을 완성한다.

**27에서 이어짐**: 27에서 exec만 있었다. 여기서는 fork를 추가해 "부모가 fork → 자식이 exec → 부모가 wait" 흐름을 구현한다. 이것이 UNIX 셸이 명령을 실행하는 방식의 핵심이다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### fork의 의미

`fork()`는 현재 프로세스를 복제해 새 프로세스(자식)를 만든다. 복제 시점부터 부모와 자식은 독립된 주소 공간을 갖는다. 이후 어느 한 쪽의 메모리 변경이 상대방에 영향을 미치지 않는다.

fork는 두 번 반환된다:
- 부모: 자식의 PID
- 자식: 0

이 차이로 `if (pid == 0)` 분기가 자식 경로를 결정한다.

### interrupt_frame 확장: user_esp / user_ss

fork는 자식이 부모와 같은 user-space EIP/ESP에서 실행을 재개해야 한다. ring 3 → ring 0 전환 시 CPU가 커널 스택에 자동으로 push하는 `user_esp`와 `user_ss`를 `interrupt_frame` 구조체 끝에 추가해 `frame->user_esp`로 접근한다.

### paging_fork_dir(parent_pd_phys)

부모 pd를 복제하되 사용자 공간 물리 프레임은 새로 할당해 내용을 복사(eager copy)한다.

```
1. page_alloc() → child pd
2. page_alloc() → child pt0
3. parent pt0 순회:
   - identity map PTE → 그대로 복사 (커널 공유)
   - 비 identity PTE (사용자 프레임) → page_alloc() + memcpy + 새 PTE 기록
4. child pd[0] = child pt0
5. child pd[1..1023] = parent pd[1..1023] (커널 공유)
```

### process_t.entry 제거

25단계에서 `entry`는 `proc_run_trampoline`이 유저 모드 진입 시 eip를 꺼내기 위해 존재했다. fork가 기본 모델이 되면 이 필드와 트램폴린은 역할이 없어진다.

- `proc_run_trampoline` 제거.
- `process_t.entry` 제거.
- `proc_spawn`은 내부적으로 `fork_eip`/`fork_esp`를 설정하고 `fork_child_trampoline`을 재사용하는 방식으로 단순화한다.

### proc_fork(frame)

```
1. proc_alloc() → 자식 슬롯
2. paging_fork_dir(parent->pd_phys) → 자식 전용 pd
3. child->fork_eip = frame->eip      (부모가 syscall 복귀할 user EIP)
4. child->fork_esp = frame->user_esp (부모의 user ESP)
5. thread_create_with_data(fork_child_trampoline, child)
6. 부모에게 child->pid 반환
```

### fork_child_trampoline

자식 스레드의 커널 쪽 진입점. `enter_user_mode_fork_child(entry, fork_esp)`를 호출한다.

### enter_user_mode_fork_child

`enter_user_mode`와 동일하나 iret 직전에 `xor eax, eax`를 실행해 자식이 user mode에 진입할 때 eax=0(fork 반환값)을 갖도록 한다.

### 실행 흐름

```
kernel_main: proc_wait(init_pid)
  → init: sys_fork() → proc_fork(frame)
    → 자식(pid=1) 생성 → fork_child_trampoline 스케줄링
    → 부모 init: pid=1 반환 → sys_write("init: waiting for child")
    → sys_wait(1, &code)
  → 자식: fork 복귀 eax=0 → sys_exec("hello") → enter_user_mode(hello_entry)
    → hello: "Hello from hello process!" → sys_exit(42)
    → proc_exit(42): pid=1 ZOMBIE
  → proc_wait(1) 반환 → sys_write("init: child done") → sys_exit(0)
  → proc_exit(0): pid=0 ZOMBIE
kernel_main: "processes: init exited code=0"
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
init: waiting for child
Hello from hello process!
process 1 exited: code=42
init: child done
process 0 exited: code=0
processes: init exited code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/interrupts.h` | 수정 | `interrupt_frame`에 `user_esp`, `user_ss` 필드 추가 |
| `boot/gdt.h` | 수정 | `enter_user_mode_fork_child(u32 eip, u32 user_esp)` 선언 추가 |
| `boot/gdt.asm` | 수정 | `enter_user_mode_fork_child` 구현; iret 전 `xor eax, eax` |
| `boot/paging.h` | 수정 | `paging_fork_dir(u32 parent_pd_phys)` 선언 추가 |
| `boot/paging.c` | 수정 | `paging_fork_dir` 구현; 사용자 프레임 eager copy |
| `boot/process.h` | 수정 | `process_t`에서 `entry` 제거, `fork_eip`/`fork_esp` 추가; `interrupts.h` include; `proc_fork` 선언 |
| `boot/process.c` | 수정 | `proc_run_trampoline` 제거; `proc_spawn` 단순화; `fork_child_trampoline`, `proc_fork` 구현 |
| `boot/syscall.h` | 수정 | `SYS_FORK=7` 추가 |
| `boot/syscall.c` | 수정 | `SYS_FORK` 핸들러 추가 |
| `user/init.c` | 수정 | `sys_fork` + `sys_exec("hello")` + `sys_wait` 패턴으로 교체 |
| `Makefile` | 수정 | `process.c` 의존성에 `gdt.h`, `interrupts.h` 추가 |

## 다음 단계 힌트

- `29-vfs`: 파일 디스크립터, 경로 해석, 단순 VFS 계층
