# 23-5 — proc-exec

**목표**: `SYS_EXEC`를 구현해 현재 프로세스의 주소 공간을 새 ELF 이미지로 교체한다.

**23-4에서 이어짐**: 23-4에서 init이 `proc_spawn`으로 자식을 만들고 `proc_wait`로 기다렸다. 여기서는 `proc_exec`을 추가해 프로세스가 자신의 이미지를 다른 ELF로 완전히 교체하는 흐름을 구현한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### exec의 의미

`exec(name)`은 현재 프로세스(PID, 스레드, 프로세스 슬롯)는 그대로 유지하면서 주소 공간의 내용만 새 ELF로 교체한다. 이후 실행은 새 ELF의 진입점에서 시작된다. 호출한 코드로는 절대 돌아오지 않는다.

### paging_free_user_pages(pd_phys)

exec 전에 현재 프로세스의 사용자 공간 프레임을 회수한다.

pt0(PDE[0]의 page_table)을 순회하며, identity map과 다른 물리 주소를 가진 PTE를 찾아 `page_free`한 뒤 identity 값으로 복원한다. 커널 코드/데이터는 identity map 그대로이므로 건드리지 않는다.

```
PTE 물리주소 == i * 0x1000  → identity map (커널), 스킵
PTE 물리주소 != i * 0x1000  → 사용자 프레임, page_free + 복원
```

### proc_exec(name)

```
1. initrd_open(name) → ELF 데이터
2. paging_free_user_pages(p->pd_phys) → 구 사용자 프레임 회수
3. elf_load_process(data, size, p->pd_phys) → 새 ELF를 같은 pd에 적재
4. enter_user_mode(entry, PROC_USTACK_TOP) → 새 진입점으로 점프, 복귀 없음
```

`enter_user_mode`가 iret으로 유저 모드에 진입하므로 syscall 핸들러로는 돌아오지 않는다. exec 실패 시에만 `proc_exec`이 반환되며, `syscall_dispatch`가 `frame->eax = -1`을 설정한다.

### 완료 기준

```
processes: init spawned pid=0
init: before exec
Hello from exec'd hello2!
process 0 exited: code=0
processes: init exited code=0
```

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 (5초 후 종료)
make clean
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/paging.h` | 수정 | `paging_free_user_pages(u32 pd_phys)` 선언 추가 |
| `boot/paging.c` | 수정 | `paging_free_user_pages` 구현; pt0 순회하며 비 identity 프레임 회수 |
| `boot/process.h` | 수정 | `proc_exec(const char *name)` 선언 추가 |
| `boot/process.c` | 수정 | `proc_exec` 구현; `enter_user_mode_fork_child` extern 선언 |
| `boot/syscall.h` | 수정 | `SYS_EXEC=6` 추가 |
| `boot/syscall.c` | 수정 | `SYS_EXEC` 핸들러 추가 |
| `user/init.c` | 수정 | spawn/wait 제거; `sys_exec("hello2")` 호출로 교체 |
| `user/hello2.c` | 신규 | "Hello from exec'd hello2!" 출력 후 code=0으로 exit |
| `Makefile` | 수정 | `hello2` 빌드 타깃; initramfs에 hello2 추가 |

## 다음 단계 힌트

- `23-6-proc-fork`: `SYS_FORK` — 부모 주소 공간을 eager copy해 자식을 생성, fork+exec 패턴 완성
