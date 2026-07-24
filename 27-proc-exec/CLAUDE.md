# 27 — proc-exec

**목표**: `SYS_EXEC`를 구현해 현재 프로세스의 주소 공간을 새 ELF 이미지로 교체한다.

**26에서 이어짐**: 26에서 init이 `proc_spawn`으로 자식을 만들고 `proc_wait`으로 기다렸다. 여기서는 `proc_exec`을 추가해 프로세스가 자신의 이미지를 다른 ELF로 완전히 교체하는 흐름을 보인다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### exec의 의미

`exec(name)`은 현재 프로세스(PID, 커널 스레드, 프로세스 슬롯)는 그대로 두고 사용자 공간의 내용만 새 ELF로 교체한다. 호출한 코드로는 절대 돌아오지 않는다.

### paging_free_user_pages(pd_phys)

exec 전에 현재 프로세스의 사용자 공간 프레임을 회수한다.

`paging_clone_dir`은 PDE[0]을 위해 새 `pt0`을 할당하고 커널 identity map을 복사한다. `elf_load_process`는 이 `pt0`에 새 프레임을 할당해 매핑한다. `paging_free_user_pages`는 `pt0`을 순회하면서:

```
PTE[i] 물리주소 == i * 0x1000  → identity map (커널), 스킵
PTE[i] 물리주소 != i * 0x1000  → 사용자 프레임, page_free 후 identity 값으로 복원
```

커널 코드/데이터는 PDE[1..] 구간이므로 건드리지 않는다.

### proc_exec(name)

```
1. initrd_open(name) → ELF 데이터
2. paging_free_user_pages(p->pd_phys) → 구 사용자 프레임 회수
3. elf_load_process(data, size, p->pd_phys) → 새 ELF를 같은 pd에 적재
4. p->entry 갱신
5. enter_user_mode(entry, PROC_USTACK_TOP) → 새 진입점으로 점프, 복귀 없음
```

exec가 성공하면 `enter_user_mode`의 `iret`이 유저 모드로 전환되어 syscall 핸들러로 돌아오지 않는다. 실패 시에만 `proc_exec`이 반환되며 `syscall_dispatch`가 `frame->eax = -1`을 설정한다.

### 실행 흐름

```
kernel_main: proc_spawn("init") → pid=0
  init: sys_write("init: before exec")
  init: sys_exec("hello2") → proc_exec("hello2")
    paging_free_user_pages → init 사용자 프레임 해제
    elf_load_process → hello2 ELF 적재
    enter_user_mode(hello2_entry) → ring3 점프
  hello2: sys_write("Hello from exec'd hello2!")
  hello2: sys_exit(0) → proc_exit(0)
    pid=0 ZOMBIE, exit_code=0
    thread_unpark(waiter=kernel_main)
kernel_main proc_wait: exit_code=0 → "processes: init exited code=0"
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
init: before exec
Hello from exec'd hello2!
process 0 exited: code=0
processes: init exited code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/paging.h` | 수정 | `paging_free_user_pages(u32 pd_phys)` 선언 추가 |
| `boot/paging.c` | 수정 | `paging_free_user_pages` 구현: pt0 순회하며 비 identity 프레임 회수 |
| `boot/process.h` | 수정 | `proc_exec(const char *name)` 선언 추가 |
| `boot/process.c` | 수정 | `proc_exec` 구현: free → load → enter_user_mode |
| `boot/syscall.h` | 수정 | `SYS_EXEC=6` 추가 |
| `boot/syscall.c` | 수정 | `SYS_EXEC` 핸들러 추가 |
| `user/init.c` | 수정 | spawn/wait 제거; `sys_exec("hello2")` 호출로 교체 |
| `user/hello2.c` | 신규 | "Hello from exec'd hello2!" 출력 후 code=0으로 exit |
| `Makefile` | 수정 | `hello2` 빌드 타깃; initramfs에 hello2 추가; clean에 hello2 포함 |

## 다음 단계 힌트

- `28-proc-fork`: `SYS_FORK` — 부모 주소 공간을 eager copy로 자식 생성, fork+exec 패턴 완성
