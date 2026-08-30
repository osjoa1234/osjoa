# 54 — getdents

**목표**: `getdents64` syscall(217)을 ext2 백엔드에 연결해 `ls`가 디스크 ext2 파일시스템(`/disk/`)에서 실제로 동작하게 한다.

**53에서 이어짐**: `53-vfs-ext2-read`는 `open`/`read`/`close`로 파일 *내용*을 읽는 경로만 완성했다 — 디렉토리 자체를 열어 "그 안에 뭐가 있는지" 나열하는 경로는 없었다. `53`의 "다음 단계 힌트"가 정확히 지적했듯, `ext2_scan_dir`이 이미 디렉토리 엔트리를 순회하는 로직(`ext2_find_in_dir_block`)을 갖고 있었으니 그걸 "찾으면 반환"에서 "전부 순회하며 출력"으로 한 번 더 갈래친 것이 이번 단계의 핵심이다. `50-busybox-sh`가 `busybox sh`에서 `ls`를 실행하면 커널 전체가 죽는다는 문제를 발견하고 "52-vfs-ext(현 54)에서 다룰 예정"이라며 완료 기준에서 의도적으로 제외해뒀던 바로 그 자리이기도 하다.

## `vfs_ops_t`에 `getdents`/`mode` 두 연산 추가

```c
struct vfs_ops {
    int  (*open)(const char *path);
    u32  (*read)(int bfd, u8 *buf, u32 len, u32 pos);
    u32  (*write)(int bfd, const u8 *buf, u32 len, u32 pos);
    u32  (*size)(int bfd);
    void (*close)(int bfd);
    void (*dup)(int bfd);
    u32  (*getdents)(int bfd, u8 *buf, u32 len, u32 *pos);   /* 신규 */
    u32  (*mode)(int bfd);                                    /* 신규 */
};
```

`getdents`는 왜 필요한지 자명하지만, `mode`가 왜 같이 필요해졌는지는 "검증에 쓰인 자원" 절에서 설명한다 — 요약하면 `busybox ls`가 대상 경로를 `stat()`으로 먼저 확인하고서야 `getdents64`를 시도하기 때문에, "이게 디렉토리다"라는 정보를 VFS가 백엔드에 물어볼 방법이 없으면 `getdents`를 아무리 잘 구현해도 `ls`는 그 앞에서 죽는다.

`getdents`는 `write`처럼 옵셔널 필드다 — 지금 이 필드를 실제로 채우는 백엔드는 `ext2_getdents` 하나뿐이고, `initrd_ops`/`console_ops`/`pipe_*_ops`는 전부 `0`(디렉토리 개념 자체가 없음)이다. `vfs_getdents`가 `ops->getdents`가 NULL이면 `0`(EOF)을 반환하므로, `initrd` 위에서 `getdents64`를 호출하면 즉시 "빈 디렉토리"로 보인다 — 이건 이번 단계가 고의로 남겨둔 스코프 경계다(아래 "다음 단계 힌트" 참고). 반면 `mode`는 이번에 **모든** `vfs_ops_t` 인스턴스에 실제 구현을 채웠다 — `console_ops`는 `S_IFCHR|0666`, `pipe_*_ops`는 `S_IFIFO|0600`, `initrd_ops`는 `S_IFREG|0644` 고정값, `ext2_ops`만 실제 inode의 `i_mode`를 그대로 반환한다. `write`/`getdents`와 달리 `mode`를 옵셔널로 남기지 않은 이유는, 리눅스에서 `stat()`은 어떤 fd/경로든 실패하지 않는 게 정상이기 때문이다(파일 종류를 모르는 파일은 없다) — 인터페이스 구현 일관성 확인 항목.

`vfs.c`에 얇은 래퍼 세 개가 추가됐다:

```c
u32 vfs_getdents(vfs_file_t *f, u8 *buf, u32 len)
{
    if (!f->ops->getdents) return 0U;
    return f->ops->getdents(f->backend_fd, buf, len, &f->pos);
}
```

`vfs_read`가 `f->pos`를 "몇 바이트 읽었나"로 증가시키는 것과 달리, `vfs_getdents`는 `&f->pos`를 백엔드에 그대로 넘겨 백엔드가 커서 의미론을 완전히 통제하게 한다 — 리눅스에서 디렉토리의 `file->f_pos`가 "바이트 오프셋"이 아니라 파일시스템이 정의하는 불투명한 커서(ext2/ext4는 실제로 디렉토리 블록 안의 바이트 오프셋을 그대로 쓴다)인 것과 같은 이유다. `vfs_mode`/`vfs_size`도 같은 모양의 얇은 래퍼로, `sys_stat`/`sys_fstat`이 백엔드 내부 타입을 몰라도 되게 한다.

## `ext2_getdents`: 디렉토리 엔트리를 `getdents64` ABI로 직렬화

```
53: ext2_scan_dir(dir_inode, name, &out_inode)       — 이름 하나를 찾으면 멈춤
54: ext2_getdents(bfd, buf, len, *pos)                — 멈추지 않고 전부 buf에 직렬화
```

디렉토리의 논리 바이트 오프셋(`cur`)을 커서로 써서 `ext2_resolve_block`으로 블록을 찾고, 그 블록 안의 `ext2_dirent_t`를 리눅스 `struct linux_dirent64` 온디스크 포맷으로 바로 써내려간다:

| 오프셋 | 필드 | 크기 |
|--------|------|------|
| 0 | `d_ino` | 8 |
| 8 | `d_off` | 8 (다음 엔트리의 절대 오프셋 — lseek 재개용 커서) |
| 16 | `d_reclen` | 2 |
| 18 | `d_type` | 1 |
| 19 | `d_name` | 가변, NUL 종단 |

`reclen`은 `(19 + name_len + 1 + 7) & ~7`로 8바이트 정렬한다 — 다음 엔트리의 `d_ino`/`d_off`(둘 다 `u64`)가 8바이트 경계에서 시작해야 하기 때문이다(x86_64는 언얼라인드 접근을 허용하지만, 리눅스 ABI 자체가 이 정렬을 보장하도록 설계돼 있고 `busybox`의 `getdents64` 파서도 이를 전제한다). 이 온디스크 struct를 C 타입으로 선언해 캐스팅하는 대신 `buf + total` 기준 바이트 오프셋으로 직접 써넣었다 — 실제 엔트리 크기가 `reclen`(가변)인데 고정 크기 struct로 캐스팅하면 다음 엔트리 자리까지 덮어쓰거나, 반대로 구조체 패딩 때문에 실제 ABI와 어긋날 위험이 있다(같은 이유로 `ext2_dirent_t`도 `__attribute__((packed))`로 온디스크 포맷을 그대로 반영한다).

`ext2_dirent_t.file_type`(ext2가 이미 저장해두는 파일 종류 태그)을 리눅스 `DT_*` 상수로 매핑하는 `ext2_dtype`도 추가했다:

| ext2 `file_type` | 리눅스 `d_type` |
|---|---|
| `1`(정규 파일) | `DT_REG`(8) |
| `2`(디렉토리) | `DT_DIR`(4) |
| `7`(심볼릭 링크) | `DT_LNK`(10) |
| 그 외 | `DT_UNKNOWN`(0) |

`i_size == 0`인 빈 엔트리(삭제된 파일이 남긴 `inode == 0` 레코드)는 건너뛰되 `cur`는 그 레코드의 `rec_len`만큼 전진시킨다 — ext2는 삭제된 엔트리를 압축하지 않고 앞 엔트리의 `rec_len`을 늘려 흡수하거나 `inode=0`으로만 표시하기 때문에, 이 스킵 로직이 없으면 그 자리에서 멈추거나 죽은 엔트리를 살아있는 것처럼 반환하게 된다.

## 진짜 병목은 `getdents`가 아니라 그 앞뒤였다

`ext2_getdents`를 연결하고 `busybox ls /disk`를 실제로 돌려보니, 순서대로 세 가지 벽에 부딪혔다. 셋 다 "getdents 하나만 만들면 `ls`가 된다"는 최초 가정이 스코프를 과소평가했다는 신호였고, 셋 다 `ls`가 실제로 동작하려면 반드시 필요한 것들이라 이번 단계 안에서 같이 고쳤다.

### 1. `vfs_open`의 마운트 포인트 자체는 열 수 없었다

`smatch(str, prefix)`는 `str`가 `prefix`로 시작하는지만 봤다 — `/disk/`라는 접두사에 대해 `/disk/hello.txt`는 매치하지만 **`/disk` 그 자체는 매치하지 않았다**(`prefix[5]`가 `/`인데 `str[5]`가 `'\0'`라 문자 비교에서 바로 탈락). `busybox ls /disk`는 정확히 이 경로로 `stat("/disk")`를 호출하는데, 이게 실패하면 `getdents`가 아무리 잘 동작해도 `ls`는 그 앞에서 "Operation not permitted"로 죽는다. 이건 53까지는 아무도 마운트 포인트 자체를 여는 코드를 짠 적이 없어서 드러나지 않았던 잠재 버그였다(`53`의 검증 코드도 항상 `/disk/무언가`만 열었다).

`smatch`를 "prefix 전체가 매치하거나(A), 혹은 prefix에서 끝의 `/` 하나를 뺀 나머지까지만 매치하고 `str`가 거기서 끝나거나(B)"로 확장했다:

```c
static int smatch(const char *str, const char *prefix, u32 *out_skip)
{
    u32 n = 0U;
    while (prefix[n] && str[n] == prefix[n]) n++;

    if (prefix[n] == '\0') { *out_skip = n; return 1; }                       /* A */
    if (prefix[n] == '/' && prefix[n+1] == '\0' && str[n] == '\0') {          /* B */
        *out_skip = n; return 1;
    }
    return 0;
}
```

반환값을 "매치 여부"에서 "몇 바이트를 건너뛸지(`out_skip`)"로 바꾼 것도 같이 필요했다 — 기존엔 `path + slen(prefix)`로 건너뛸 길이를 항상 prefix 전체 길이로 계산했는데, 케이스 B(`/disk` 자체를 여는 경우)에선 `prefix` 전체 길이(6, `"/disk/"`)만큼 건너뛰면 문자열 끝을 넘어가버린다 — 실제로 건너뛴 길이(5)를 매치 함수가 직접 알려줘야 한다. `initrd_ops`의 마운트 접두사(`"/"`)는 케이스 A로 항상 처리되므로(어떤 절대경로든 `prefix[1]='\0'`에서 바로 전체 매치) 이번 변경으로 동작이 달라지지 않는다 — `git diff`로 `initrd_open` 호출 인자가 이전과 동일한 걸 확인했다.

### 2. `stat_t`가 리눅스 x86_64 ABI와 다른 레이아웃이었다

`sys_fstat`은 47단계 즈음부터 존재했지만 `st_mode`를 항상 캐릭터 디바이스 고정값으로 채우는 스텁이었다 — 아무도 그 반환값의 필드 순서가 진짜 커널 ABI와 맞는지 검증할 이유가 없었다(musl이 `isatty()` 등에서 fstat을 호출은 하지만 반환값이 뭐든 크게 신경 안 쓰는 경로만 지금까지 탔다). `sys_stat`을 새로 만들면서 처음으로 "이 구조체의 필드 값을 실제로 신뢰해야 하는" 호출자(`busybox`의 `ls`, `stat()`으로 디렉토리인지 판별)가 생겼다.

기존 `stat_t`:

```c
typedef struct { u64 st_dev; u64 st_ino; u32 st_mode; u32 st_nlink; u64 st_size; u64 st_blksize; } stat_t;
```

리눅스 x86_64 진짜 `struct stat`은 `st_nlink`(8바이트)가 `st_mode`보다 **먼저** 오고, 그 사이에 `st_uid`/`st_gid`/패딩/`st_rdev`가 더 있다. 기존 레이아웃대로면 musl이 오프셋 16에서 읽는 `st_mode`(4바이트)는 실제로 커널이 그 자리에 쓴 값과 우연히 맞았을 수도 있지만(양쪽 다 struct 맨 앞 8+8을 `dev`/`ino`로 쓰니까), 오프셋 48에서 읽는 `st_size`는 우리 구조체에서 그 자리가 완전히 다른 필드(`st_blksize` 중간)를 가리켜 항상 쓰레기 값이었다. `mode`가 이전엔 상수라 우연히 안 터졌을 뿐, `ls`가 `st_mode`로 `S_ISDIR`을 판별하려는 순간 이 어긋남이 정확히 "디렉토리인데 파일로 보임" 증상으로 나타났다.

리눅스 x86_64 ABI에 맞춰 필드 순서/크기/개수를 다시 맞췄다(`st_uid`/`st_gid`/`st_rdev`/타임스탬프 3종/예약 필드까지 포함해 `sizeof == 144`, 실제 커널 `struct stat`과 동일):

```c
typedef struct {
    u64 st_dev; u64 st_ino; u64 st_nlink;
    u32 st_mode; u32 st_uid; u32 st_gid; u32 __pad0;
    u64 st_rdev; u64 st_size; u64 st_blksize; u64 st_blocks;
    u64 st_atime; u64 st_atime_nsec;
    u64 st_mtime; u64 st_mtime_nsec;
    u64 st_ctime; u64 st_ctime_nsec;
    u64 __unused[3];
} stat_t;
```

`st_uid`/`st_gid`/타임스탬프/`st_blocks`는 여전히 0으로만 채운다(권한·시간 개념 자체가 이 커널에 아직 없음) — 지금 값을 채워 넣는 필드는 `st_mode`(`vfs_mode`)와 `st_size`(`vfs_size`)뿐이다. 이건 "ls가 디렉토리인지 판별하는 데 필요한 최소"를 채운 것이지 `stat(2)`의 완전한 구현이 아니다.

### 3. `SYS_STAT`(4)에 이어 `SYS_LSTAT`(6)도 필요했다

`getdents64`로 얻은 이름 목록을 나열하기 전에 `busybox ls`는 각 엔트리마다 `lstat()`을 부른다(심볼릭 링크를 따라가지 않고 그 항목 자체의 종류를 알기 위해서 — 색깔 강조나 `-F` 없이도 기본 동작이다). 이 커널엔 심볼릭 링크가 아예 없으므로 `lstat`과 `stat`이 다를 이유가 없다 — 그래서 `SYS_LSTAT` 디스패치를 `SYS_STAT`과 같은 `sys_stat` 핸들러로 합쳤다. 심볼릭 링크가 생기는 순간(스코프 밖) 이 둘은 갈라져야 한다.

세 가지를 다 고치고 나서야 `busybox ls /disk`가 `README`/`doubleindirect.txt`/`hello.txt`/`lost+found`/`multiblock.txt`/`singleindirect.txt`/`sub` 7개 항목을 정확히 출력하고 `exit code=0`으로 끝났다(`syscall: unimplemented rax=228`(`clock_gettime`)과 `rax=72`(`fcntl`)는 로그만 남기고 무시되는데, `50-busybox-sh`에서 이미 확인한 것과 같은 "실패해도 graceful하게 넘어가는" 종류라 새로 처리하지 않았다).

## 검증: 커널 syscall 직접 호출 + 진짜 `busybox ls`

53까지의 패턴을 그대로 이었다 — `user/init.c`가 셸 프롬프트 진입 전에 자동으로 검증한다. 이번엔 두 층위로 나눴다:

1. **`check_getdents(path, want)`**: `sys_getdents64`(신규 raw syscall 래퍼, syscall 217)를 직접 호출해 `/disk/`(루트)에서 `hello.txt`/`sub`를, `/disk/sub`에서 `nested.txt`를 찾는다. `dirent_has_name`이 커널이 만든 `linux_dirent64` 버퍼를 유저 공간에서 직접 파싱(`d_reclen`으로 다음 엔트리로 건너뛰기)해 원하는 이름이 있는지 확인한다 — ext2 백엔드의 `getdents` 구현 자체를 busybox라는 외부 변수 없이 결정론적으로 검증한다.
2. **`busybox ls /disk`**: `run_argv`(신규 — `sys_fork`+`sys_exec`+`sys_wait`를 한데 묶은 헬퍼, 파이프라인 실행 코드와 같은 패턴)로 진짜 `busybox` 바이너리를 실행해 그 출력을 그대로 콘솔에 남긴다 — 이번 단계의 완료 기준이 "getdents 구현"이 아니라 "**`ls`가 동작**"이므로, 커널 내부 검증만으로는 부족하고 실제 바이너리로 확인해야 의미가 있다.

`check_getdents`는 `"/disk/"`(트레일링 슬래시 포함)로 루트를 연다 — 이번에 고친 마운트 포인트 매치(위 "1번" 항목)는 `"/disk"`도 지원하지만, 굳이 트레일링 슬래시 있는 형태로 검증한 이유는 이 함수가 확인하려는 대상이 "getdents 자체가 맞게 동작하는가"이지 "마운트 포인트 매치 버그가 고쳐졌는가"가 아니기 때문이다 — 후자는 `busybox ls /disk`(트레일링 슬래시 없음)가 실제로 검증한다.

## 이전 단계와의 전제조건 확인

- **읽기 전용 불변식 유지**: `ata_write_sector`는 여전히 정의만 있고 호출부가 없다(`grep -n ata_write_sector boot/*.c`). `make run-nogui`를 두 번 실행해 `disk.img` md5sum이 그대로임을 확인했다.
- **`vfs_open` 매치 로직 변경이 기존 마운트 순회를 깨지 않음**: `initrd_ops`(prefix `"/"`)는 케이스 A(prefix 전체 매치)로만 걸리므로 `path + skip` 결과가 이전 `path + slen(prefix)`와 항상 같다 — initrd 경로 탐색(`proc_exec`가 여전히 `initrd_open`을 직접 호출하는 것과는 별개로, VFS 경유 initrd 접근도) 회귀 없음.
- **`sys_fstat`도 같이 고쳐 fd 기반/경로 기반 stat의 의미론을 통일**: `sys_stat`을 만들면서 `vfs_mode`/`vfs_size`라는 실제 조회 경로가 생겼는데, `sys_fstat`만 옛날 고정 스텁(`char device + 0666`)으로 남겨두면 같은 파일을 `fstat(fd)`와 `stat(path)`로 각각 조회했을 때 리눅스라면 당연히 같아야 할 결과가 갈라진다 — 인터페이스 구현 일관성 확인 항목이라 `sys_fstat`도 `vfs_mode`/`vfs_size`를 쓰도록 같이 고쳤다.

## 완료 기준

`make run-nogui`에서 `ata: primary master ready` 다음, `$` 프롬프트 직전에 다음이 보이면 성공이다:

```
shell: ext2 /disk/sub/nested.txt: hello nested dir
shell: ext2 getdents /disk/ hello.txt: found
shell: ext2 getdents /disk/ sub: found
shell: ext2 getdents /disk/sub nested.txt: found
shell: busybox ls /disk:
syscall: unimplemented rax=228
syscall: unimplemented rax=72
README
doubleindirect.txt
hello.txt
lost+found
multiblock.txt
singleindirect.txt
sub
process 1 exited: code=0
$
```

## 이전 단계(53) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/vfs.h` | 수정 | `vfs_ops_t`에 `getdents`/`mode` 필드 추가; `vfs_getdents`/`vfs_mode`/`vfs_size` 선언 추가 |
| `boot/vfs.c` | 수정 | `smatch`를 "매치 여부"에서 "몇 바이트를 건너뛸지"로 확장해 마운트 포인트 자체(`/disk`, 트레일링 슬래시 없이)도 열리게 수정(`slen` 제거, 호출부는 `skip` 사용); `vfs_getdents`/`vfs_mode`/`vfs_size` 구현; `vfs_open`이 prefix를 다 뗀 나머지가 빈 문자열이면 `"/"`를 대신 백엔드 `open()`에 넘기도록 수정(아래 "다음 단계 힌트" 참고) |
| `boot/ext2.h` | 수정 | `ext2_getdents`/`ext2_mode` 선언 추가 |
| `boot/ext2.c` | 수정 | `ext2_dtype`(ext2 `file_type` → 리눅스 `DT_*` 매핑), `ext2_getdents`(디렉토리 바이트 오프셋 커서 기반으로 `linux_dirent64` ABI를 유저 버퍼에 직렬화), `ext2_mode`(캐싱된 inode의 `i_mode` 그대로 반환) 추가 |
| `boot/initrd.h` | 수정 | `initrd_mode` 선언 추가 |
| `boot/initrd.c` | 수정 | `initrd_mode` — 모든 파일을 `S_IFREG\|0644` 고정값으로 보고(디렉토리 개념 없음) |
| `boot/console_dev.c` | 수정 | `con_mode` 추가(`S_IFCHR\|0666`), `console_ops`에 연결 |
| `boot/pipe.c` | 수정 | `pipe_mode` 추가(`S_IFIFO\|0600`), `pipe_read_ops`/`pipe_write_ops`에 연결 |
| `boot/syscall.h` | 수정 | `SYS_STAT=4`/`SYS_LSTAT=6`/`SYS_GETDENTS64=217` 추가 |
| `boot/syscall.c` | 수정 | `stat_t`를 리눅스 x86_64 ABI 실제 레이아웃(144바이트)으로 재정의; `sys_fstat`이 `vfs_mode`/`vfs_size`로 실제 값을 채우도록 수정; `sys_stat`(경로 기반, `SYS_STAT`/`SYS_LSTAT` 공용) 신규; `sys_getdents64` 신규, 디스패치에 세 case 추가 |
| `boot/kernel.c` | 수정 | `initrd_ops`/`ext2_ops` 초기화에 `getdents`/`mode` 필드 추가(`ext2_ops`만 실제 `ext2_getdents` 연결, `initrd_ops.getdents`는 `0`) |
| `user/init.c` | 수정 | `sys_getdents64` raw syscall 래퍼 추가; `check_getdents`(커널 getdents 구현을 결정론적으로 검증)와 `dirent_has_name`(유저 공간에서 `linux_dirent64` 파싱) 추가; `run_argv`(`fork`+`exec`+`wait` 헬퍼) 추가해 `busybox ls /disk`를 셸 프롬프트 진입 전 자동 실행 |
| `rootfs/*`, `boot/ata.c`, `boot/ata.h`, `tools/ext2_testgen.py`, `Makefile` | 변경 없음 | 53의 파일 그대로 |

## 다음 단계 힌트

- `55-ext2-write`: `ext2_ofile_t`의 inode identity 공유(53)와 이번 단계의 `getdents`/`stat` 모두 읽기 전용 상태를 전제로 짜여 있다 — 쓰기가 생기면 (1) 디렉토리에 엔트리를 추가/삭제하는 경로가 `ext2_getdents`가 순회하는 바로 그 온디스크 포맷을 갱신해야 하고, (2) `sys_stat`이 반환하는 `st_size`가 쓰기 직후에도 최신이어야 한다(53에서 이미 지적한 inode identity 공유 덕에 `g_ofiles[i].inode`만 갱신하면 같은 파일을 연 다른 fd도 최신을 보긴 하지만, 아직 디스크에 flush되지 않은 상태에서 새로 `ext2_open`하면 옛날 값을 다시 읽어올 수 있다는 coherency 문제는 여전히 남아있다).
- **initrd는 `getdents`를 지원하지 않는다**: `initrd_ops.getdents = 0`이라 `ls /`(initrd 루트)는 VFS 경유로는 빈 디렉토리처럼 보인다 — initrd가 애초에 평평한 파일 목록(디렉토리 개념 없음)이라 이번 스코프에서 의도적으로 손대지 않았다. `busybox`가 initrd 안에 있는 실행 파일이라 `busybox ls /`를 치면 이 한계가 바로 보인다. initrd에 디렉토리 개념을 부여하는 건 이번 로드맵에 없다 — 필요해지면 그때 별도 스코프로 다룰 것.
- **`getdents`가 direct 블록만 검증됐다**: `ext2_resolve_block`을 재사용하므로 이론적으로 indirect 디렉토리 블록도 처리는 되지만, 지금 테스트 디렉토리(`rootfs/`, `rootfs/sub/`)는 전부 direct 블록 12개 안에 들어가는 크기라 indirect 디렉토리 블록 경로는 53의 triple indirect와 같은 이유로 실제 데이터로는 검증되지 않았다.
- **`stat`/`fstat`이 여전히 uid/gid/타임스탬프를 0으로만 채운다**: `busybox ls -l`처럼 이 필드들을 실제로 보여주는 명령을 쓰면 전부 `1970-01-01`/`uid 0`으로 나온다 — 이 커널에 아직 사용자/시간 개념이 없어서다. 나중에 타이머(`15-pit-timer`)나 RTC를 `st_*time`에 연결하거나 사용자/권한 모델이 생기면 그때 채울 자리다.
- **모든 syscall 실패가 여전히 `-1`(EPERM) 하나로 뭉개진다**: `sys_stat`이 `vfs_open` 실패 시 반환하는 값도 마찬가지다(존재하지 않는 경로든 권한 문제든 구분 없이 `-1`). 53의 다음 단계 힌트에서 이미 지적된 채로 남아있다 — `ENOENT`/`ENOTDIR` 등 errno 구분이 필요해지면 그때 다룰 것.
- **`vfs_open`의 마운트 포인트 매치는 여전히 문자열 레벨 땜빵이다**: `smatch`가 "prefix 전체 매치" 외에 "트레일링 슬래시 하나 뺀 매치"를 특수 케이스로 처리하고, 그렇게 매치돼 나머지 문자열이 비면 `vfs_open`이 `"/"`를 대신 넘기는 것도 마찬가지로 특수 케이스다. 리눅스는 이 문제 자체가 구조적으로 발생하지 않는다 — VFS 코어가 경로를 컴포넌트 단위로 잘라 dentry 트리를 순회하고(`lookup(dir_inode, name)`을 컴포넌트마다 호출), 마운트 지점을 만나면 경로가 거기서 끝나든 안 끝나든 상관없이 해당 `vfsmount`의 캐시된 루트 dentry(`mnt_root`)로 무조건 건너뛰기 때문에 "prefix를 다 뗀 나머지가 빈 문자열"이라는 상황 자체가 생기지 않는다. 우리 쪽에서 이걸 구조적으로 고치려면 `vfs_ops_t.open(path)`(경로 문자열 전체를 백엔드에 넘기는 지금 인터페이스)를 `lookup(parent, name)`(컴포넌트 하나씩 넘기는 인터페이스)로 바꾸고 `vfs_open`이 직접 컴포넌트 순회 + 마운트 크로싱을 수행하는 구조로 재설계해야 한다 — `initrd`/`ext2`/`console`/`pipe` 백엔드 전부의 인터페이스가 바뀌는 큰 리팩터링이라 이번 로드맵엔 없다. 필요해지면(디렉토리 트리 기반 VFS로 전환하는 별도 단계가 생기면) 그때 다룰 것.
