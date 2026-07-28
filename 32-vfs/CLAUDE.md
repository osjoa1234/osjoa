# 32 — vfs

**목표**: syscall과 initrd 사이에 VFS 레이어를 두고, 프로세스별 파일 디스크립터 테이블을 구현한다.

**31에서 이어짐**: 31에서는 `SYS_OPEN`/`SYS_READ`가 initrd 인덱스를 fd로 그대로 노출했다. 여기서는 `vfs_ops_t` vtable과 마운트 테이블을 두어 백엔드(initrd)를 추상화하고, 프로세스마다 독립된 fd 테이블을 유지한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### VFS 레이어

```c
struct vfs_ops {
    int  (*open)(const char *path);
    u32  (*read)(int bfd, u8 *buf, u32 len);
    void (*close)(int bfd);
};
```

`vfs_open(path)` → 마운트 테이블을 순회하며 path 접두어가 맞는 백엔드를 찾고, 백엔드의 `open`을 호출해 `vfs_file_t *`를 반환한다. `vfs_read`와 `vfs_close`는 `vfs_file_t.ops` vtable을 통해 디스패치된다.

### 마운트

```c
static vfs_ops_t initrd_ops = { initrd_open, initrd_read, initrd_vfs_close };
vfs_mount("/", &initrd_ops);
```

`kernel.c`에서 initrd 초기화 직후 "/" 에 마운트한다. `vfs_open("/hello.txt")` 호출 시 "/" 접두어가 일치하면 `initrd_open("hello.txt")`를 호출한다.

### 프로세스별 fd 테이블

```c
#define PROC_FD_MAX 8U

typedef struct {
    ...
    vfs_file_t *fds[PROC_FD_MAX];
} process_t;
```

`SYS_OPEN`: `vfs_open` → 빈 슬롯에 `vfs_file_t *` 저장 → 슬롯 인덱스(fd) 반환.  
`SYS_READ`: `p->fds[fd]` 조회 → `vfs_read` 호출.  
`SYS_CLOSE`: `vfs_close` → 슬롯을 NULL로 초기화.

`proc_alloc`에서 fd 테이블을 0으로 초기화하고, `proc_exec`와 `proc_exit`에서 열린 fd를 모두 닫는다.

### 인라인 asm clobber

`sys_write` 같이 반환값이 없는 syscall 래퍼는 EAX output constraint를 명시해야 한다. 그렇지 않으면 `-O2` 컴파일러가 EAX가 변하지 않는다고 가정해 연속된 syscall에서 잘못된 EAX 값을 사용한다. `"memory"` clobber는 `sys_read`처럼 커널이 유저 버퍼를 수정하는 경우 필요하다.

```c
static void sys_write(const char *s)
{
    unsigned int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(0U), "b"(s) : "memory");
}
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
init: open /hello.txt
init: read: hello, world!
init: vfs done
process 0 exited: code=0
processes: init exited code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/vfs.h` | 신규 | `vfs_ops_t` vtable, `vfs_file_t`, 마운트 테이블 인터페이스 |
| `boot/vfs.c` | 신규 | `vfs_init`, `vfs_mount`, `vfs_open`, `vfs_read`, `vfs_close` 구현 |
| `boot/process.h` | 수정 | `vfs.h` 포함; `PROC_FD_MAX` 상수; `fds[PROC_FD_MAX]` 필드 추가 |
| `boot/process.c` | 수정 | `proc_alloc` — fd 테이블 초기화; `proc_exec`/`proc_exit` — fd 전체 닫기 |
| `boot/syscall.h` | 수정 | `SYS_CLOSE = 10` 추가 |
| `boot/syscall.c` | 수정 | `initrd.h` 제거; `vfs.h` 추가; `sys_open`/`sys_read` → VFS 경유; `sys_close` 추가 |
| `boot/kernel.c` | 수정 | `vfs.h` 포함; `initrd_ops` 정의; `vfs_init` + `vfs_mount("/", &initrd_ops)` 호출 |
| `Makefile` | 수정 | `VFSOBJ` 추가; 빌드 의존성 갱신 |
| `user/init.c` | 수정 | 스레드 데모 코드 제거; `sys_open`/`sys_read`/`sys_close` 추가; VFS 파일 읽기 데모; 인라인 asm clobber 수정 |
| `user/clone.asm` | 유지 | 링크 유지 (이후 단계에서 재사용) |
| `initrd/hello.txt` | 신규 | VFS로 읽을 텍스트 파일 ("hello, world!\n") |

## 다음 단계 힌트

- `33-user-shell`: 사용자 모드 셸과 기본 명령 실행
