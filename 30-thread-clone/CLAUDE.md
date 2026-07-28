# 30 — thread-clone

**목표**: `process_t`를 1:N 스레드 구조로 확장하고, `SYS_CLONE`으로 현재 프로세스 내에 새 유저 스레드를 생성한다. 스레드마다 독립된 유저스택을 할당한다.

**29에서 이어짐**: 29에서 wait queue와 waitpid(-1)을 완성했다. 여기서는 `process_t`가 단일 `thread_t *thread`만 가지던 구조를 `thread_t *threads` 연결 리스트로 확장하고, `SYS_CLONE`으로 같은 주소 공간을 공유하는 스레드를 추가로 생성할 수 있게 한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### process_t 1:N 스레드 구조

`thread_t`에 `proc_next` 포인터를 추가해 같은 프로세스 내 스레드들을 연결 리스트로 관리한다.

```
process_t
  threads → thread_t { proc_next → thread_t { proc_next → NULL } }
```

`thread_t->next`는 스케줄러의 전역 실행 큐용(변경 없음), `proc_next`는 프로세스 내부 목록 전용이다.

`process_t`에 `next_ustack` 필드를 추가해 다음 클론 스레드에 할당할 유저스택 top 주소를 추적한다. 초기값은 `PROC_USTACK_TOP - PROC_USTACK_SIZE`이다.

### 유저스택 레이아웃

`elf_load_process`는 메인 스레드 스택 페이지를 `PROC_USTACK_TOP - 0x1000`에 할당한다. 클론 스레드들은 그 아래부터 각자의 4KB 스택 페이지를 가진다.

```
0x00400000  ← PROC_USTACK_TOP, 메인 스레드 ESP
  [0x3FF000–0x400000]  메인 스레드 스택 페이지 (elf_load_process가 할당)
  [0x3FE000–0x3FF000]  clone thread 1 스택 페이지
  [0x3FD000–0x3FE000]  clone thread 2 스택 페이지
  ...
```

`proc_clone`은 `page_alloc` → `paging_map_user_page`로 새 스택 페이지를 프로세스 PD에 직접 매핑한다.

### proc_clone 흐름

SYS_CLONE은 SYS_FORK와 같은 fork-style로 동작한다. `proc_clone(user_eip)`는 syscall 진입 시점의 EIP(= `int $0x80` 다음 명령)를 받아 자식 스레드가 같은 지점에서 실행을 재개하게 한다. 부모에게는 `t->id`를, 자식 스레드에게는 `enter_user_mode_fork`의 `xor eax, eax`로 0을 반환한다.

```
1. p->next_ustack에서 스택 top(utop) 읽기
2. page_alloc() → 페이지 0으로 초기화 → paging_map_user_page(pd, utop-0x1000, frame)
3. p->next_ustack -= 0x1000
4. clone_fork_ctx_t { proc, user_eip, user_esp=utop } 할당  ← fork와 동일한 구조체 재사용
5. thread_create_with_data(clone_fork_trampoline, ctx)      ← fork와 동일한 trampoline 재사용
6. t->pd = p->pd_phys  (같은 PD 공유)
7. p->threads 연결 리스트 끝에 t 연결
8. 반환: t->id (부모) / 0 (자식, eax를 xor로 클리어)
```

`clone_fork_trampoline`에서는 ctx를 꺼낸 뒤 `thread_current()->user_data = proc`으로 교체하고, ctx를 kfree한 다음 `enter_user_mode_fork(eip, esp)`를 호출한다. 자식 스레드는 SYS_CLONE `int $0x80` 직후 주소로 복귀하며, eax=0이므로 `if (tid == 0)` 분기를 탄다. 새 gdt.asm 함수는 추가하지 않고 기존 `enter_user_mode_fork`를 그대로 재사용한다.

### SYS_THREAD_EXIT (9)

클론 스레드가 작업을 마치고 호출한다. 프로세스의 다른 살아있는 스레드가 있으면 `thread_exit()`로 이 스레드만 종료하고, 마지막 스레드이면 `proc_exit(0)`으로 프로세스 전체를 종료한다.

### proc_exit 변경

`proc_exit`는 이제 프로세스의 모든 스레드(`p->threads` 리스트)를 순회해 현재 스레드 외 나머지를 `THREAD_DEAD`로 강제 종료한 뒤 자신은 `thread_exit()`로 빠져나간다.

### 실행 흐름

```
processes: init spawned pid=0
init: before clone
init: clones spawned
thread 1: hello from clone        (스케줄링 순서에 따라 변동 가능)
thread 2: hello from clone
init: both clones done
process 0 exited: code=0
processes: init exited code=0
```

init의 메인 스레드와 두 클론 스레드는 같은 주소 공간에서 `done_count` 공유 변수를 통해 동기화한다. 두 클론이 완료되면 메인이 `sys_exit(0)`으로 프로세스를 종료한다.

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
init: before clone
init: clones spawned
thread 1: hello from clone
thread 2: hello from clone
init: both clones done
process 0 exited: code=0
processes: init exited code=0
```

(thread 1/2 출력 순서는 스케줄링에 따라 달라질 수 있다.)

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/thread.h` | 수정 | `proc_next` 필드 추가 (프로세스 내부 스레드 연결 리스트) |
| `boot/thread.c` | 수정 | `thread_create_with_data`: `proc_next = 0` 초기화 |
| `boot/process.h` | 수정 | `thread_t *thread` → `thread_t *threads`; `next_ustack` 추가; `PROC_USTACK_SIZE` 상수 추가; `proc_clone`/`proc_thread_exit` 선언 |
| `boot/process.c` | 수정 | `proc_alloc` — next_ustack 초기화; `proc_spawn`/`proc_fork` — threads/proc_next 설정; `proc_exec` — next_ustack 리셋; `proc_exit` — threads 리스트 순회해 나머지 스레드 THREAD_DEAD 처리; `proc_clone`/`proc_thread_exit` 추가; `clone_fork_ctx_t`·`clone_fork_trampoline` 재사용(fork-style clone); phys_mem.h·kheap.h include 추가 |
| `boot/syscall.h` | 수정 | `SYS_CLONE=8`, `SYS_THREAD_EXIT=9` 추가 |
| `boot/syscall.c` | 수정 | SYS_CLONE → `proc_clone(frame->eip)`, SYS_THREAD_EXIT → `proc_thread_exit` 디스패치 |
| `user/init.c` | 수정 | SYS_CLONE을 fork-style로 호출(eax=8); `tid==0`이면 clone 자식으로 판별해 `worker(n)` 진입; `sys_thread_exit`(eax=9); `done_count` 공유 변수로 동기화 |
| `Makefile` | 수정 | hello/hello2 빌드 제거; process.c 의존성에 phys_mem.h·kheap.h 추가 |

## 다음 단계 힌트

- `31-clone-trampoline`: 유저 공간이 스레드 스택을 소유, `clone_trampoline` ASM 심으로 `thread_create(fn, arg)` 라이브러리 구현
