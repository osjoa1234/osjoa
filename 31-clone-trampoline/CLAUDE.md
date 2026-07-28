# 31 — clone-trampoline

**목표**: 유저 공간이 스레드 스택을 직접 소유하고, `clone_trampoline` ASM 심으로 `thread_create(fn, arg)` 라이브러리 함수를 구현한다.

**30에서 이어짐**: 30에서는 커널이 `page_alloc` + `paging_map_user_page`로 클론 스레드 스택을 할당했다. 여기서는 그 역할을 유저 공간으로 옮긴다. 커널은 스택을 건드리지 않고 유저가 넘겨준 `child_stack` 포인터만 받아 그대로 사용한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### 유저 공간 스택 소유

```c
static char stacks[2][STACK_SIZE];
```

유저 이미지의 BSS에 스택 버퍼를 정적으로 선언한다. 커널이 스택 페이지를 할당하고 PD에 매핑하는 과정이 사라지고, 유저가 이미 ELF 로드 시점에 주소 공간 안에 확보한 메모리를 그대로 쓴다.

### clone_trampoline

`clone_trampoline(sp)`는 부모가 스택을 준비한 뒤 호출하는 ASM 함수다.

```
부모 호출 시 스택(sp):
  sp[0] = fn   (워커 함수 포인터)
  sp[1] = arg  (워커 인자)
```

```asm
clone_trampoline:
    mov ebx, [esp+4]   ; ebx = sp (child_stack)
    mov eax, 8         ; SYS_CLONE
    int 0x80           ; 커널 진입
    test eax, eax
    jnz .parent        ; eax != 0 → 부모: 반환
    ; 자식: ESP = sp (커널이 child_stack으로 세팅)
    pop eax            ; eax = fn
    pop ecx            ; ecx = arg
    push ecx
    call eax           ; fn(arg)
    mov eax, 9         ; SYS_THREAD_EXIT
    int 0x80
.parent:
    ret
```

`SYS_CLONE` 진입 시 커널은 `frame->ebx`(=sp)를 `child_stack`으로 받아 `clone_fork_ctx_t.user_esp`에 저장한다. 자식 스레드가 유저 모드로 복귀하면 `enter_user_mode_fork`가 ESP를 이 값으로 세팅하므로, 자식의 스택은 처음부터 `[fn, arg]`를 담은 sp를 가리키게 된다.

### thread_create

```c
static unsigned int thread_create(void (*fn)(unsigned int), unsigned int arg)
{
    unsigned int *sp = (unsigned int *)(stacks[stack_idx++] + STACK_SIZE);
    *(--sp) = arg;
    *(--sp) = (unsigned int)fn;
    return clone_trampoline(sp);
}
```

스택 top에서 내려가며 `fn`, `arg`를 push한 뒤 `clone_trampoline`을 호출한다. 부모에게는 `t->id`, 자식에게는 0을 반환한다.

### 커널 측 변경

`proc_clone(user_eip, child_stack)` — 스택 할당 코드 전체 제거.

`process_t`에서 `next_ustack` 필드와 `PROC_USTACK_SIZE` 상수도 제거한다. 커널이 유저 스택 위치를 추적할 이유가 없어졌기 때문이다.

### SYS_THREAD_EXIT

30에서는 `worker`가 명시적으로 `sys_thread_exit()`를 호출했다. 31에서는 `worker`가 단순히 `return`하면 `clone_trampoline`이 `SYS_THREAD_EXIT`를 대신 발행한다. 유저 코드에서 스레드 종료를 의식할 필요가 없어진다.

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
| `boot/process.h` | 수정 | `PROC_USTACK_SIZE` 상수 제거; `next_ustack` 필드 제거; `proc_clone` 시그니처에 `child_stack` 파라미터 추가 |
| `boot/process.c` | 수정 | `proc_alloc` — `next_ustack` 초기화 제거; `proc_exec` — `next_ustack` 리셋 제거; `proc_clone` — 스택 할당(`page_alloc`/`paging_map_user_page`) 전체 제거, `child_stack` 파라미터로 `ctx->user_esp` 설정 |
| `boot/syscall.c` | 수정 | `SYS_CLONE` 핸들러에서 `frame->ebx`를 `child_stack`으로 전달 |
| `user/init.c` | 수정 | `stacks[2][STACK_SIZE]` 정적 버퍼 추가; `thread_create(fn, arg)` 함수 추가; `clone_trampoline` extern 선언; `worker`에서 `sys_thread_exit()` 호출 제거; `_start`에서 raw syscall 대신 `thread_create` 사용 |
| `user/clone.asm` | 신규 | `clone_trampoline` 구현 — `ebx`에 child_stack 세팅 후 SYS_CLONE, 자식 쪽 fn/arg 디스패치 및 SYS_THREAD_EXIT |
| `Makefile` | 수정 | `CLONEOBJ` 추가; `USERINIT` 링크에 `CLONEOBJ` 포함 |

## 다음 단계 힌트

- `32-vfs`: 파일 디스크립터, 경로 해석, 단순 VFS 계층
