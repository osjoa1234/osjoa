# 34 — vfs-seek

**목표**: `vfs_file_t`에 `pos` 필드를 추가하고, 파일 위치를 VFS 레이어에서 직접 관리한다. `SYS_LSEEK`를 구현해 SEEK_SET/SEEK_CUR/SEEK_END를 지원한다.

**33에서 이어짐**: 33에서는 initrd 백엔드가 내부 `pos`를 직접 관리했다. 여기서는 위치 상태를 `vfs_file_t`로 끌어올려 VFS 레이어가 소유하게 한다. 백엔드 `read`/`write`는 외부에서 받은 `pos`를 사용하는 순수 함수가 된다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### pos 소유권 이동

```
33: initrd_file.pos  ← 백엔드가 상태 소유
34: vfs_file_t.pos   ← VFS 레이어가 상태 소유
    백엔드 read(bfd, buf, len, pos) ← 외부 pos 수신, 내부 상태 없음
```

백엔드는 stateless해지고, seek는 `vfs_file_t.pos`만 조작하면 된다.

### lseek 경로

```
lseek(fd, offset, whence)  ← 유저 syscall
  → SYS_LSEEK              ← 커널 syscall_dispatch
  → p->fds[fd]             ← 프로세스 fd 테이블 조회
  → vfs_seek(f, offset, whence)
  → f->pos = 계산된 새 위치
  → 새 pos 반환
```

### vfs_ops_t 변경

```c
struct vfs_ops {
    int  (*open)(const char *path);
    u32  (*read)(int bfd, u8 *buf, u32 len, u32 pos);   /* pos 추가 */
    u32  (*write)(int bfd, const u8 *buf, u32 len, u32 pos); /* pos 추가 */
    u32  (*size)(int bfd);    /* 신규: SEEK_END용 파일 크기 */
    void (*close)(int bfd);
};
```

`size`가 없는 백엔드(console 등)는 NULL로 초기화한다. `vfs_seek`에서 NULL 여부를 확인하고 SEEK_END를 무시한다.

### SEEK_SET / SEEK_CUR / SEEK_END

```c
case SEEK_SET: newpos = (u32)offset;
case SEEK_CUR: newpos = (u32)((int)f->pos + offset);
case SEEK_END: newpos = (u32)((int)f->ops->size(bfd) + offset);
```

`offset`은 signed int로 음수 역방향 탐색도 가능하다.

### fork 후 pos 상속

`vfs_dup`이 `pos` 필드까지 복사하므로 자식은 부모가 읽던 위치를 그대로 이어받는다. console fd는 pos가 무의미하지만 복사해도 무해하다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 (5초 후 종료)
make clean
```

## 완료 기준

`make run-nogui`:

```
processes: init spawned pid=0
init: stdout via fd 1
child: inherited fd 1 works
process 1 exited: code=0
init: child done
init: read: hello, world!
init: seek+read: world!
init: type something: (키보드 대기 — nogui에서 멈춤)
```

`make run` (QEMU GUI, 키보드 입력):

```
...
init: read: hello, world!
init: seek+read: world!
init: type something: hello
init: stdin got: hello
init: vfs-seek done
process 0 exited: code=0
processes: init exited code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/vfs.h` | 수정 | `vfs_file_t`에 `pos` 필드 추가; `vfs_ops_t`에 `size` 콜백·`read`/`write` pos 파라미터 추가; `vfs_seek` 선언; `SEEK_SET/CUR/END` 상수 |
| `boot/vfs.c` | 수정 | `vfs_open`·`vfs_dup`에서 `pos` 초기화/복사; `vfs_read`/`vfs_write`에서 pos 전달·증가; `vfs_seek` 구현 |
| `boot/initrd.h` | 수정 | `initrd_read` 시그니처에 `u32 pos` 추가 |
| `boot/initrd.c` | 수정 | `struct initrd_file`에서 `pos` 제거; `initrd_open`에서 pos 리셋 제거; `initrd_read`를 stateless로 변경 |
| `boot/console_dev.c` | 수정 | `con_read`/`con_write`에 `u32 pos` 파라미터 추가 (무시); `console_ops`에 `size=NULL` 추가; `console_dev_open`에서 `pos=0` 초기화 |
| `boot/kernel.c` | 수정 | `initrd_ops`에 `initrd_size` 추가; 부팅 메시지 "vfs-seek" |
| `boot/syscall.h` | 수정 | `SYS_LSEEK = 11` 추가 |
| `boot/syscall.c` | 수정 | `sys_lseek` 구현; `SYS_LSEEK` 케이스 추가 |
| `user/init.c` | 수정 | `sys_lseek` 래퍼 추가; seek 데모: 전체 읽기 → SEEK_SET 7 → 나머지 읽기; 메시지 "vfs-seek done" |

## 다음 단계 힌트

- `35-user-shell`: 사용자 모드 셸과 기본 명령 실행
