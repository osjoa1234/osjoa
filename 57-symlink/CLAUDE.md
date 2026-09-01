# 57 — symlink

**목표**: ext2에 심볼릭 링크(`S_IFLNK`) 쓰기 지원을 추가한다 — `symlink(2)`로 생성, `readlink(2)`로 대상 문자열을 읽고, 경로 탐색(`ext2_resolve_path`)이 중간 컴포넌트와 최종 컴포넌트 모두에서 심링크를 따라가도록 확장한다. `proc_exec`는 아직 initrd를 직접 조회하므로 이번 단계에서 건드리지 않는다(58에서 VFS 경유로 리팩터링하며 다룬다) — 이번 단계는 커널 레벨 자체 검증만 한다.

**56에서 이어짐**: 56까지는 파일 타입이 `S_IFREG`/`S_IFDIR` 둘뿐이었다. `EXT2_FT_SYMLINK`/`DT_LNK`는 이미 52~53 시절부터 `ext2_dtype`에 매핑만 돼 있었을 뿐 실제로 만들어내는 코드는 없었다 — 이번 단계가 그 자리를 처음 채운다.

## fast symlink vs slow symlink — 실제 ext2 온디스크 표현을 그대로 따름

리눅스 ext2는 심링크 대상 문자열이 `sizeof(inode->i_block)`(60바이트) 미만이면 별도 데이터 블록을 할당하지 않고 inode의 `i_block[15]` 배열(원래 direct/indirect 블록 포인터가 들어갈 자리) 안에 문자열을 직접 욱여넣는다("fast symlink", `i_blocks == 0`). 60바이트 이상이면 일반 파일처럼 데이터 블록에 문자열을 쓴다("slow symlink"). `ext2_symlink`가 이 둘을 그대로 구현한다:

```c
if (target_len < sizeof(new_inode.i_block)) {
    /* fast: i_block을 char*로 캐스트해 문자열을 직접 씀, i_blocks=0 그대로 */
} else {
    /* slow: ext2_get_or_alloc_block(fs, &new_inode, 0, ...)로 블록 하나 확보 후 write */
}
```

`ext2_read_symlink_target`(읽기 쪽)은 `inode->i_blocks == 0`을 기준으로 fast/slow를 구분한다 — 이게 실제 리눅스 커널(`fs/ext2/symlink.c`)이 fast symlink를 판별하는 바로 그 조건이다. `i_size`만으로 구분하지 않은 이유: 향후 어떤 이유로든 60바이트 미만인데 블록에 저장된 상태가 생기더라도(정상적으로는 발생하지 않지만) `i_blocks`가 진실을 말해주는 필드이기 때문이다.

`e2fsck`가 이 구분을 실제로 검증한다는 걸 확인했다 — `debugfs -R "stat /hello_link"`는 `Blockcount: 0`과 `Fast link dest: "hello.txt"`를 보여주고, 60바이트 넘는 `long_link`는 `Blockcount: 2`(블록 하나 = 섹터 2개, 1024바이트 블록 기준)를 보여주며 "Fast link dest" 줄 자체가 없다 — `debugfs -R "cat /long_link"`로 블록 내용도 그대로 읽힌다.

심링크 대상 문자열의 최대 길이는 `EXT2_MAX_PATH`(128, 이 코드베이스가 경로 버퍼 전반에 이미 쓰던 상수)로 제한했다 — `ext2_read_symlink_target`이 단일 블록(`ext2_resolve_block(fs, inode, 0, ...)`)만 읽는 이유이기도 하다: `block_size=1024`(Makefile의 `mkfs.ext2 -b 1024`)가 128보다 훨씬 크므로 slow symlink라도 항상 블록 하나 안에 들어간다는 게 보장된다. 55/56의 indirect 블록 체인 순회 로직을 재사용할 필요가 없다.

`i_mode`는 `S_IFLNK | 0777`(새 상수 `EXT2_DEFAULT_LMODE`) — 리눅스도 심링크는 항상 0777로 만들고 실제 접근 제어는 대상 파일의 권한이 담당한다(mkdir의 `EXT2_DEFAULT_DMODE`/파일의 `EXT2_DEFAULT_FMODE`와 같은 스코프 결정: mode 인자를 받는 인터페이스가 없어 고정값).

## 경로 탐색이 심링크를 따라가도록 확장 — `ext2_resolve_from`

기존 `ext2_resolve_path`는 루트에서 시작해 세그먼트를 하나씩 스캔하며 내려가는 단순 루프였다. 이번 단계에서 재귀 함수 `ext2_resolve_from(fs, start_inode_num, path, out_inode, depth)`로 바꿨다 — 세그먼트를 스캔해 얻은 inode가 `S_IFLNK`면:

1. `ext2_read_symlink_target`으로 대상 문자열을 읽고,
2. 대상이 `/`로 시작하면 새 탐색의 시작점을 `EXT2_ROOT_INO`로, 아니면 **지금 이 심링크를 담고 있던 디렉토리의 inode 번호**(`cur_inode_num`, 즉 relative 심링크가 실제로 상대적이어야 하는 기준)로 잡고,
3. `ext2_resolve_from`을 그 시작점과 대상 문자열로 재귀 호출해 최종 inode를 얻은 뒤, 원래 루프를 이어간다.

문자열을 이어붙여 경로를 재구성하는 방식(예: `newpath = target + "/" + remaining`) 대신 "탐색 시작점(inode 번호)을 바꿔서 대상 문자열 자체를 새 경로로 재귀 호출"하는 방식을 택했다 — relative 심링크의 기준 디렉토리를 이미 inode 번호로 들고 있으므로 그 경로 문자열을 몰라도 되고, 남은 경로(`p`)는 바깥쪽 루프가 그대로 이어서 처리하므로 원본 경로 버퍼를 훼손하거나 잘라 붙일 필요가 없다.

**중간 컴포넌트와 최종 컴포넌트를 구분하지 않는다** — 세그먼트 스캔 루프 자체가 모든 세그먼트(마지막 포함)에 대해 이 심링크-따라가기를 적용하므로, `ext2_open`이 `ext2_resolve_path(fs, path, ...)`를 전체 경로에 그대로 호출하는 것만으로 리눅스 `open(2)` 기본 동작(모든 컴포넌트에서 심링크를 따라감, `O_NOFOLLOW` 없음)과 정확히 같아진다. `ext2_mkdir`/`ext2_unlink`/`ext2_symlink`/`ext2_readlink`는 (55/56이 이미 그랬듯) `ext2_split_parent`로 부모/이름을 나눠 **부모만** `ext2_resolve_path`(심링크 따라감)로 찾고 최종 이름은 `ext2_scan_dir`로 직접 찾는 패턴이라, 자연히 "최종 컴포넌트는 심링크 자체를 대상으로 함"이 되어 별도 처리 없이 리눅스의 `unlink(2)`/`readlink(2)` 의미론과 맞아떨어진다 — 이건 56이 `ext2_unlink`를 이 패턴으로 짰을 때 이미 세워둔 구조를 그대로 재사용한 것이지 이번 단계가 새로 설계한 게 아니다.

**루프 방지**: `depth` 인자가 `EXT2_SYMLINK_MAX_DEPTH`(8)를 넘으면 즉시 실패한다 — 리눅스의 `ELOOP`과 같은 역할이나, 이 코드베이스의 나머지 에러 처리와 마찬가지로 구분된 에러코드 없이 단순 `-1`이다. `loopa -> loopb -> loopa -> ...` 순환을 만들어 `open`이 무한루프 없이 실패로 끝나는지 검증했다(아래 "검증" 참고).

## `/disk/` 마운트 프리픽스와 심링크 절대경로의 관계 — 의도된 단순화

`ext2_resolve_from`은 절대경로 심링크 대상을 **ext2 파일시스템 자신의 루트** 기준으로 해석한다(`EXT2_ROOT_INO`에서 시작) — VFS 마운트 테이블(`vfs.c`)이 어디에 이 파일시스템을 붙였는지(`/disk/`)는 전혀 모른다. 즉 `/disk/` 안에서 `symlink("/hello.txt", "/disk/abs_link")`처럼 **ext2 루트 기준** 절대경로를 대상으로 써야 하고, `symlink("/disk/hello.txt", ...)`처럼 마운트 프리픽스를 포함한 전체 VFS 경로를 대상으로 쓰면 ext2 안에 존재하지 않는 `disk`라는 이름의 서브디렉토리를 찾으려다 실패한다.

실제 리눅스는 심링크 절대경로를 전체 VFS 루트(마운트 트리 전체) 기준으로 해석하므로 이 프로젝트의 동작은 리눅스와 다르다. 이 차이를 감수한 이유: 이 프로젝트의 `vfs.c`는 재귀적 마운트 트리/dentry 캐시가 아니라 단순 prefix-매칭 평면 테이블이라, "절대경로 심링크가 다른 마운트로 되돌아 나갈 수 있어야 한다"는 걸 지원하려면 `ext2_resolve_from`이 `ext2.c` 내부에서 자기 완결적으로 도는 대신 `vfs.c`의 마운트 테이블을 다시 순회해 백엔드를 바꿔 타야 하는데, 그러려면 vfs 계층에 "경로를 열지 않고 (백엔드, inode 유사값)으로만 해석하는" 새 제네릭 인터페이스가 필요하다 — 로드맵 어디에도 그런 마운트 트리 재설계가 예정돼 있지 않으므로 지금 범위를 넘는 설계 변경이라고 판단했다. 이 프로젝트에는 쓰기 가능한 파일시스템이 `/disk/` 하나뿐이라 실질적 제약은 크지 않다.

## `SYS_LSTAT`을 `SYS_STAT`에서 분리 — 이번 단계가 드러낸 회귀

56까지 `SYS_LSTAT`은 `SYS_STAT`과 같은 `sys_stat()`(내부적으로 `vfs_open`으로 완전히 따라가서 연다)을 호출하도록 합쳐져 있었다 — 심링크 자체가 없던 시절엔 stat과 lstat이 관찰 가능한 차이가 없어 문제가 안 됐다. 이번 단계로 심링크가 생기자마자 이 합침이 실제 오류가 됐다: `busybox ls /disk`가 `dangling_link`/`loopa`/`loopb`/`long_link`(대상이 없거나 루프인 심링크)에 대해 `lstat`을 호출했을 때 커널이 여전히 대상을 "따라가서" 열려고 시도해 실패 → `ls: /disk/dangling_link: Operation not permitted`가 찍혔다. 실제 리눅스의 `ls`는 `lstat`(대상을 따라가지 않음)만으로 디렉토리 엔트리를 나열하므로 대상이 없는 심링크도 이름만으로 문제없이 나열한다 — 이 프로젝트의 동작이 그 기준에서 어긋난 것이므로 고쳤다.

고친 내용: `vfs_ops_t`에 `int (*lstat)(const char *path, u32 *out_mode, u32 *out_size)`를 추가하고, `ext2_lstat`을 `ext2_readlink`/`ext2_unlink`와 같은 패턴(`ext2_split_parent`로 부모/이름 분리 → 부모만 `ext2_resolve_path`로 따라가서 찾음 → 최종 이름은 `ext2_scan_dir`로 심링크째로 찾아 그 inode의 `i_mode`/`i_size`를 그대로 반환)으로 구현했다. `syscall.c`의 `SYS_LSTAT` 디스패치를 별도 `sys_lstat()`(파일을 열지 않고 `vfs_lstat`만 호출)로 분리했다. 커널 레벨 검증으로 `check_lstat_is_link`를 추가해 `dangling_link`/`loopa`처럼 `open`은 실패해야 하지만 `lstat`은 성공하고 `st_mode`에 `S_IFLNK`가 찍히는지 확인했다.

이건 "이번 단계가 새로 만든 서브시스템이 이미 있던 서브시스템의 전제조건을 깨뜨리지 않았는지"의 사례라기보다, **"이미 있던 두 진입점(`stat`/`lstat`)이 우연히 같은 동작이었던 게, 이번 단계가 그 둘을 관찰 가능하게 갈라놓는 새 타입(심링크)을 만들면서 처음으로 잘못된 동작으로 드러난"** 경우다 — 원인이 56 이전 코드에 있었더라도, 그 잘못이 실제로 나타나게 만든 것도 고쳐야 하는 것도 이번 단계다.

## `SYS_OPEN` 실패 반환값의 부호 확장 버그 — 자체 검증 중 발견

`open-dangling-symlink`/`open-symlink-loop` 검증(아래 "검증" 참고)을 처음 짰을 때 커널은 두 경우 모두 올바르게 `-1`을 반환하는데도 유저 공간의 `check_open_fails`가 "UNEXPECTED SUCCESS"를 찍었다. 원인을 추적하니 `sys_open`(내부 구현, `syscall.c`)이 `u32` 타입으로 실패를 `(u32)-1U`(`0xFFFFFFFF`)로 반환하는데, 디스패치 코드가 `frame->rax = sys_open(...)`로 이 `u32` 값을 `u64` 필드(`frame->rax`)에 그냥 대입하고 있었다 — C의 정수 승격 규칙상 이건 부호 확장이 아니라 **0으로 채우는 확장**이라 `frame->rax`엔 `0x00000000FFFFFFFF`(약 43억, 부호 있는 64비트 정수로 보면 여전히 양수)가 들어간다. 유저 공간의 `long ret; asm("syscall" : "=a"(ret) ...)`은 `rax` 전체 64비트를 읽으므로 `ret`은 저 큰 양수가 되고, 기존 검증 코드 전반이 쓰던 관용구 `if (fd < 0)`는 이 값을 실패로 인식하지 못한다.

이 버그는 56까지 한 번도 드러나지 않았다 — `sys_open`이 실패해야 하는 시나리오(존재하지 않는 파일을 여는 것) 자체가 이전 단계들의 검증 목록에 없었기 때문이다("커널이 진짜로 open을 거부하는지"를 처음 검증한 게 이번 단계의 `open-dangling-symlink`/`open-symlink-loop`다). 고친 내용: `SYS_OPEN` 디스패치에서 `(u64)(long long)(int)sys_open(...)`로 캐스팅 체인을 명시해, `u32` 실패값을 먼저 부호 있는 32비트로 재해석(`(int)0xFFFFFFFFU == -1`)한 뒤 64비트로 부호 확장시키도록 고쳤다. 성공 경로(작은 양의 fd 인덱스)는 이 캐스팅 체인을 거쳐도 값이 그대로다.

이 부호 확장 문제 자체는 `sys_open`뿐 아니라 `u32`를 반환하고 `(u32)-1U`를 실패 센티널로 쓰는 다른 syscall들(`sys_read`, `sys_write`, `sys_lseek` 등)에도 잠재적으로 있을 수 있지만, 이번 단계가 실제로 의존하는 건 `open`의 실패 경로뿐이라 그것만 고쳤다 — 나머지는 "다음 단계 힌트" 참고.

## 검증: 커널 레벨 syscall 직접 호출만 (busybox 연동은 58로 미룸)

로드맵이 명시한 대로 이번 단계는 busybox와의 상호작용(예: `busybox ln -s`)을 검증하지 않는다 — 다만 56부터 이미 있던 `busybox ls /disk` 호출이 이번 단계가 만든 심링크들 위에서 그대로 실행되므로, 그 결과가 (의도치 않게) 두 번째 검증축 역할을 했다(위 "SYS_LSTAT" 절 참고).

1. **relative fast symlink**: `symlink("hello.txt", "/disk/hello_link")` → `readlink`로 대상 문자열이 정확히 "hello.txt"인지, `open`+`read`로 내용이 `hello.txt`와 같은지 확인.
2. **absolute fast symlink**: `symlink("/hello.txt", "/disk/abs_link")` → 위와 동일한 왕복 확인. 마운트 프리픽스를 포함하지 않은 ext2-루트-기준 절대경로여야 함(위 "절대경로 심링크" 절 참고).
3. **디렉토리를 가리키는 심링크 + 중간 컴포넌트 통과**: `symlink("sub", "/disk/sub_link")` → `open("/disk/sub_link/nested.txt")`로 `ext2_resolve_from`이 중간 세그먼트에서 심링크를 따라 들어가는지 확인.
4. **체인(심링크의 심링크)**: `symlink("hello_link", "/disk/chain_link")` → 재귀 호출이 두 단계 이상 정상 동작하는지 확인.
5. **dangling 심링크**: `symlink("does_not_exist.txt", "/disk/dangling_link")` → `readlink`는 성공(대상 문자열 자체는 존재와 무관), `open`은 실패, `lstat`은 성공하고 `S_IFLNK`가 찍히는지 확인 — 세 검증이 서로 다른 답을 내야 정상이라는 점 자체가 stat/lstat/open의 의미론이 서로 다름을 보여준다.
6. **순환 심링크**: `loopa -> loopb -> loopa` → `open`이 (`EXT2_SYMLINK_MAX_DEPTH` 덕분에) 무한루프 없이 실패로 끝나는지, `lstat`은 여전히 성공하는지 확인.
7. **slow symlink**: 66바이트 대상(`LONG_SYMLINK_TARGET`, 60바이트 inline 한도를 넘김)으로 `symlink`+`readlink` 왕복 확인 — 블록 할당 경로가 실제로 실행됨.
8. **`make run-nogui` 뒤 `e2fsck -f -n build/disk.img`**: Pass 1~5 에러 없이 통과, `33/2048 files, 615/8192 blocks`로 보고. `debugfs -R "stat /hello_link"`/`"stat /abs_link"`가 `Blockcount: 0`과 `Fast link dest: "..."`를, `"stat /long_link"`가 `Blockcount: 2`(블록 하나, 1024바이트 블록 기준 섹터 2개)를 보여주고 "Fast link dest" 줄이 없음을 확인 — fast/slow 구분이 실제 온디스크 표현에서도 올바르다는 이중 확인. `debugfs -R "cat /long_link"`로 블록 내용도 대조.

## 완료 기준

`make run-nogui`에서 56의 mkdir/unlink 로그 다음, `busybox mkdir /disk/bbdir` 직전에 다음이 보이면 성공이다:
```
shell: ext2 symlink /disk/hello_link -> hello.txt: OK
shell: ext2 getdents /disk/ hello_link: found
shell: ext2 readlink /disk/hello_link: OK
shell: ext2 read-via-symlink /disk/hello_link: OK
shell: ext2 symlink /disk/abs_link -> /hello.txt: OK
shell: ext2 readlink /disk/abs_link: OK
shell: ext2 read-via-symlink /disk/abs_link: OK
shell: ext2 symlink /disk/sub_link -> sub: OK
shell: ext2 read-via-symlink /disk/sub_link/nested.txt: OK
shell: ext2 symlink /disk/chain_link -> hello_link: OK
shell: ext2 readlink /disk/chain_link: OK
shell: ext2 read-via-symlink /disk/chain_link: OK
shell: ext2 symlink /disk/dangling_link -> does_not_exist.txt: OK
shell: ext2 readlink /disk/dangling_link: OK
shell: ext2 open-dangling-symlink /disk/dangling_link: OK
shell: ext2 lstat-is-link /disk/dangling_link: OK
shell: ext2 symlink /disk/loopa -> loopb: OK
shell: ext2 symlink /disk/loopb -> loopa: OK
shell: ext2 open-symlink-loop /disk/loopa: OK
shell: ext2 lstat-is-link /disk/loopa: OK
shell: ext2 symlink /disk/long_link -> 0123456789/0123456789/0123456789/0123456789/0123456789/0123456789/: OK
shell: ext2 readlink /disk/long_link: OK
```
그리고 `busybox ls /disk:` 출력이 `syscall: unimplemented` 줄(기존에 있던, 무관한 `clock_gettime`/`fcntl` 미구현 경고) 외에는 에러 없이 모든 항목(`abs_link`, `chain_link`, `dangling_link`, `hello_link`, `long_link`, `loopa`, `loopb`, `sub_link` 포함)을 나열하고 `process 1 exited: code=0`로 끝나야 한다(56까지는 이 줄이 없었지만 이번 단계 전 잠깐 회귀 상태에서 `code=1`이 찍혔었다 — 위 "SYS_LSTAT" 절 참고). `build/disk.img`에 대해 `e2fsck -f -n`이 에러 없이 통과해야 한다.

**주의**: `build/disk.img`는 `rootfs/` 시드 파일에만 의존하므로 `make run-nogui`를 반복 실행하면 이전 실행이 써넣은 내용이 누적된다 — 두 번째 실행부터 `mkdir`/`symlink` 등이 `FAIL`(이미 존재)로 나오는 게 정상이다. 진짜 처음부터 검증하려면 `rm -f build/disk.img` 후 `make run-nogui`를 실행해야 한다(55/56에도 이미 있던 특성이며, 이번 단계가 새로 만든 문제는 아니다).

## 이전 단계(56) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/vfs.h` | 수정 | `vfs_ops_t`에 `symlink`/`readlink`/`lstat` 함수 포인터 추가; `vfs_symlink`/`vfs_readlink`/`vfs_lstat` 선언 추가 |
| `boot/vfs.c` | 수정 | `vfs_symlink`/`vfs_readlink`/`vfs_lstat` 신규 — 기존 마운트 prefix 매칭 폴백 루프 패턴 재사용 |
| `boot/ext2.h` | 수정 | `ext2_symlink`/`ext2_readlink`/`ext2_lstat` 선언 추가 |
| `boot/ext2.c` | 수정 | `EXT2_S_IFLNK`/`EXT2_DEFAULT_LMODE`/`EXT2_SYMLINK_MAX_DEPTH` 상수 추가; `ext2_read_symlink_target`(fast/slow 판별 읽기) 신규; `ext2_resolve_path`를 재귀 `ext2_resolve_from`으로 재구성해 모든 경로 세그먼트에서 심링크를 따라가도록 확장(루프 방지용 depth 인자); `ext2_symlink`(fast/slow 쓰기 분기), `ext2_readlink`(대상 문자열 읽기, 최종 컴포넌트는 안 따라감), `ext2_lstat`(mode/size 조회, 안 따라감) 신규 |
| `boot/kernel.c` | 수정 | `ext2_ops`에 `ext2_symlink`/`ext2_readlink`/`ext2_lstat` 연결 |
| `boot/syscall.h` | 수정 | `SYS_SYMLINK = 88`, `SYS_READLINK = 89` 추가(Linux x86_64 ABI 번호). 기존 `SYS_LSTAT = 6`은 번호 변경 없음 |
| `boot/syscall.c` | 수정 | `sys_symlink`/`sys_readlink`(각각 `vfs_symlink`/`vfs_readlink` 래핑) 신규; `sys_lstat`을 `sys_stat`에서 분리해 `vfs_lstat` 기반으로 새로 구현; `SYS_OPEN` 디스패치의 실패값 부호 확장 버그 수정(`(u64)(long long)(int)sys_open(...)`); `SYS_SYMLINK`/`SYS_READLINK`/(분리된) `SYS_LSTAT` 디스패치 케이스 추가 |
| `user/init.c` | 수정 | `sys_symlink`/`sys_readlink`/`sys_lstat`(raw syscall wrapper) 및 `stat_t` 유저 공간 미러 구조체 추가; `check_symlink`/`check_readlink`/`check_read_via_symlink`/`check_open_fails`/`check_lstat_is_link` 검증 헬퍼 추가; relative/absolute/디렉토리/체인/dangling/순환/slow 심링크 검증 블록을 56의 unlink 검증 직후, busybox mkdir 검증 직전에 추가 |
| 나머지 전부 | 변경 없음 | 56의 파일 그대로 |

## 다음 단계 힌트

- **`proc_exec`는 여전히 initrd 직접 조회**: 58(`58-exec-vfs-symlink`)이 `proc_exec`를 VFS 경유로 리팩터링하면서 이번 단계가 만든 심링크 따라가기가 실행 경로에도 적용되게 한다 — `busybox`를 `cat`/`ls` 등으로 심링크한 멀티콜 바이너리 실행이 그 목표다.
- **절대경로 심링크는 VFS 마운트 트리를 모른다**: 위 "`/disk/` 마운트 프리픽스와 심링크 절대경로의 관계" 절에서 서술한 대로, `/disk/` 안의 절대경로 심링크는 다른 마운트(`/`, initrd)로 되돌아 나갈 수 없고 ext2 자신의 루트 기준으로만 해석된다. 여러 개의 쓰기 가능한 마운트가 생기기 전까지는 실질적 제약이 아니지만, 로드맵에 "진짜 마운트 트리"가 생기면 재검토가 필요하다.
- **`u32` 반환 syscall 전반의 부호 확장 문제**: 이번 단계가 고친 건 `SYS_OPEN` 하나뿐이다. `sys_read`/`sys_write`/`sys_lseek` 등 다른 `u32`-반환 syscall도 실패 시 `(u32)-1U`를 그대로 `frame->rax`(u64)에 대입하므로 이론적으로 같은 문제가 있을 수 있다 — 다만 그 실패 경로들이 실제로 유저 공간에서 부호 비교로 검증된 적이 아직 없어 드러나지 않았을 뿐이다. 다음에 그런 실패 경로를 검증하게 되면 이 패턴을 의심해볼 것.
- **심링크 자체에 대한 `chmod`/소유권 없음**: `i_mode`가 항상 `0777` 고정이고 `i_uid`/`i_gid`도 0으로 고정이다(mkdir/create_file과 같은 스코프 결정) — 사용자별 권한 개념이 이 프로젝트에 아직 없으므로 자연스러운 제약이다.
