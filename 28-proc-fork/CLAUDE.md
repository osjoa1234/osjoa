# 28 — proc-fork

**목표**: `SYS_FORK`를 구현해 부모 프로세스의 주소 공간을 eager copy로 자식에게 복제하고, fork+exec 패턴을 완성한다.

**27에서 이어짐**: 27에서 `SYS_EXEC`로 현재 프로세스 이미지를 교체하는 흐름을 완성했다. 여기서는 `SYS_FORK`를 추가해 자식 프로세스를 새로 만들고, 자식이 exec로 새 이미지를 적재하는 fork+exec 패턴을 구현한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### fork의 의미

`fork()`는 호출 프로세스(부모)의 사용자 주소 공간을 통째로 복제해 새 프로세스(자식)를 만든다. 부모와 자식은 각자 독립된 페이지 디렉토리와 프레임을 가진다(eager copy). 이후 두 프로세스는 독립적으로 실행된다.

- 부모: `fork()` 반환값 = 자식 PID (양수)
- 자식: `fork()` 반환값 = 0

### interrupt_frame에 user_esp / user_ss 추가

`int 0x80`이 ring 3 → ring 0 전환을 일으킬 때 CPU는 스택에 `SS:ESP`(유저 스택)를 추가로 저장한다. `struct interrupt_frame`에 `user_esp`와 `user_ss` 필드를 추가해 이 값을 `SYS_FORK` 핸들러에서 읽을 수 있게 했다.

### paging_copy_user_pages(src_pd_phys, dst_pd_phys)

`paging_clone_dir`이 빈 pt0을 가진 자식 pd를 만든 뒤, 이 함수가 부모 pt0의 모든 present PTE를 순회하며:

1. 새 물리 프레임을 할당
2. 부모 프레임 내용을 바이트 단위로 복사 (KERNEL_OFFSET을 통해 직접 접근)
3. 자식 pt0에 같은 가상 주소로 새 프레임을 매핑

커널 영역(PDE[1..])은 `paging_clone_dir`이 이미 공유 매핑으로 복사하므로 건드리지 않는다.

### enter_user_mode_fork(eip, esp)

`iret` 직전에 `xor eax, eax`를 실행해 자식이 유저 모드로 복귀할 때 `eax = 0`이 되도록 한다. 부모의 `iret`은 인터럽트 핸들러의 `popad`를 거쳐 `frame->eax = child_pid`로 복귀한다.

### proc_fork(user_eip, user_esp)

```
1. proc_alloc() → 자식 process_t 슬롯 확보
2. paging_clone_dir() → 빈 pt0을 가진 자식 pd 생성
3. paging_copy_user_pages(parent->pd_phys, child_pd) → 사용자 프레임 eager copy
4. child->entry = user_eip, child->user_esp = user_esp 저장
5. thread_create_with_data(fork_child_trampoline, child) → 자식 커널 스레드 생성
6. 부모에게 child->pid 반환
```

자식 커널 스레드는 `fork_child_trampoline`에서 시작해 `enter_user_mode_fork(p->entry, p->user_esp)`를 호출한다. 자식은 부모가 `int $0x80` 다음에 복귀할 EIP와 동일한 지점에서 `eax = 0`으로 유저 모드를 시작한다.

### 실행 흐름

```
kernel_main: proc_spawn("init") → pid=0
  init (pid=0): sys_write("init: before fork")
  init (pid=0): sys_fork()
    proc_fork → child pid=1 생성, 자식 스레드 스케줄 대기
    부모: frame->eax = 1, iret → 유저 모드 복귀, pid=1 받음
  init (pid=0): sys_wait(1) → proc_wait(1) → thread_park()

  child (pid=1): fork_child_trampoline → enter_user_mode_fork(eip, esp), eax=0
  child (pid=1): sys_fork() returns 0 → if (pid == 0) 분기
  child (pid=1): sys_write("child: exec hello2")
  child (pid=1): sys_exec("hello2") → proc_exec("hello2")
    paging_free_user_pages → child의 init 프레임 해제
    elf_load_process → hello2 ELF 적재
    enter_user_mode(hello2_entry) → ring3 점프
  child (pid=1): sys_write("Hello from fork+exec'd hello2!")
  child (pid=1): sys_exit(0) → proc_exit(0)
    pid=1 ZOMBIE, thread_unpark(waiter=init's thread)

  init (pid=0): proc_wait 복귀
  init (pid=0): sys_write("init: child done")
  init (pid=0): sys_exit(0) → proc_exit(0)
    pid=0 ZOMBIE, thread_unpark(waiter=kernel_main)

kernel_main: proc_wait 복귀 → "processes: init exited code=0"
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
child: exec hello2
Hello from fork+exec'd hello2!
process 1 exited: code=0
init: child done
process 0 exited: code=0
processes: init exited code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/interrupts.h` | 수정 | `interrupt_frame`에 `user_esp`, `user_ss` 필드 추가 |
| `boot/gdt.asm` | 수정 | `enter_user_mode_fork` 추가: xor eax,eax 후 iret |
| `boot/paging.h` | 수정 | `paging_copy_user_pages(u32 src, u32 dst)` 선언 추가 |
| `boot/paging.c` | 수정 | `paging_copy_user_pages` 구현: pt0 순회하며 프레임 복제 |
| `boot/process.h` | 수정 | `process_t`에 `user_esp` 필드 추가; `proc_fork` 선언 추가 |
| `boot/process.c` | 수정 | `fork_child_trampoline`, `proc_fork` 구현 추가 |
| `boot/syscall.h` | 수정 | `SYS_FORK=7` 추가 |
| `boot/syscall.c` | 수정 | `SYS_FORK` 핸들러: `proc_fork(frame->eip, frame->user_esp)` |
| `user/init.c` | 수정 | fork → 자식 exec, 부모 wait 패턴으로 교체 |
| `user/hello2.c` | 수정 | 메시지를 "Hello from fork+exec'd hello2!"로 변경 |

## 다음 단계 힌트

- `29-vfs`: 파일 디스크립터, 경로 해석, 단순 VFS 계층
