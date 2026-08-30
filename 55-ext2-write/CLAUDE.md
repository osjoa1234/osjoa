# 55 — ext2-write

**목표**: ext2에 실제로 쓰기 지원을 추가해 파일을 새로 만들고(생성) 늘릴(확장) 수 있게 한다 — 블록/inode 비트맵 할당기, direct 블록 포인터 갱신을 통한 파일 확장, 디렉토리 엔트리 추가. `>` 리다이렉션의 전제조건이지만 셸에 `>` 파싱을 넣는 것 자체는 이번 스코프 밖이다(로드맵에 별도 단계가 없다 — 필요해지면 그때 셸 파서에 추가).

**54에서 이어짐**: 54까지는 `ata_write_sector`가 정의만 있고 아무도 호출하지 않는 "읽기 전용 불변식"이 유지됐다(54의 "이전 단계와의 전제조건 확인" 절 참고). 이번 단계가 그 불변식을 처음으로 깬다 — 정확히는 깨는 것 자체가 이번 단계의 목표다.

## `O_CREAT`을 열기 인터페이스로 흘려보내기

`sys_open`은 `open(2)`의 3-인자를 다 받아왔지만(raw syscall wrapper가 이미 `rsi=flags`, `rdx=mode`를 넘기고 있었다) 커널 쪽 `sys_open(const char *path)`은 그 값을 버렸다. "생성"이 필요해지기 전까지는 아무도 인지 못했던 반쪽짜리 구현이었다.

`vfs_ops_t.open`의 시그니처를 path 하나에서 `(path, flags)` 두 개로 넓혔다:
```c
int (*open)(const char *path, u32 flags);
```
이 변경은 실제로 열기 로직을 갖는 두 백엔드(initrd, ext2)에만 의미가 있다 — `console_ops`/`pipe_*_ops`는 `vfs_mount`에 등록되지 않아 `vfs_open`을 거치지 않는다(`console_dev_open()`/`pipe_create()`가 직접 `vfs_file_t`를 만든다). 그래도 struct 리터럴의 함수 포인터 타입은 맞춰야 해서 `con_open`도 `flags` 파라미터를 추가하되 무시한다. `initrd_open`은 다른 곳(`proc_exec`)에서 옛 시그니처로 직접 호출되므로 그 시그니처는 그대로 두고, `kernel.c`에 `initrd_vfs_close`와 같은 자리에 `initrd_vfs_open(path, flags)` 어댑터를 하나 더 추가해 `initrd_open(path)`로 위임했다.

`O_CREAT`(0x40)은 `vfs.h`에 새로 정의한다:

| 비트 | 이름 | 의미 |
|---|---|---|
| `0x40` | `O_CREAT` | 없으면 새로 만든다(생성 시 mode는 0644 고정 — 아래 "스코프 결정" 참고) |

`ext2_open`이 `ext2_resolve_path` 실패(파일 없음) + `flags & O_CREAT`일 때만 `ext2_create_file`을 호출한다 — 있는 파일을 `O_CREAT`로 열면 그냥 기존 로직대로 연다(`O_EXCL`/`O_TRUNC`는 구현하지 않음, "다음 단계 힌트" 참고). `initrd_open`은 flags를 무시하므로 initrd 위에서 `O_CREAT`는 조용히 실패한다(파일이 없으면 못 연다) — initrd는 read-only 파일시스템이라는 설계가 이미 정해져 있으므로 이건 회귀가 아니라 그 설계의 자연스러운 결과다(인터페이스 구현 일관성 확인: 리눅스에서도 read-only로 마운트된 fs에 `O_CREAT`로 열면 `EROFS`로 실패하는 것과 같은 종류의 차이 — 우리 쪽엔 errno 구분이 없어 그냥 `-1`이라는 건 53/54의 다음 단계 힌트에서 이미 지적된 채로 남아있는 한계라 이번에 새로 고치지 않는다).

## 블록/inode 비트맵 할당기

ext2의 블록 그룹 디스크립터(51에서 이미 파싱)가 두 비트맵의 위치를 갖고 있다: `bg_block_bitmap`, `bg_inode_bitmap`. 각각 블록 하나 크기이고, 비트 i가 1이면 "그 블록/inode가 사용 중"이다.

| 비트맵 | 비트 i → 실제 번호 |
|---|---|
| 블록 비트맵 | `phys_block = sb.s_first_data_block + i` |
| inode 비트맵 | `inode_num = i + 1`(inode는 1-based) |

`ext2_bitmap_alloc(fs, bitmap_block, max_bits)`가 공통 로직이다 — 비트맵 블록을 읽고 0인 첫 비트를 찾아 1로 세팅한 뒤 다시 써넣고, "1-based 비트 인덱스"(`i+1`)를 반환한다. inode 쪽은 이 반환값이 그대로 `inode_num`이라 우연히 편리하고, 블록 쪽은 `s_first_data_block`을 더해야 한다 — `ext2_alloc_block`/`ext2_alloc_inode` 두 함수가 같은 `ext2_bitmap_alloc`을 감싸면서 이 차이만 처리한다.

새 블록/inode를 할당할 때마다 그룹 디스크립터(`bg_free_blocks_count`/`bg_free_inodes_count`)와 슈퍼블록(`s_free_blocks_count`/`s_free_inodes_count`)의 free count를 갱신하고 즉시 디스크에 flush한다(`ext2_flush_gd`/`ext2_flush_sb`) — 51/52가 슈퍼블록·그룹 디스크립터를 부팅 시 한 번만 읽고 끝이었던 것과 달리, 이제 그 두 구조체는 "쓸 때마다 디스크와 동기화해야 하는 상태"가 됐다. 새로 할당된 블록은 `ext2_zero_block`으로 즉시 0으로 채운다 — mkfs가 만든 디스크 이미지는 데이터 영역을 미리 지우지 않으므로 그 자리에 임의의 잔여 바이트가 남아있을 수 있는데, 특히 디렉토리 블록에서 이 값이 우연히 유효한 `rec_len`처럼 보이면 다음 단계(디렉토리 엔트리 삽입)가 쓰레기를 진짜 엔트리로 오인할 수 있다.

이 커널은 여전히 블록 그룹이 하나라고 가정한다(51부터 있던 가정 — `ext2_init`이 그룹 디스크립터를 하나만 읽는다) — 그래서 `max_bits`로 `s_blocks_per_group`/`s_inodes_per_group`을 그대로 쓴다. 8MB/1024바이트 블록 이미지는 정확히 블록 그룹 하나(`blocks_per_group=8192`, `blocks_count=8192`)라 이 가정이 실제로 맞다.

## direct 블록 포인터 갱신으로 파일 확장

`ext2_get_or_alloc_block(fs, inode, logical, out_phys)`이 53의 `ext2_resolve_block`(읽기 전용, indirect까지 지원)과 짝을 이루는 쓰기 버전이다 — 하지만 **direct 블록(0~11)만** 지원한다. `logical >= EXT2_DIRECT_BLOCKS`면 그냥 실패를 반환한다. indirect 블록에 새 포인터를 채워 넣으려면 indirect 블록 자체도(없으면) 새로 할당해야 하는데, 이는 "포인터 갱신"이 아니라 "포인터의 포인터 갱신"이라 성격이 다른 확장이다 — 한 단계 한 개념 원칙에 따라 이번엔 로드맵 문구 그대로 "direct 블록 포인터 갱신"까지만 하고 indirect 확장은 다음 기회로 미룬다("다음 단계 힌트" 참고).

기존 direct 포인터가 이미 있으면(`inode->i_block[logical] != 0`) 그 블록을 그대로 재사용하고, 없으면 `ext2_alloc_block`으로 새로 받아 `inode->i_block[logical]`에 박아 넣고 `inode->i_blocks`(512바이트 섹터 단위 카운트)를 `block_size/512`만큼 늘린다. 이 함수는 파일 데이터 확장과 디렉토리 확장(아래) 양쪽에서 재사용된다 — 리눅스도 디렉토리가 결국 "특별한 내용을 담은 일반 파일"이라 블록 할당 메커니즘 자체는 공유하는 것과 같은 이유다.

`ext2_write`는 이 함수로 블록을 확보한 뒤 항상 **읽고-수정하고-쓰는(read-modify-write)** 방식으로 블록 하나를 처리한다 — 새로 할당된 블록도 이미 `ext2_zero_block`으로 0 채움이 끝난 상태라 "읽으면 0이 나온다"가 보장되므로, 기존 파일의 부분 덮어쓰기든 새 파일의 첫 쓰기든 같은 코드 경로를 탄다. 루프가 끝나면 `pos+total > inode->i_size`일 때만 `i_size`를 늘리고(기존 파일 중간을 덮어쓰는 경우 크기가 줄어들면 안 되므로), inode를 즉시 디스크에 flush한다.

## 디렉토리 엔트리 삽입

54의 `ext2_getdents`가 순회하던 바로 그 온디스크 포맷(`ext2_dirent_t` + 이름)에 엔트리 하나를 끼워 넣어야 한다. 실제 ext2는 디렉토리 블록 안의 엔트리를 빈틈없이 채우지 않는다 — 마지막 엔트리의 `rec_len`이 "실제로 필요한 크기"보다 크게 잡혀 블록 끝까지 채워지는 방식으로 여유 공간을 표현한다(삭제된 엔트리도 `inode=0`으로 표시하고 앞/뒤 `rec_len`에 흡수시키는 방식으로 같은 여유 공간을 만든다).

```
사용된 크기(used) = (8 + name_len + 3) & ~3   // 4바이트 정렬, ext2 온디스크 표준
여유 공간(avail)  = de->rec_len - (de->inode ? used(de) : 0)
```

`ext2_insert_dirent_in_block`이 블록 하나 안에서 `avail >= needed`인 엔트리를 찾으면:
- 그 엔트리가 이미 쓰이고 있었다면(`de->inode != 0`) `rec_len`을 `used`만큼 줄이고, 그 뒤에 새 엔트리를 `rec_len=(원래 rec_len - used)`로 만든다(분할).
- 이미 비어있는 슬롯이었다면(`de->inode == 0`, 삭제된 엔트리 자리) `rec_len`을 그대로 두고 그 슬롯 전체를 새 엔트리로 채운다(분할하지 않음 — 아래 "파일 생성" 절의 스코프 결정과 같은 이유로, 여유 공간을 더 잘게 쪼개는 최적화는 이번 스코프 밖).

`ext2_dir_insert`가 디렉토리의 기존 블록들을 순서대로 이 함수에 넣어보고, 전부 실패하면(여유 공간이 있는 블록이 하나도 없으면) `ext2_get_or_alloc_block`으로 새 블록을 하나 더 받아 그 블록 전체를 `inode=0, rec_len=block_size`인 "빈 엔트리 하나"로 초기화한 뒤 거기에 넣는다 — 새로 생기는 디렉토리 블록의 초기 상태 자체가 "엔트리 하나로 꽉 찬 자유 공간"이라는 ext2 표준 불변식과 일치한다. 성공하면 두 경우 모두 부모 디렉토리 inode(`i_size`가 늘었을 수도 있음)를 디스크에 flush한다.

## 파일 생성

`ext2_create_file(fs, path)`는 경로를 "부모 디렉토리 경로"와 "마지막 이름"으로 쪼갠 뒤(`ext2_split_parent` — 마지막 `/`를 찾아 자르는 문자열 조작, `/a/b`→(`/a`,`b`), `b`→(`/`,`b`)), 부모를 `ext2_resolve_path`(이미 있던 53의 함수, read-only 탐색)로 찾고, 새 inode를 `ext2_alloc_inode`로 받아 최소한의 필드(`i_mode=S_IFREG|0644`, `i_links_count=1`, 나머지 전부 0)로 초기화해 디스크에 쓴 뒤, `ext2_dir_insert`로 부모 디렉토리에 엔트리를 추가한다.

### 스코프 결정: 생성된 파일은 항상 0644

`vfs_ops_t.open`은 `(path, flags)`만 받고 mode(`open(path,flags,mode)`의 그 세 번째 인자)는 받지 않는다 — `sys_open`이 `frame->rdx`(mode)를 아예 읽지 않고 버린다. `open(2)` 세 번째 인자까지 백엔드에 흘려보내려면 `vfs_ops_t.open`을 세 번째 파라미터까지 늘려야 하는데, 그 mode를 실제로 쓰는 백엔드가 지금 ext2 하나뿐이라 인터페이스 전체를 넓히는 건 이번 스코프(블록/inode 할당기, direct 확장, 디렉토리 엔트리)에 비해 과하다고 판단해 미뤘다 — 생성되는 모든 파일은 `initrd_mode`의 고정값(`S_IFREG|0644`)과 동일한 0644로 고정한다. 필요해지면(예: 셸이 실행 파일을 생성해야 하는 상황이 생기면) 그때 `vfs_ops_t.open`의 시그니처를 다시 넓힌다.

## 검증: syscall 직접 호출 + 커널 밖 e2fsprogs로 이중 확인

53/54까지의 패턴대로 `user/init.c`가 셸 진입 전에 자동 검증한다:

1. **`check_write_create`**: `O_CREAT`로 새 파일을 만들고 짧은 문자열을 쓴 뒤 fd를 닫고 **다시 연**(새 ofile 슬롯, 디스크에서 inode를 다시 읽음) fd로 읽어 내용이 일치하는지 확인한다 — close/reopen을 거치는 이유는 54의 다음 단계 힌트가 지적한 "flush 안 된 상태에서 새로 open하면 옛날 값이 보일 수 있다"는 coherency 문제가 이번 단계의 write-through(모든 쓰기가 즉시 `ext2_write_inode`/`ext2_flush_gd`/`ext2_flush_sb`로 디스크에 반영됨) 설계로 실제로 해소됐는지를 바로 이 지점에서 검증하기 때문이다.
2. **`check_write_extend`**: 2500바이트(block_size=1024 기준 3개 블록, direct 한도 12개 안)를 써서 블록 할당기가 여러 블록에 걸쳐 정상 동작하는지 패턴 바이트(`i%256`)로 확인한다.
3. **각 쓰기 뒤 `check_getdents`**로 방금 만든 파일이 디렉토리 목록에 실제로 나타나는지 확인한다 — 디렉토리 엔트리 삽입이 getdents가 읽는 바로 그 포맷과 어긋나지 않았는지를 같은 코드(54의 getdents)로 교차 검증하는 셈이다.
4. **`/disk/sub/child.txt`**로 하위 디렉토리에 대한 생성(부모 경로 탐색이 루트가 아닌 경우)도 같이 확인한다.
5. **`busybox ls /disk`**가 `written.txt`/`multiwrite.txt`를 목록에 실제로 출력한다 — 커널 내부 검증과 별개로 진짜 바이너리가 우리가 쓴 디스크 상태를 정상적인 ext2로 인식한다는 뜻이다.

여기에 더해 이번 단계는 **커널 밖에서 e2fsprogs로 결과물을 검증**했다 — 커널의 getdents/read 자체가 우리가 만든 것이므로, 쓰기가 정말 올바른지는 그 둘만으로는 "자기 자신이 자기 자신을 채점"하는 꼴이 된다. `make run-nogui`가 끝난 뒤 `build/disk.img`를 신뢰할 수 있는 외부 구현으로 확인했다:
```
$ debugfs -R "ls -l /" build/disk.img            # written.txt(17B)/multiwrite.txt(2500B) inode/크기 확인
$ debugfs -R "cat /written.txt" build/disk.img   # 내용 확인 → "hello ext2 write"
$ debugfs -R "stat /multiwrite.txt" build/disk.img   # BLOCKS:(0-2):607-609, Blockcount:6 (3블록×2섹터)
$ e2fsck -f -n build/disk.img                    # Pass 1~5 전부 에러 없이 통과
```
`e2fsck`가 "22/2048 files ..., 611/8192 blocks"로 무결하다고 판단했다는 게 중요하다 — 비트맵 카운트, 디렉토리 구조, 참조 카운트, 그룹 요약 정보(우리가 직접 갱신한 free count들)가 실제 ext2 스펙과 어긋남 없이 전부 맞다는 뜻이다.

## 이전 단계와의 전제조건 확인

- **읽기 경로 회귀 없음**: `/disk/hello.txt`(19B)/`multiblock.txt`(2600B)/`singleindirect.txt`(20480B)/`doubleindirect.txt`(307200B) 전부 이번 단계 이전과 같은 크기·내용으로 여전히 통과한다(53/54의 검증 코드를 그대로 뒀다) — 쓰기 경로 추가가 기존 읽기 전용 캐시(`ext2_ofile_t`, inode identity 공유)의 동작을 건드리지 않았다.
- **getdents가 쓰기 결과와 어긋나지 않음**: 54의 `ext2_getdents`는 온디스크 dirent 포맷을 그대로 순회하는데, 이번 단계가 만든 새 엔트리도 같은 포맷(4바이트 정렬 `rec_len`, `file_type` 등)이라 별도 수정 없이 바로 새 파일들을 나열했다 — "인터페이스 구현 일관성" 확인이 이번엔 `vfs_ops_t` 백엔드 간이 아니라 "쓰기가 만든 온디스크 구조"와 "읽기가 기대하는 온디스크 구조" 사이에서 이뤄진 셈이다.
- **`sys_stat`/`sys_fstat`의 `st_size`가 쓰기 직후에도 최신**: `check_write_extend`가 쓰기 직후 open→read로 다시 읽어 크기까지 검증하므로(`vfs_size` → `ext2_size` → 캐시된 `inode.i_size`, 쓰기 시점에 이미 갱신됨) 54가 남겨둔 "쓰기가 생기면 `st_size`가 최신이어야 한다"는 힌트가 충족됐다.
- **`O_CREAT`가 `vfs_ops_t.open` 시그니처를 넓히면서 initrd/console 양쪽 다 컴파일 타임에 강제로 갱신됨**: 두 백엔드 다 flags를 무시하도록 명시적으로 고쳤다(빠뜨리면 함수 포인터 타입이 안 맞아 컴파일이 실패한다) — 이 강제성 자체가 "vtable을 넓히면 모든 구현체가 즉시 드러난다"는, 새 필드를 옵셔널 NULL로 추가했던 54의 getdents/mode와는 다른 종류의 안전성이다.

## 완료 기준

`make run-nogui`에서 `ata: primary master ready` 다음, `$` 프롬프트 직전에 다음이 보이면 성공이다:
```
shell: ext2 getdents /disk/sub nested.txt: found
shell: ext2 write-create /disk/written.txt: OK
shell: ext2 getdents /disk/ written.txt: found
shell: ext2 write-extend /disk/multiwrite.txt: OK
shell: ext2 getdents /disk/ multiwrite.txt: found
shell: ext2 write-create /disk/sub/child.txt: OK
shell: ext2 getdents /disk/sub child.txt: found
shell: busybox ls /disk:
...
multiwrite.txt
...
written.txt
process 1 exited: code=0
$
```
그리고 `build/disk.img`에 대해 `e2fsck -f -n`이 에러 없이 통과해야 한다(커널 밖 검증).

## 이전 단계(54) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/vfs.h` | 수정 | `O_CREAT` 정의 추가; `vfs_ops_t.open`/`vfs_open` 시그니처에 `flags` 추가 |
| `boot/vfs.c` | 수정 | `vfs_open`이 flags를 받아 백엔드 `open(rest, flags)`로 전달 |
| `boot/ext2.h` | 수정 | `ext2_open` 시그니처에 `flags` 추가, `ext2_write` 선언 추가 |
| `boot/ext2.c` | 수정 | `ext2_write_block`/`ext2_zero_block`/`ext2_write_inode`/`ext2_flush_sb`/`ext2_flush_gd`(디스크 flush 계열), `ext2_bitmap_alloc`/`ext2_alloc_block`/`ext2_alloc_inode`(비트맵 할당기), `ext2_get_or_alloc_block`(direct 블록 확보/확장), `ext2_dirent_used`/`ext2_insert_dirent_in_block`/`ext2_dir_insert`(디렉토리 엔트리 추가), `ext2_split_parent`/`ext2_create_file`(파일 생성), `ext2_write`(쓰기 syscall 백엔드) 신규; `ext2_open`이 `flags`를 받아 `O_CREAT` 처리 |
| `boot/console_dev.c` | 수정 | `con_open`이 `flags` 파라미터를 추가로 받되 무시(vtable 시그니처 일치용) |
| `boot/kernel.c` | 수정 | `initrd_vfs_open` 어댑터 추가(새 시그니처로 `initrd_open` 위임); `ext2_ops`에 `ext2_write` 연결 |
| `boot/syscall.c` | 수정 | `sys_open`이 그동안 버려지던 `flags`(`frame->rsi`)를 받아 `vfs_open`에 전달; `sys_access`/`sys_stat`의 `vfs_open` 호출에 `flags=0` 추가 |
| `Makefile` | 수정 | `EXT2OBJ` 의존성에 `boot/vfs.h` 추가(ext2.c가 이제 vfs.h의 `O_CREAT`를 참조) |
| `user/init.c` | 수정 | `sys_creat`(`O_CREAT` raw syscall wrapper) 추가; `check_write_create`/`check_write_extend`(생성+쓰기+재오픈 읽기 검증) 추가해 셸 프롬프트 진입 전 자동 실행 |
| `boot/initrd.c`, `boot/initrd.h`, `boot/pipe.c`, `boot/syscall.h`, `rootfs/*`, `boot/ata.c`, `boot/ata.h`, `tools/ext2_testgen.py` | 변경 없음 | 54의 파일 그대로(`ata_write_sector`는 51부터 이미 구현돼 있었고 이번 단계에서 처음 호출되기 시작했을 뿐, 정의 자체는 그대로) |

## 다음 단계 힌트

- **`56-mkdir-unlink`**: 이번 단계의 `ext2_alloc_inode`/`ext2_dir_insert`는 `mkdir`(디렉토리 inode + `.`/`..` 엔트리 + 부모 `i_links_count` 증가)에도 거의 그대로 재사용할 수 있다 — 다만 디렉토리 inode는 `i_mode=S_IFDIR`이고 첫 블록에 `.`/`..` 두 엔트리를 미리 채워야 한다는 차이가 있다. `unlink`는 반대 방향(디렉토리 엔트리를 `inode=0`으로 표시 + 앞 엔트리 `rec_len`에 흡수, inode의 `i_links_count` 감소, 0이 되면 블록/inode 비트맵 해제)이라 이번 단계가 만든 할당기의 "해제" 짝이 없다는 게 빠진 부분이다 — 지금은 alloc만 있고 free가 없다.
- **indirect 블록 확장은 아직 없다**: `ext2_get_or_alloc_block`이 `logical >= EXT2_DIRECT_BLOCKS`면 그냥 실패해 `ext2_write`가 그 지점에서 조용히 부분 쓰기로 끝난다(반환값이 요청한 len보다 작아짐 — 유닉스 `write(2)`의 정상적인 동작이라 에러는 아니지만, 큰 파일을 새로 만들 때 12블록(block_size=1024면 12KB) 이상을 한 번에 쓰려고 하면 그 이후는 잘린다). single/double/triple indirect의 "필요하면 새 indirect 블록 자체도 할당" 로직은 이번 스코프 밖이다.
- **`O_TRUNC`/`O_EXCL` 미구현**: `O_CREAT`만 해석하고 `O_EXCL`(있으면 실패)/`O_TRUNC`(있으면 0으로 자르기)은 무시한다 — 기존 파일을 `O_CREAT`로 열면 항상 "있는 그대로 열기"가 된다. `>` 리다이렉션을 셸에 실제로 붙이려면 `O_TRUNC` 정도는 필요해질 가능성이 높다.
- **생성된 파일은 항상 mode 0644**: `vfs_ops_t.open`이 flags까지만 받고 mode(`open`의 3번째 인자)는 안 받아서다(위 "스코프 결정" 참고) — 실행 파일을 ext2에 직접 만들어야 하는 상황이 오면 그때 인터페이스를 넓힌다.
- **부모 디렉토리 캐시 coherency 미해결**: 생성/삽입 시 부모 디렉토리 inode를 매번 디스크에서 새로 읽어(`ext2_resolve_path`) 쓰는데, 만약 그 부모 디렉토리가 이미 다른 fd로 열려 `ext2_ofile_t` 캐시에 들어 있다면 그 캐시된 사본은 갱신되지 않는다(같은 세션에서 디렉토리를 열어둔 채로 그 안에 파일을 새로 만드는 시나리오는 아직 검증 안 됨) — 54의 다음 단계 힌트가 지적했던 "identity 공유 캐시의 coherency" 문제가 파일 자체(같은 inode를 여러 fd로 열기)에는 이번 단계의 write-through로 해소됐지만, 디렉토리(부모 inode)에 대해서는 여전히 남아있다.
- **타임스탬프 여전히 0**: 새로 만든 inode도 `i_atime`/`i_ctime`/`i_mtime`을 전부 0으로 남긴다 — 54의 다음 단계 힌트와 같은 이유(시간 개념 부재)로 그대로 넘어간다.
