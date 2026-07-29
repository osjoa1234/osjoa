# 33 — fd-stdio

**목표**: `vfs_ops_t`에 `write`를 추가하고, console을 VFS 백엔드로 연결하여 fd 1로 쓰는 경로를 완성한다. 프로세스 생성 시 fd 0/1/2를 console로 미리 열어두고, fork 시 자식에게 상속한다.

**32에서 이어짐**: 32에서는 read 방향만 VFS를 통했고, `SYS_WRITE`는 커널 콘솔을 직접 호출했다. 여기서는 `SYS_WRITE`도 fd 경유로 바꿔서 `write(1, buf, n)`이 fd 테이블 → vfs_ops_t.write → console_dev 경로를 타도록 한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### write 경로

```
write(1, buf, n)          ← 유저 syscall
  → SYS_WRITE             ← 커널 syscall_dispatch
  → p->fds[1]             ← 프로세스 fd 테이블 조회
  → vfs_write(f, buf, n)  ← VFS 레이어
  → f->ops->write(...)    ← vtable 디스패치
  → con_write(...)        ← console_dev 백엔드
  → console_putchar(c)    ← VGA 출력
```

### vfs_ops_t.write

```c
struct vfs_ops {
    int  (*open)(const char *path);
    u32  (*read)(int bfd, u8 *buf, u32 len);
    u32  (*write)(int bfd, const u8 *buf, u32 len);  /* 신규 */
    void (*close)(int bfd);
};
```

write가 없는 백엔드(initrd 등)는 NULL로 초기화한다. `vfs_write`에서 NULL 여부를 확인하고 건너뛴다.

### fd 0/1/2 초기화

`proc_spawn`에서 ELF 적재 후 `console_dev_open()`을 세 번 호출해 fd 0/1/2를 미리 채운다. 번호 자체에는 아무 의미가 없고, "0/1/2번 슬롯에 console이 연결되어 있다"는 약속이 stdout/stderr 규칙의 전부다.

### fork fd 상속

`proc_fork`에서 `proc_alloc()` 직후 부모 fd 테이블을 순회하며 각 슬롯을 `vfs_dup`으로 복사한다. `vfs_dup`은 동일한 ops와 backend_fd를 가진 새 `vfs_file_t`를 할당한다. 자식이 종료할 때 `proc_exit`에서 독립적으로 닫힌다.

### proc_exec fd 처리

exec는 현재 프로세스 이미지를 교체하지만 fd 0/1/2는 유지한다(POSIX 기본값). fd 3 이상만 닫는다.

### SYS_WRITE 시그니처 변경

32의 `SYS_WRITE`는 `(EBX=string_ptr)` 형태로 null-terminated 문자열을 직접 받았다. 33부터는 `(EBX=fd, ECX=buf, EDX=len)`으로 바뀐다. 커널 내부에서 콘솔을 직접 부르는 경로가 사라지고 VFS를 통한다.

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
init: stdout via fd 1
child: inherited fd 1 works
process 1 exited: code=0
init: child done
init: read: hello, world!
init: fd-stdio done
process 0 exited: code=0
processes: init exited code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/console.h` | 수정 | `console_putchar(u8 c)` 선언 추가 |
| `boot/console.c` | 수정 | `console_putchar` 공개 함수 추가 (`console_put_char` 래퍼) |
| `boot/console_dev.h` | 신규 | console VFS 백엔드 헤더 (`console_dev_open` 선언) |
| `boot/console_dev.c` | 신규 | `console_ops` vtable + `con_write` → `console_putchar`; `console_dev_open` 구현 |
| `boot/vfs.h` | 수정 | `vfs_ops_t`에 `write` 함수 포인터 추가; `vfs_write`, `vfs_dup` 선언 |
| `boot/vfs.c` | 수정 | `vfs_write` — NULL 체크 후 vtable 디스패치; `vfs_dup` — 같은 ops로 새 `vfs_file_t` 할당 |
| `boot/process.c` | 수정 | `console_dev.h` 포함; `proc_spawn` — fd 0/1/2 console 열기; `proc_fork` — `vfs_dup`으로 fd 복사; `proc_exec` — fd 3 이상만 닫기 |
| `boot/syscall.c` | 수정 | `sys_write` → `(fd, buf, len)` 인터페이스로 교체, VFS 경유; `console.h` include 제거 |
| `boot/kernel.c` | 수정 | `initrd_ops`에 NULL write 추가; 부팅 메시지에 "fd-stdio" 추가 |
| `user/init.c` | 수정 | `sys_write(fd, buf, len)` 새 인터페이스; `sys_fork`/`sys_wait` 추가; fork로 fd 1 상속 데모 |
| `Makefile` | 수정 | `CONDEVOBJ` 추가; 빌드·링크 의존성 갱신 |

## 다음 단계 힌트

- `34-vfs-seek`: `vfs_file_t`에 `pos` 필드 추가, `SYS_LSEEK` 구현
