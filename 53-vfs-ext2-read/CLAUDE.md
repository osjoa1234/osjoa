# 53 — vfs-ext2-read

**목표**: `52-ext2-probe`가 확정한 슈퍼블록/그룹 디스크립터/inode/디렉토리 엔트리 파싱 로직을 재사용 가능한 형태로 쪼개 기존 VFS(`vfs_ops_t`)에 연결한다. `vfs_mount("/disk/", &ext2_ops)`로 ext2가 처음으로 VFS 네임스페이스에 등록되고, 유저 프로세스가 `open`/`read`/`close` 시스템콜로 디스크 위의 파일을 읽을 수 있게 된다.

**52에서 이어짐**: `52`의 `ext2_probe()`는 슈퍼블록을 읽고 루트 디렉토리 엔트리를 로그로 나열하는 단발성 함수 하나였다. `53`은 그 안에 있던 조각(슈퍼블록/그룹 디스크립터 읽기, inode 조회, 디렉토리 엔트리 순회)을 `ext2_init()`(부팅 시 1회, 슈퍼블록·그룹 디스크립터를 정적 상태로 캐싱)과 `ext2_open`/`ext2_read`/`ext2_size`/`ext2_close`(파일 단위 반복 호출)로 나눈다. 이 네 함수의 시그니처는 `boot/initrd.c`의 `initrd_open`/`initrd_read`/`initrd_size`와 정확히 같은 모양이다 — `kernel.c`가 `initrd_ops`를 만들 때처럼 `ext2_ops = { ext2_open, ext2_read, 0, ext2_size, ext2_close, 0 }`를 그대로 조립해 `vfs_mount`에 넘긴다.

## 왜 `/disk/`에 마운트하는가

`initrd_ops`는 이미 `/`에 마운트되어 있다. VFS 마운트 테이블은 접두사(prefix) 문자열 일치로 백엔드를 고르는데(`boot/vfs.c`의 `smatch`), ext2도 `/`에 마운트하면 두 백엔드가 항상 같은 접두사로 경쟁하게 된다. 그래서 ext2는 `/disk/`라는 별도 접두사를 쓴다 — `open("/disk/hello.txt")`는 `"hello.txt"`로 잘려 `ext2_open`에, `open("/hello")`는 `"hello"`로 잘려 `initrd_open`에 전달된다.

**`vfs_open`의 접두사 매칭은 fallthrough라서 사실 `/`만으로도 동작은 했을 것이다**: `smatch(path, "/")`는 `/`로 시작하는 모든 경로에 매치되므로, `initrd_open`이 실패(`bfd < 0`)하면 루프가 다음 마운트 엔트리로 넘어가 `ext2_open`을 시도한다(`boot/vfs.c:42-61`, 이 fallthrough 자체는 32단계부터 있던 기존 동작이라 이번에 건드리지 않았다). 그럼에도 `/disk/`를 쓰는 이유는 이 fallthrough에 암묵적으로 기대는 대신 두 백엔드의 네임스페이스를 명시적으로 분리해, 이름이 initrd와 ext2 양쪽에 동시에 존재할 때(예: 둘 다 `hello.txt`가 있는 경우) 어느 쪽이 열리는지가 마운트 순서라는 우연에 좌우되지 않게 하기 위함이다.

## `ext2.c` 구조 변화

```
52: ext2_probe()  — 슈퍼블록 read → 그룹 desc read → 루트 inode read → 루트 dir 순회+출력, 전부 한 함수
53: ext2_init()   — 슈퍼블록 read → 그룹 desc read, 결과를 static 전역(g_sb/g_gd/g_block_size)에 캐싱
    ext2_open(path)          — 루트 디렉토리에서 이름으로 inode 탐색(ext2_lookup) → open-file 슬롯에 inode 캐싱 → bfd 반환
    ext2_read(bfd,buf,len,pos) — 캐싱된 inode의 i_block[]을 pos/block_size로 인덱싱해 필요한 블록만 읽어 복사
    ext2_size(bfd)            — 캐싱된 inode.i_size 반환 (SEEK_END용)
    ext2_close(bfd)           — 슬롯 반납
```

`ext2_read_block`(블록→섹터 변환), `ext2_read_inode`(inode 번호→테이블 오프셋)는 52와 동일한 로직을 전역 `g_sb`/`g_gd`/`g_block_size`를 참조하도록만 바꿔 재사용했다. 52의 `ext2_print_root_dir_block`(엔트리를 콘솔에 출력)은 `ext2_find_in_dir_block`(이름이 일치하는 엔트리를 찾으면 inode 번호를 반환)으로 바뀌었다 — 순회 구조는 동일하고 "찾으면 출력"이 "찾으면 반환"으로 바뀐 것뿐이다.

### open-file 테이블

```c
#define EXT2_MAX_OPEN 8U

typedef struct {
    int          used;
    ext2_inode_t inode;
} ext2_ofile_t;

static ext2_ofile_t g_ofiles[EXT2_MAX_OPEN];
```

`ext2_open`이 호출될 때마다 inode 전체(128바이트, `i_block[0..11]` 포함)를 슬롯에 복사해둔다. 그래서 `ext2_read`는 매 호출마다 inode를 다시 읽지 않고 `pos`만으로 어느 direct 블록의 어느 오프셋인지 계산한다. 슬롯이 8개뿐이라 동시에 열 수 있는 ext2 파일은 8개까지다(`vfs_dup`으로 fd를 복제해도 `ext2_ops.dup`이 없으므로 백엔드 슬롯은 늘지 않고 그대로 공유된다 — `boot/console_dev.c`의 console 백엔드와 같은 패턴).

### 여러 direct 블록에 걸친 읽기

52는 루트 디렉토리(1블록)와 `lost+found`(정확히 12블록, 사람이 직접 순회) 정도만 다뤘지만, 53의 `ext2_read`는 임의의 `len`/`pos`로 호출될 수 있어야 한다. 그래서 한 번의 `ext2_read` 호출 내부에서 `block_index = (pos+total)/block_size`가 바뀔 때마다 블록을 새로 읽어오는 루프를 돈다:

```c
while (total < len) {
    block_index = (pos + total) / g_block_size;
    block_off   = (pos + total) % g_block_size;
    if (block_index >= 12) break;          /* indirect 블록 미지원, 52와 동일한 한계 */
    phys_block = f->inode.i_block[block_index];
    ...
    chunk = min(g_block_size - block_off, len - total);
    /* block_buf에서 chunk바이트 복사 */
    total += chunk;
}
```

이 경로가 실제로 여러 블록을 정확히 이어붙이는지 검증하려고 `rootfs/multiblock.txt`(2600바이트, `'0'`~`'9'` 반복 패턴, 블록 크기 1024에서 블록 3개에 걸침)를 새로 추가했다. `hello.txt`/`README`는 둘 다 1블록 안에 들어가 이 경로를 전혀 테스트하지 못했다.

## 검증 경로: 커널 직접 호출이 아니라 진짜 syscall

32/34단계(vfs-open, vfs-seek)가 그랬듯, 이번에도 검증은 `kernel_main`이 아니라 `user/init.c`(=셸)가 부팅 직후, 셸 프롬프트 루프에 들어가기 전에 `open`(syscall 2)/`read`(syscall 0)/`close`(syscall 3)를 직접 호출해서 한다 — `sys_open` 래퍼를 이번에 처음 추가했다. `/disk/hello.txt`를 읽어 내용을 그대로 출력하고, `/disk/multiblock.txt`를 한 번에 2600바이트 읽어 각 바이트가 패턴과 일치하는지 유저 공간에서 직접 검사해 `content OK`/`content MISMATCH`를 출력한다. `make run-nogui`가 셸 프롬프트 직전까지 자동으로 출력하므로 이 두 줄은 GUI 없이도 확인된다 — 이 프로젝트에서 파일 시스템 연동을 GUI 상호작용 없이 자동 검증한 첫 사례다.

`kernel.c`는 `ext2_init()` 호출과 `vfs_mount("/disk/", &ext2_ops)` 등록, 로그 출력만 하고 파일을 직접 읽지는 않는다 — 52의 `ext2_probe()` 호출 자리를 대체하되, 실제 읽기 검증 책임은 VFS를 통해 접근하는 유저 프로세스 쪽으로 옮겼다.

## 이전 단계와의 전제조건 확인

- **읽기 전용 불변식 유지**: `ata_write_sector`는 이번 단계 코드에서도 호출되지 않는다(`grep -n ata_write_sector boot/*.c`로 확인 — 정의만 있고 호출부 없음). `make run-nogui`를 두 번 돌려 `disk.img`의 md5sum이 그대로임을 확인했다.
- **initrd 경로 회귀 없음**: `proc_exec`는 여전히 `initrd_open`을 직접 호출하고(`boot/process.c`) VFS를 거치지 않으므로, 실행 파일 탐색(셸의 `busybox`, `pipe` 등 명령 실행)은 이번 변경과 무관하게 그대로 동작한다. VFS 경유 `open`/`read`(파일 내용을 읽는 syscall 경로)만 ext2를 새로 인식한다.
- **`VFS_MOUNT_MAX=4`**: 마운트 테이블에 `/`(initrd), `/disk/`(ext2) 두 엔트리가 들어가 여유가 있다.

## 명령

```bash
make            # build/os.iso, build/disk.img(rootfs/에 multiblock.txt 포함해 재생성) 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean      # build/ 전체 삭제 (disk.img도 함께 삭제됨 — 다음 make에서 rootfs/로부터 재생성)
```

## 완료 기준

`make run-nogui`에서 `ata: primary master ready` 다음, `$` 프롬프트 직전에 다음이 보이면 성공이다:

```
ata: primary master ready (0x1F0-0x1F7, ctrl=0x3F6)
ext2: superblock magic=0xEF53 rev=1 block_size=1024 blocks=8192 inodes=2048
ext2: group 0: inode_table=5 block_bitmap=3 inode_bitmap=4 free_blocks=7913 free_inodes=2034
initramfs: 13 file(s) found
vfs: initrd mounted at /
vfs: ext2 mounted at /disk/
timer: PIT 100Hz IRQ0 ready
keyboard ready: IRQ1 unmasked
processes: init spawned pid=0
shell: linux-abi ready
shell: ext2 /disk/hello.txt: hello ext2 root fs
shell: ext2 /disk/multiblock.txt: content OK
$
```

## 이전 단계(52) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/ext2.h` | 수정 | `ext2_probe()` 선언을 `ext2_init`/`ext2_open`/`ext2_read`/`ext2_size`/`ext2_close`로 교체 |
| `boot/ext2.c` | 수정 | 슈퍼블록/그룹 디스크립터를 static 전역에 캐싱하는 `ext2_init`; 루트 디렉토리에서 이름으로 inode를 찾는 `ext2_lookup`/`ext2_find_in_dir_block`; open-file 슬롯 테이블(`g_ofiles`, `EXT2_MAX_OPEN=8`) 기반 `ext2_open`/`ext2_read`(direct 블록 경계를 넘는 다중 블록 읽기)/`ext2_size`/`ext2_close` |
| `boot/kernel.c` | 수정 | `ext2_probe()` 호출을 `ext2_init()`으로 교체; `ext2_ops` vtable 추가; `vfs_mount("/disk/", &ext2_ops)` 등록 |
| `user/init.c` | 수정 | `sys_open` 래퍼 추가; 셸 루프 진입 전 `/disk/hello.txt` 읽어 출력, `/disk/multiblock.txt` 전체를 읽어 패턴 일치 여부를 유저 공간에서 검사해 `content OK`/`content MISMATCH` 출력 |
| `rootfs/multiblock.txt` | 신규 | 2600바이트, `'0'`~`'9'` 반복 패턴 — 블록 크기 1024에서 direct 블록 3개(0,1,2)에 걸친 읽기 검증용 |
| `Makefile` | 수정 | `ROOTFSFILES`에 `rootfs/multiblock.txt` 추가(변경 시 `disk.img` 재생성 트리거) |
| `rootfs/hello.txt`, `rootfs/README` | 변경 없음 | 52의 파일 그대로, 이제 `ext2_open`으로 실제로 열려 읽힌다 |
| `boot/ata.c`, `boot/ata.h` | 변경 없음 | 51의 드라이버 그대로 |

## 다음 단계 힌트

- `54-getdents`: `ext2_open`이 루트 디렉토리 안의 "파일 이름 하나"만 찾을 수 있고, 디렉토리 자체를 열어 엔트리 목록을 얻는 경로는 아직 없다 — `getdents` syscall과 `ls`가 이 경로를 필요로 한다. 지금 `ext2_lookup`이 이미 디렉토리 엔트리를 순회하는 로직을 갖고 있으니 그걸 노출하는 형태가 될 것이다.
- **서브디렉토리 미지원**: `ext2_lookup`은 루트 inode(2)에서만 이름을 찾는다. `/disk/sub/file.txt`처럼 중첩 경로는 아직 해석하지 못한다(디렉토리 엔트리를 만나도 항상 파일로 취급해 그 inode를 열어버린다 — `i_mode`의 디렉토리 비트를 검사하지 않음). 경로를 `/`로 나눠 각 세그먼트마다 `ext2_lookup`을 반복 호출하는 형태로 확장해야 한다.
- **indirect 블록 포인터(`i_block[12..14]`) 여전히 미지원**: `ext2_read`는 `block_index >= 12`면 그 자리에서 멈춘다. 12KB(블록 크기 1024 기준)를 넘는 파일은 뒷부분이 잘려서 읽힌다.
- 블록 그룹 1개, 그룹 0만 지원하는 한계도 52와 동일하게 유지된다.
- `55-ext2-write`에서 쓰기 경로가 추가되면 지금 `ext2_read`가 참조하는 캐싱된 inode(`g_ofiles[i].inode`)가 파일 크기 변경 등으로 stale해질 수 있다는 점을 염두에 둬야 한다 — 지금은 읽기 전용이라 문제되지 않는다.
