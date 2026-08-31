# 56 — mkdir-unlink

**목표**: ext2에 디렉토리 생성(`mkdir`)과 파일 삭제(`unlink`)를 추가한다 — 55가 만든 블록/inode 비트맵 할당기의 "해제" 짝(`ext2_free_block`/`ext2_free_inode`)을 처음 구현하고, 디렉토리 엔트리 삽입(55)의 반대 방향인 삭제(엔트리 병합/`inode=0` 표시)를 추가한다.

**55에서 이어짐**: 55는 alloc만 있고 free가 없었다(55의 "다음 단계 힌트" 참고). 이번 단계가 그 free 쪽을 처음 채운다.

## vfs_ops_t에 mkdir/unlink 추가

`vfs_ops_t`에 `int (*mkdir)(const char *path)`, `int (*unlink)(const char *path)`를 구조체 맨 뒤에 추가했다 — 기존 필드 뒤에 붙이면 pipe.c(`pipe_read_ops`/`pipe_write_ops`)와 console_dev.c(`console_ops`)의 positional struct literal은 C의 부분 초기화 규칙에 따라 자동으로 0(NULL)으로 채워져서, 컴파일러 경고(`-Wmissing-field-initializers`, `-Werror`는 아니므로 빌드는 통과)만 나고 코드 수정은 필요 없었다 — 이 두 백엔드가 애초에 `vfs_mount`에 등록되지 않는다는 55의 관찰과 같은 이유로 mkdir/unlink도 자연히 지원 대상이 아니다.

`vfs_open`과 같은 마운트 prefix 매칭 폴백 루프를 `vfs_mkdir`/`vfs_unlink`에도 그대로 반복해서 적었다(공유 헬퍼로 묶지 않은 이유: "/"가 먼저 마운트되어 모든 경로와 매치하지만 initrd에 없으면 다음 마운트 "/disk/"로 넘어가는 폴백 자체는 `vfs_open`도 mkdir/unlink도 똑같이 필요한데, "성공/실패를 판단하는 기준"이 서로 다르다 — open은 백엔드 호출의 반환값(`bfd>=0`)으로, mkdir/unlink는 함수 포인터가 NULL인지로 판단한다. 이 차이 때문에 억지로 하나로 묶기보다 짧은 루프를 그대로 반복하는 쪽이 기존 스타일과 더 맞다고 판단했다).

## ext2_alloc_block/ext2_alloc_inode의 해제 짝

`ext2_bitmap_alloc`의 짝인 `ext2_bitmap_free(fs, bitmap_block, bit_index)`를 새로 추가했다 — 비트맵 블록을 읽어 해당 비트를 0으로 지우고 다시 쓴다. `ext2_free_block`/`ext2_free_inode`가 각각 `ext2_alloc_block`/`ext2_alloc_inode`와 대칭을 이루며, free count 증가 + `ext2_flush_gd`/`ext2_flush_sb` 호출까지 alloc 쪽과 동일한 패턴을 따른다.

## indirect 블록까지 명시적으로 해제

파일을 완전히 지우려면 55의 `ext2_get_or_alloc_block`이 만들었을 수 있는 direct/single/double/triple indirect 블록을 전부 순회하며 반환해야 한다. 55가 alloc 쪽에서 재귀 대신 "direct/single/double/triple을 명시적으로 나눠 적는" 스타일(`ext2_get_or_alloc_block`)을 택했던 것과 같은 이유로, 해제 쪽도 재귀 대신 `ext2_free_indirect_block`(single)/`ext2_free_double_indirect`/`ext2_free_triple_indirect` 세 함수로 명시적으로 나눠 적었다 — 진짜 재귀를 썼다면 각 깊이마다 블록 하나(최대 4096바이트)를 읽어들이는 정적 버퍼가 재귀 호출 중에도 값이 유지돼야 하는데, 함수를 깊이별로 분리하면 각 함수가 자신만의(서로 다른 심볼의) 정적 버퍼를 가지므로 이 문제가 자연히 사라진다. `ext2_free_inode_blocks`가 이 셋과 direct 블록 12개를 모아 inode 하나의 데이터를 전부 반환한다.

이 디스크(8MB)에서는 double/triple indirect 해제 경로도 55와 마찬가지로 실제로는 도달·검증되지 않는다(direct/single 범위 안에서만 테스트가 이뤄진다) — 55의 같은 한계가 여기도 그대로 이어진다.

## mkdir

`ext2_mkdir(path)`는 55의 `ext2_create_file`과 거의 같은 뼈대(`ext2_split_parent`로 부모/이름 분리 → 부모 디렉토리 확인 → `ext2_alloc_inode` → `ext2_dir_insert`로 부모에 엔트리 추가)를 따르되 세 가지가 다르다:

1. **이름 중복 체크**: `ext2_create_file`은 별도 존재 확인이 없다(`ext2_open`이 `ext2_resolve_path` 실패 시에만 호출하므로 호출부가 이미 보장한다). `ext2_mkdir`는 syscall에서 직접 호출되는 진입점이라 그 보장이 없으므로, `ext2_scan_dir`로 부모 안에 같은 이름이 이미 있는지 직접 확인하고 있으면 실패한다(Linux `mkdir(2)`가 `EEXIST`로 실패하는 것과 같은 의미론).
2. **`i_mode`/`i_links_count`**: `S_IFDIR|0755`(새 상수 `EXT2_DEFAULT_DMODE`, 55의 `EXT2_DEFAULT_FMODE=0644`와 같은 스코프 결정 — mode 인자를 받는 인터페이스가 없어 고정값), `i_links_count=2`(자기 자신의 `.` 엔트리 + 부모의 dirent 하나).
3. **첫 데이터 블록에 `.`/`..` 채우기**: `ext2_get_or_alloc_block(fs, &new_inode, 0, ...)`으로 블록 하나를 확보한 뒤 `.`(자기 inode 번호)와 `..`(부모 inode 번호) 두 dirent를 직접 써넣는다 — `.`은 4바이트 정렬된 최소 크기(12바이트), `..`은 나머지 전부(`block_size - 12`)를 차지해 블록을 꽉 채운다. 55의 `ext2_dir_insert`가 새 블록을 만들 때 "엔트리 하나로 꽉 찬 자유 공간"을 초기 상태로 만들던 것과 같은 불변식을, 여기서는 "엔트리 두 개(`.`/`..`)로 꽉 찬 상태"로 만드는 셈이다.

부모의 `i_links_count`도 하나 늘려야 한다(새 서브디렉토리의 `..`가 부모를 가리키므로) — `ext2_dir_insert` 호출 전에 `parent_inode.i_links_count++`을 해두면, `ext2_dir_insert`가 성공 경로 어느 쪽을 타든 항상 마지막에 `ext2_write_inode(fs, dir_inode_num, dir_inode)`로 부모를 디스크에 flush하므로 별도 처리 없이 함께 반영된다.

새 디렉토리 하나가 생겼으므로 그룹 디스크립터의 `bg_used_dirs_count`도 증가시키고 `ext2_flush_gd`로 반영한다 — `e2fsck`가 이 값을 실제 디렉토리 개수와 대조하므로(아래 "검증" 참고), 여기서 빠뜨리면 바로 걸린다.

## unlink

`ext2_unlink(path)`는 부모 디렉토리를 찾아 그 안에서 이름이 일치하는 dirent를 지우고, 대상 inode의 `i_links_count`를 감소시켜 0이 되면 블록/inode를 반환한다.

**대상이 디렉토리면 거부한다** — `(target_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR`이면 그냥 실패를 반환한다. 실제 Linux도 `unlink(2)`를 디렉토리에 쓰면 `EISDIR`로 거부하고 `rmdir(2)`/`unlinkat(AT_REMOVEDIR)`만 디렉토리를 지울 수 있다 — 이 프로젝트는 `rmdir`을 구현하지 않으므로(로드맵 문구 자체가 "mkdir/unlink"만 명시) 지금은 디렉토리를 지울 방법이 없다는 뜻이고, 이는 의도된 스코프 밖이다(아래 "다음 단계 힌트" 참고).

**dirent 제거는 병합 방식**: 삭제할 dirent가 블록 안에서 첫 엔트리가 아니면 바로 앞 엔트리의 `rec_len`에 흡수시킨다(`prev->rec_len += de->rec_len`) — 이후 이 블록을 rec_len 기반으로 순회하는 모든 코드(`ext2_find_in_dir_block`, `ext2_getdents`, 55의 `ext2_insert_dirent_in_block`)가 지워진 엔트리의 바이트 자체를 건드리지 않고 그냥 건너뛰게 된다. 삭제할 dirent가 블록의 첫 엔트리면(병합할 이전 엔트리가 없으면) `inode=0`으로만 표시하고 `rec_len`은 그대로 둔다 — 이건 55의 `ext2_insert_dirent_in_block`이 이미 "삭제된 엔트리"로 인식하도록 짜여 있는 바로 그 형태다(`avail = de->rec_len - (de->inode ? used(de) : 0)`, `inode==0`이면 전체 `rec_len`이 빈 공간). 즉 삭제 쪽 구현이 55가 이미 정해둔 온디스크 표현을 그대로 재사용한 것이지 새로 발명한 게 아니다.

**freed inode는 완전히 0으로 지운 뒤 디스크에 써야 한다** — 처음 구현에서는 `ext2_free_inode`(비트맵 비트 해제 + free count 증가)만 호출하고 inode 구조체 자체(`i_mode`/`i_links_count` 등)는 그대로 뒀는데, `e2fsck -f -n`으로 검증하다가 바로 걸렸다:
```
Pass 4: Checking reference counts
Unattached zero-length inode 23.  Clear? no
Pass 5: Checking group summary information
Inode bitmap differences:  +23
```
원인은 `ext2_free_inode`가 비트맵만 지우고 inode 테이블의 실제 내용(mode=0100644, links_count=1로 남아있는 "겉보기엔 멀쩡한 파일")은 그대로 남겨뒀기 때문이다 — e2fsck의 Pass 1은 inode 테이블을 직접 훑어 mode가 유효하고 links_count>0인 inode를 "사용 중"으로 판단하는데, 그 inode를 가리키는 디렉토리 엔트리는 이미 지워졌으니 Pass 3(디렉토리 연결성 검사)에서 그 inode에 도달하지 못해 "unattached"로 잡히고, Pass 5에서는 e2fsck 자신의 판단(사용 중)과 우리가 지워둔 비트맵(free)이 어긋나 불일치로 잡힌 것이다. 고친 내용은: 블록을 다 반환한 뒤 inode 구조체 전체를 0으로 채워 `ext2_write_inode`로 먼저 디스크에 써서 "mode=0, links=0"인 명백히 빈 inode로 만들고, 그다음에 비트맵을 지운다(순서: 블록 해제 → inode 내용 0으로 flush → inode 비트맵 해제) — 이렇게 하면 e2fsck 관점에서 mode=0인 inode는 애초에 "사용 중" 후보에서 제외되므로 비트맵과 어긋날 일이 없다.

이 버그는 커널 내부 검증(getdents-absent 등)만으로는 전혀 드러나지 않았다 — getdents는 부모 디렉토리의 dirent만 보므로 "이름이 사라졌다"는 것까지만 확인하고, inode 테이블 자체의 잔여 상태는 55처럼 e2fsck 같은 외부 도구로만 드러난다(55의 CLAUDE.md가 "커널의 getdents/read 자체가 우리가 만든 것이므로... 자기 자신이 자기 자신을 채점하는 꼴"이라고 지적한 바로 그 이유가 이번에도 실제로 버그를 잡아낸 사례다).

## 검증: syscall 직접 호출 + busybox + e2fsck

55와 같은 3단 검증(커널 내부 syscall → 실제 busybox 바이너리 → 커널 밖 e2fsck)을 그대로 따른다.

1. **`check_mkdir("/disk/newdir")`** + `check_getdents`로 부모에 나타나는지 확인, 그 안에 `check_write_create("/disk/newdir/inner.txt", ...)`로 파일을 만들어 새로 생성된 디렉토리 안에서 경로 탐색·쓰기가 정상 동작하는지 확인(55의 `ext2_resolve_path`가 새 디렉토리도 문제없이 타고 들어가는지 검증).
2. **`check_write_create` → `check_unlink` → `check_getdents_absent`**로 만든 파일을 지우고 목록에서 사라졌는지 확인.
3. **`busybox mkdir /disk/bbdir`** + `check_getdents`로 실제 바이너리가 만든 디렉토리를 확인.
4. **`busybox rm /disk/touched.txt`**로 55에서 `busybox touch`가 만들어둔 바로 그 파일을 지운다 — 생성(55)과 삭제(56)가 같은 파일을 놓고 왕복하는 셈이라 별도 파일을 새로 안 만들어도 된다. `check_getdents_absent`로 사라졌는지 확인.
5. **`make run-nogui` 뒤 `e2fsck -f -n build/disk.img`**: 위에서 서술한 orphan inode 버그를 이 검증이 실제로 잡아냈고, 수정 후에는 Pass 1~5 전부 에러 없이 통과하며 `25/2048 files ..., 614/8192 blocks`로 보고한다. `debugfs -R "stat /newdir"`/`"stat /bbdir"`로 각각 `Links: 2`(자기 자신 + 부모 dirent, 서브디렉토리 없음), `debugfs -R "stat /"`로 루트가 `Links: 6`(`.`+`..` 2 + `lost+found`/`sub`/`newdir`/`bbdir` 4개 서브디렉토리의 `..`)임을 확인해 `i_links_count` 관리가 정확함을 이중 확인했다.

## 이전 단계와의 전제조건 확인

- **55의 읽기/쓰기 경로 회귀 없음**: 55의 모든 검증 코드(`check_indirect_probe`, `check_dual_open`, `check_write_extend` 등)를 그대로 뒀고 전부 통과한다.
- **비트맵 alloc/free 왕복이 실제로 공간을 재사용한다**: `tounlink.txt`를 만들고 지운 뒤 `busybox mkdir /disk/bbdir`이 그 자리(inode 26, `tounlink.txt`가 쓰던 바로 그 번호)를 재사용한 것을 디버깅 과정에서 직접 확인했다 — `ext2_alloc_inode`의 first-fit 비트맵 스캔이 해제된 낮은 번호를 다시 찾아내는 것까지 실제로 동작한다는 뜻이다.
- **`vfs_ops_t`에 필드 추가가 기존 백엔드들(initrd/console/pipe)을 깨뜨리지 않음**: positional initializer의 부분 초기화 규칙 덕분에 컴파일 경고만 발생하고 동작은 그대로다(위 "vfs_ops_t에 mkdir/unlink 추가" 참고) — 55가 `O_CREAT` 추가 때 "vtable을 넓히면 모든 구현체가 즉시 드러난다"고 언급한 강제성과 달리, 이번엔 트레일링 필드라 강제되지 않는다는 차이가 있다(빠뜨려도 컴파일은 통과하고 경고만 뜬다) — 다만 이번 구현 대상(mkdir/unlink)이 애초에 initrd/console/pipe에는 의미가 없는 연산이라 이 차이가 실질적 위험으로 이어지지는 않는다.
- **`ext2_dir_insert`/`ext2_insert_dirent_in_block`(55)와 이번 단계의 삭제 로직 간 온디스크 표현 일관성**: 위 "unlink" 절에서 서술했듯 삭제가 만드는 두 가지 빈 슬롯 형태(첫 엔트리는 `inode=0` 유지, 중간/끝 엔트리는 이전 것에 병합)가 정확히 55의 삽입 로직이 "빈 공간"으로 인식하는 형태와 같다 — 인터페이스 구현 일관성이 아니라 "삽입이 기대하는 온디스크 불변식"과 "삭제가 만드는 온디스크 결과물" 사이의 일관성이라는 점에서 55의 "쓰기가 만든 구조와 읽기가 기대하는 구조" 확인과 같은 종류다.

## 완료 기준

`make run-nogui`에서 `busybox touch` 관련 로그 다음, `$` 프롬프트 직전에 다음이 보이면 성공이다:
```
shell: ext2 mkdir /disk/newdir: OK
shell: ext2 getdents /disk/ newdir: found
shell: ext2 write-create /disk/newdir/inner.txt: OK
shell: ext2 getdents /disk/newdir inner.txt: found
shell: ext2 write-create /disk/tounlink.txt: OK
shell: ext2 getdents /disk/ tounlink.txt: found
shell: ext2 unlink /disk/tounlink.txt: OK
shell: ext2 getdents-absent /disk/ tounlink.txt: OK
shell: busybox mkdir /disk/bbdir:
process 1 exited: code=0
shell: ext2 getdents /disk/ bbdir: found
shell: busybox rm /disk/touched.txt:
process 1 exited: code=0
shell: ext2 getdents-absent /disk/ touched.txt: OK
shell: busybox ls /disk:
...
bbdir
...
newdir
...
process 1 exited: code=0
$
```
(`touched.txt`는 더 이상 목록에 없어야 한다.) 그리고 `build/disk.img`에 대해 `e2fsck -f -n`이 에러 없이 통과해야 한다(커널 밖 검증) — `debugfs`로 `/newdir`, `/bbdir`의 `Links: 2`, 루트의 `Links: 6`도 함께 확인한다.

## 이전 단계(55) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/vfs.h` | 수정 | `vfs_ops_t`에 `mkdir`/`unlink` 함수 포인터 추가; `vfs_mkdir`/`vfs_unlink` 선언 추가 |
| `boot/vfs.c` | 수정 | `vfs_mkdir`/`vfs_unlink` 신규 — `vfs_open`과 같은 마운트 prefix 매칭 폴백 루프 |
| `boot/ext2.h` | 수정 | `ext2_mkdir`/`ext2_unlink` 선언 추가 |
| `boot/ext2.c` | 수정 | `EXT2_DEFAULT_DMODE` 상수 추가; `ext2_bitmap_free`/`ext2_free_block`/`ext2_free_inode`(비트맵 해제 계열), `ext2_free_indirect_block`/`ext2_free_double_indirect`/`ext2_free_triple_indirect`/`ext2_free_inode_blocks`(indirect 포함 전체 블록 해제), `ext2_mkdir`(디렉토리 inode + `.`/`..` + 부모 `i_links_count`/`bg_used_dirs_count` 갱신), `ext2_unlink`(dirent 제거 + 병합 + inode 0 클리어 후 free) 신규 |
| `boot/kernel.c` | 수정 | `ext2_ops`에 `ext2_mkdir`/`ext2_unlink` 연결(`initrd_ops`는 트레일링 필드 자동 0-채움이라 수정 불필요) |
| `boot/syscall.h` | 수정 | `SYS_MKDIR = 83`, `SYS_UNLINK = 87` 추가(Linux x86_64 ABI 번호) |
| `boot/syscall.c` | 수정 | `sys_mkdir`/`sys_unlink`(각각 `vfs_mkdir`/`vfs_unlink` 래핑) 신규 + 디스패치 케이스 추가 |
| `user/init.c` | 수정 | `sys_mkdir`/`sys_unlink`(raw syscall wrapper) 추가; `check_getdents_absent`/`check_mkdir`/`check_unlink` 검증 헬퍼 추가; `/disk/newdir`(+ 내부 파일)/`/disk/tounlink.txt`(생성 후 삭제)/`busybox mkdir /disk/bbdir`/`busybox rm /disk/touched.txt` 검증 블록을 셸 프롬프트 진입 전에 추가 |
| 나머지 전부 | 변경 없음 | 55의 파일 그대로 |

## 다음 단계 힌트

- **`rmdir` 없음**: `unlink`는 디렉토리를 거부만 하고, 빈 디렉토리를 지우는 방법이 없다 — 로드맵에 별도 번호가 없으므로 필요해지면 새 단계로 분리하거나 `unlink`를 확장한다(`.`/`..` 두 엔트리만 있는지 확인 → 부모 `i_links_count` 감소 → `bg_used_dirs_count` 감소까지 필요해 unlink보다 처리할 게 많다).
- **부모 디렉토리 캐시 coherency는 여전히 미해결**: 55의 다음 단계 힌트가 지적한 문제(부모 디렉토리가 다른 fd로 이미 열려 캐시돼 있으면 mkdir/dir_insert가 그 캐시를 갱신하지 않음)가 mkdir/unlink에도 동일하게 남아있다 — 이번 단계 검증은 매번 새로 resolve한 부모 inode만 사용해 이 문제를 피해가지, 고치지는 않았다.
- **unlink된 파일을 다른 fd가 이미 열고 있는 경우(delete-on-last-close) 처리 안 함**: 리눅스는 unlink 후에도 이미 열린 fd로는 계속 읽고 쓸 수 있고, 마지막 fd가 닫힐 때 비로소 블록/inode를 반환한다. 이 구현은 그런 구분 없이 `i_links_count`가 0이 되는 즉시 블록/inode를 반환한다 — 만약 그 순간 다른 fd가 같은 inode를 `ext2_ofile_t` 캐시로 참조하고 있다면(예: 열어둔 채로 자기 자신을 unlink) 그 fd의 이후 읽기/쓰기가 이미 반환되어 재할당될 수 있는 블록을 가리키는 use-after-free에 준하는 상황이 된다. 지금 있는 어떤 테스트도 "열어둔 파일을 그 자리에서 unlink"하는 시나리오를 다루지 않으므로 검증되지 않은 채 남아있다.
- **`bg_used_dirs_count`는 mkdir에서만 관리된다**: `rmdir`이 생기기 전까지는 감소시킬 필요가 없어 그대로 뒀다.
