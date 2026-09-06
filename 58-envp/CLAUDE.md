# 58 — envp

**목표**: 커널이 `execve` 시 유저 스택에 진짜 `envp`를 실어주고, 유저 공간이 `environ`/`getenv`로 그걸 읽게 한다. `PATH` 등 환경변수를 실제로 "활용"하는 건(예: 이름만으로 실행 파일을 찾을 때 `PATH`를 뒤지는 것) 59로 미루고, 이번 단계는 배관 자체 — 스택 레이아웃, `SYS_EXECVE`의 세 번째 인자, `getenv` — 만 검증한다.

**57에서 이어짐**: 45(`argv-auxv`)가 `elf_setup_stack`에 `argc`/`argv[]`를 실어 보내는 길을 텄을 때, `envp` 자리는 이미 스택 레이아웃에 있었지만 — `frame_words`에 `argv` NULL 다음 오는 두 번째 `0ULL`이 사실 "빈 envp의 NULL 종단자"였다 — 그 자리를 실제로 채우는 코드가 없어서 모든 프로세스가 항상 `envp = {NULL}`(빈 환경)으로 시작했다. 이번 단계가 그 자리를 처음 채운다.

## `elf_setup_stack` — `argv[]` 확장을 그대로 `envp[]`에도 반복

45가 `argv0` 문자열 하나만 받던 함수를 `argv[]` + `argc`로 일반화했을 때 쓴 패턴(문자열들을 스택 맨 위 페이지에 거꾸로 채워 넣고, 오프셋 배열에 기록한 뒤, 포인터 테이블을 씀)을 그대로 `envp[]` + `envc`에 반복했다:

```c
u64 elf_setup_stack(u32 pml4_phys, char *const argv[], u32 argc,
                     char *const envp[], u32 envc, u64 entry,
                     u64 phdr, u32 phnum, u32 phentsize)
```

`argv_off[USTACK_ARGV_MAX]` 옆에 `envp_off[USTACK_ENVP_MAX]`(둘 다 8개 상한)를 두고, 문자열을 쓰는 루프와 포인터 테이블을 쓰는 루프 모두 `argv` 다음에 `envp`용을 하나씩 더 돌린다. 최종 레이아웃은 리눅스 표준 그대로:

```
argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL, auxv[0].type, auxv[0].val, ..., AT_NULL, AT_NULL
```

`envc = 0`일 때 예전과 완전히 동일한 워드 수·내용이 나온다(빈 `envp` 루프가 아무것도 안 쓰고 NULL 종단자 한 워드만 남으므로) — 회귀가 없다는 걸 코드 형태로 보장한다.

## `proc_exec`/`proc_spawn` — `argv`와 완전히 대칭인 `envp` 처리

45가 `proc_exec`에서 짚은 use-after-free 패턴(`exec`하는 프로세스 자신의 유저 메모리를 가리키는 `argv[]` 문자열들을, `paging_free_user_pages`로 옛 주소공간을 반납하기 *전에* 커널 지역 버퍼로 복사해둬야 함)이 `envp[]`에도 똑같이 적용된다 — `envp[]`도 호출자의 유저 메모리를 가리키는 포인터 배열이기 때문이다. `PROC_EXEC_ENVMAX`/`PROC_EXEC_ENVLEN`(각각 `PROC_EXEC_ARGMAX`/`PROC_EXEC_ARGLEN`과 같은 8/64)을 추가하고, `argv_buf`/`argv_copy`와 나란히 `envp_buf`/`envp_copy`를 만들어 같은 순서(카운트 → 로컬 버퍼로 복사 → NULL 종단)로 채운 뒤 `paging_free_user_pages` 이전에 끝낸다.

`proc_spawn`(커널이 `"init"`을 직접 호출하는 경로, `argv={name,0}`을 그 자리에서 만드는 것과 같은 스코프)은 `envp={"PATH=/", 0}`를 그 자리에서 만들어 넘긴다 — 이게 이 커널이 부팅하는 최초 프로세스(`init`, 즉 이 셸)의 환경변수 시드값이다. `PATH=/`를 고른 이유: 지금은 `proc_exec`가 여전히 `initrd_open(name)`으로 이름만 보고 찾으므로(59에서 VFS 경유로 바뀔 예정) `PATH`를 실제로 뒤지는 코드가 없다 — 값 자체보다 "다음 단계(59)가 그대로 집어 쓸 수 있는 `PATH` 변수가 이미 존재한다"는 배관의 존재를 보이는 게 이번 단계의 목적이다.

## `SYS_EXECVE`가 세 번째 레지스터(`rdx`)로 `envp`를 받는다

리눅스 `execve(2)`의 관례(`rdi`=path, `rsi`=argv, `rdx`=envp)를 그대로 따라 `syscall.c`의 디스패치가 `frame->rdx`를 `envp`로 읽어 `proc_exec`에 전달한다. `boot/process.h`의 `proc_exec` 선언에 `envp[]` 인자가 추가된 것 말고는 호출 규약이 44(`linux-abi`)가 이미 세워둔 x86_64 syscall ABI 그대로다.

## `user/init.c`의 `_start` — 45의 `syscall64.c` 패턴을 셸 자신에 적용

지금까지 셸(`init.c`)의 `_start`는 `void _start(void)`로, 커널이 채워준 초기 스택(`argc`/`argv`/`envp`/`auxv`)을 완전히 무시했다 — 셸이 실행할 명령은 키보드 입력으로 받지 커널이 주는 `argv`로 받는 게 아니었으니 지금까지는 문제가 없었다. 하지만 이번 단계에서 셸 자신이 `environ`/`getenv`를 가지려면 커널이 `proc_spawn`으로 채워준 `envp`를 읽어야 한다.

45의 `syscall64.c`가 이미 이 문제를 풀어둔 정확히 같은 패턴을 그대로 가져다 썼다 — 일반 C 함수의 프롤로그(`push rbp` 등)가 먼저 실행되면 커널이 넘겨준 원본 `%rsp`가 어디였는지 알 수 없으므로, 파일 스코프 `__asm__` 블록으로 `_start`를 손수 짜서 `%rsp`를 그대로 `%rdi`에 담아 `init_main(unsigned long *stack)`으로 넘긴다:

```c
__asm__(
    ".global _start\n"
    "_start:\n"
    "    mov %rsp, %rdi\n"
    "    and $-16, %rsp\n"
    "    call init_main\n"
);
```

`init_main`은 예전 `_start` 본문을 그대로 이어받되, 맨 앞에서 `stack[0]`을 `argc`로, `stack+1`을 `argv`로 읽어 `environ = argv + argc + 1`(NULL 종단자 다음)을 계산해둔다. 셸 로직 자체(ext2/symlink 검증, 파이프라인, 프롬프트 루프)는 한 글자도 안 바뀌었다 — `_start`라는 진입점 이름과 시그니처만 `syscall64.c`와 같은 모양으로 바뀌었을 뿐이다.

## `environ`/`getenv` — 셸이 직접 구현하는 최소 libc 조각

이 프로젝트에는 아직 실제 libc가 없으므로(46/50에서 붙인 musl 정적 바이너리들은 각자 자기 자신의 musl libc를 정적으로 담고 있을 뿐, 이 커널이 제공하는 게 아니다), `environ`/`getenv`는 셸이 스스로 구현해야 한다:

```c
static char **environ;

static char *getenv(const char *name)
{
    for (i = 0; environ[i]; i++) {
        /* environ[i]가 "NAME=value" 형태면 '=' 앞부분이 name과 일치하는지 비교 */
    }
    return 0;
}
```

`check_getenv("PATH", "/")`가 55~57이 써온 `check_*` 진단 헬퍼와 같은 모양(`OK`/`FAIL` 한 줄)으로 부팅 시퀀스 맨 앞, `"shell: linux-abi ready"` 직후에 실행된다 — 커널이 `proc_spawn`으로 심어준 `PATH=/`가 셸 자신의 `environ`에 정확히 도달했는지 확인하는 첫 번째 홉이다.

## 두 번째 홉 — `execve`를 거쳐 자식 프로세스까지 `envp`가 살아서 도착하는지

셸이 `busybox`를 `sys_exec(argv[0], argv)`로 실행하던 모든 자리(`run_argv`, 파이프라인 실행 루프)에 `environ`을 세 번째 인자로 실어 보내도록 고쳤다 — 실제 셸의 `execve(path, argv, environ)` 관례 그대로, 부모의 환경을 자식에게 그대로 물려준다. `sys_exec`도 `rdx`로 `envp`를 실어 보내도록 확장됐다.

이 경로가 실제로 맞는지 눈으로 확인하려면 `envp`를 읽어서 보여주는 자식이 필요하다 — 45가 `argv`/`auxv` 검증용으로 만든 `syscall64` 프로그램이 정확히 그 역할이었으므로 그대로 확장했다. `start_main`이 `envp[i]`를 세던 루프(`while (envp[i]) i++;`, 지금까지 `envp`가 항상 비어 있어서 카운트가 늘 0이었다)를 카운트하면서 각 항목을 출력하는 루프로 바꿨다. 부팅 시퀀스에 `syscall64 envtest`를 자동 실행하는 블록을 추가해(59부터 있던 `busybox touch/mkdir/rm/ls` 자동 실행과 같은 스타일) `syscall64: envp[0000000000000000] = PATH=/`가 찍히는지 확인한다 — 이게 "커널이 심은 값 → 셸의 `environ` → `execve`로 자식에게 전달 → 자식의 초기 스택에서 읽음"이 전부 연결됐다는 증거다.

## 검증

1. **셸 자신의 `environ`(1홉)**: `check_getenv("PATH", "/")` — `proc_spawn`이 심은 `envp`가 셸의 `init_main`이 계산한 `environ`에 정확히 도달했는지.
2. **`execve`를 통한 전달(2홉)**: `syscall64 envtest` 자동 실행 — 셸이 자신의 `environ`을 `sys_exec`의 세 번째 인자로 넘기고, 커널의 `proc_exec`가 그걸 복사해 새 스택에 쓰고, `syscall64`의 `init_main`(을 그대로 재사용하는 `start_main`)이 그 스택에서 `envp[0] = "PATH=/"`를 읽어 출력하는지.
3. **회귀**: 57까지의 ext2/symlink 검증, `busybox touch/mkdir/rm/ls` 전부 이전과 동일하게 통과해야 한다 — `elf_setup_stack`의 `envc=0` 경로(다른 `run_argv` 호출들도 여전히 `environ`을 넘기지만, `proc_spawn`이 심은 `PATH=/` 하나뿐이라 사실상 `envc=1`로 통과한다)와 `envc>0` 경로 둘 다 이번 검증에서 실제로 실행된다.
4. **`make run-nogui` 뒤 `e2fsck -f -n build/disk.img`**: 에러 없이 통과 — 이번 단계는 ext2를 건드리지 않았으므로 57과 동일하게 나와야 한다.

## 완료 기준

`make run-nogui`에서 `shell: linux-abi ready` 직후, 57의 ext2 열기 검증(`shell: ext2 /disk/hello.txt: ...`) 이전에 다음이 보이면 성공이다:

```
shell: linux-abi ready
shell: getenv PATH: OK
shell: syscall64 envtest:
syscall64: argc = 0x0000000000000002
syscall64: argv[0000000000000000] = syscall64
syscall64: argv[0000000000000001] = envtest
syscall64: envp[0000000000000000] = PATH=/
syscall64: auxv AT_PAGESZ = 0x0000000000001000
syscall64: arch_prctl(SET_FS) rc = 0x0000000000000000
syscall64: fs:0 = 0x1234567890ABCDEF
syscall64: arch_prctl(GET_FS) rc = 0x0000000000000000
syscall64: fs_base readback = 0x0000000000300860
syscall64: getpid = 0x0000000000000001
syscall64: getuid = 0x0000000000000000
syscall64: uname rc = 0x0000000000000000
syscall64: uname sysname = custom-os
syscall64: uname release = 0.43.0
process 1 exited: code=0
```

그 뒤로는 57과 완전히 동일한 ext2/symlink 검증과 `busybox ls /disk:` 출력이 에러 없이 이어지고 `process 1 exited: code=0`로 끝나야 한다. `build/disk.img`에 대해 `e2fsck -f -n`이 에러 없이 통과해야 한다.

## 이전 단계(57) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/elf.h` | 수정 | `elf_setup_stack` 시그니처에 `envp[]`/`envc` 추가 |
| `boot/elf.c` | 수정 | `USTACK_ENVP_MAX`(8) 추가, `envp_off[]` 배열과 `envp` 문자열 쓰기 루프·포인터 테이블 루프를 `argv`와 대칭으로 추가, `nwords` 계산에 `envc`+NULL 종단자 반영 |
| `boot/process.h` | 수정 | `PROC_EXEC_ENVMAX`(8)/`PROC_EXEC_ENVLEN`(64) 추가, `proc_exec` 시그니처에 `envp[]` 추가 |
| `boot/process.c` | 수정 | `proc_spawn`이 `envp={"PATH=/", 0}`를 만들어 `elf_setup_stack`에 전달; `proc_exec`가 `argv`와 같은 use-after-free 방지 패턴(`paging_free_user_pages` 전에 커널 지역 버퍼로 복사)을 `envp`에도 적용 |
| `boot/syscall.c` | 수정 | `SYS_EXECVE` 디스패치가 `frame->rdx`를 `envp`로 읽어 `proc_exec`에 전달 |
| `user/init.c` | 수정 | `_start`를 `syscall64.c`와 같은 파일 스코프 `__asm__` 진입점으로 바꾸고 본문을 `init_main(unsigned long *stack)`으로 이동, 맨 앞에서 `environ` 계산; `getenv`/`check_getenv` 신규; `sys_exec`가 `envp` 세 번째 인자(`rdx`)를 받도록 확장, `run_argv`와 파이프라인 실행 루프가 `environ`을 전달; 부팅 시퀀스 맨 앞에 `check_getenv("PATH", "/")`와 `syscall64 envtest` 자동 실행 블록 추가 |
| `user/syscall64.c` | 수정 | `start_main`의 `envp` 카운트 전용 루프(`while (envp[i]) i++;`)를 `argv[i]`처럼 각 항목을 출력하는 루프로 확장 |
| 나머지 전부 | 변경 없음 | 57의 파일 그대로 |

## 다음 단계 힌트

- **`proc_exec`는 여전히 initrd 직접 조회**: 59(`59-exec-vfs-symlink`)가 `proc_exec`를 VFS 경유로 리팩터링하면서, 이번 단계가 배관만 놓은 `PATH=/`를 실제로 뒤지는 이름 탐색을 붙인다 — `busybox`를 `cat`/`ls`/`sh` 등으로 심링크한 멀티콜 바이너리를 짧은 이름으로 실행하는 것도 이 시점에 함께 검증된다.
- **셸에 `export`/환경변수 수정 기능 없음**: `environ`은 `proc_spawn`이 심은 값을 그대로 들고 있을 뿐, 셸 안에서 새 변수를 추가하거나 값을 바꿀 방법이 없다 — 지금까지 그런 요구가 로드맵에 없었으므로 자연스러운 스코프 결정이다. 필요해지면 `environ`을 가리키는 배열을 셸의 힙/정적 버퍼로 옮기고 `setenv`류를 추가해야 한다.
- **`envp`/`argv` 상한(8개)과 스택 페이지 한 장짜리 문자열 영역**: 45 때 정해진 `USTACK_ARGV_MAX=8`과 같은 상한을 `USTACK_ENVP_MAX=8`에도 그대로 적용했다 — 지금 값들(`PATH=/` 하나)로는 전혀 문제가 안 되지만, 나중에 환경변수가 여러 개 필요해지면(예: `HOME`, `TERM`) 상한과 스택 맨 위 페이지 한 장(4KB)에 문자열이 다 들어가는지 함께 재검토해야 한다.
