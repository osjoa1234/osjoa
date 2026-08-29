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

### open-file 테이블과 inode identity 공유

```c
#define EXT2_MAX_OPEN 8U

typedef struct {
    int          used;
    u32          inode_num;
    u32          refcount;
    ext2_inode_t inode;
} ext2_ofile_t;

static ext2_ofile_t g_ofiles[EXT2_MAX_OPEN];
```

`ext2_open`이 호출될 때마다 inode 전체(128바이트, `i_block[0..11]` 포함)를 슬롯에 복사해둔다. 그래서 `ext2_read`는 매 호출마다 inode를 다시 읽지 않고 `pos`만으로 어느 direct 블록의 어느 오프셋인지 계산한다.

처음 구현했을 때는 `ext2_open`이 빈 슬롯을 찾을 때 "이미 같은 파일이 열려있는가"를 확인하지 않았다 — 같은 경로를 두 번 열면 `ext2_resolve_path`가 매번 같은 inode 번호를 찾아내는데도 서로 다른 슬롯에 inode 데이터가 통째로 두 벌 복사됐다. `initrd_open`(정적 배열 인덱스라 같은 이름이면 항상 같은 fd를 반환)과 비교하면 같은 `vfs_ops_t` 인터페이스의 두 구현이 identity 의미론에서 어긋나 있었던 것 — 리눅스라면 이 자리는 `(superblock, inode 번호)`로 찾는 icache가 담당해서 어느 백엔드든 같은 동작을 보장한다. 그래서 `ext2_open`을 lookup-or-create로 바꿨다:

```c
for (i = 0; i < EXT2_MAX_OPEN; i++) {
    if (fs->ofiles[i].used && fs->ofiles[i].inode_num == inode_num) {
        fs->ofiles[i].refcount++;
        return (int)i;
    }
}
/* 없으면 기존처럼 빈 슬롯에 새로 읽어옴, refcount = 1 */
```

슬롯을 공유하게 됐으니 `ext2_close`도 즉시 반납 대신 `refcount--` 후 0이 될 때만 `used = 0`으로 바꿨다. 이 리눅스식 lookup-or-create/refcount 패턴은 icache 전체를 새로 만드는 게 아니라 `ext2.c` 내부에 로컬하게 최소한으로 구현한 것이다 — identity 공유가 필요한 백엔드가 지금은 ext2 하나뿐이라 `vfs.c`에 공용 캐시 레이어를 신설하는 건 이 스코프에 과한 일반화다.

이 fix가 진짜로 필요해지는 이유(fd A로 쓴 내용이 fd B에서도 보여야 한다)는 55(ext2-write)에서 쓰기가 생겨야 관찰 가능하다. 하지만 "이 인터페이스의 여러 백엔드가 같은 의미론을 지켜야 한다"는 53 자신의 스코프이므로, 구현은 지금 끝내고 **검증만** `user/init.c`의 `check_dual_open("/disk/hello.txt", ...)`으로 한다 — 같은 파일을 두 fd로 열어 각각 독립된 `pos`로 정상히 읽히는지, 한쪽을 닫아도 남은 쪽이 refcount 덕분에 계속 정상 동작하는지까지 확인한다. "fd A가 쓴 게 fd B에 보이는가"라는 coherency 자체는 여전히 55의 몫으로 남는다.

슬롯이 8개뿐이라 동시에 열려있는 서로 다른 파일은 8개까지다(같은 파일을 여러 번 여는 건 이제 슬롯 하나를 공유하므로 이 한도에 안 걸림). `vfs_dup`으로 fd를 복제해도 `ext2_ops.dup`이 없으므로 백엔드 슬롯은 늘지 않고 그대로 공유된다 — `boot/console_dev.c`의 console 백엔드와 같은 패턴이자, 지금 추가한 refcount와도 결이 같다(다만 `vfs_dup`은 `ext2_open`을 다시 부르지 않으므로 refcount가 증가하지 않는다 — dup된 fd들은 원본과 같은 참조 하나를 공유할 뿐이다).

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

## 중첩 디렉터리 경로 탐색

`ext2_lookup`은 원래 루트 inode(2)의 디렉터리 엔트리만 이름으로 검색했다 — `/disk/sub/file.txt`처럼 경로에 `/`가 더 있으면 그 뒤는 통째로 무시하고 첫 세그먼트만 찾으려다 실패했다. `ext2_read`가 `i_block[]` 12개 이후를 direct 블록처럼 착각하면 안 되는 것과 같은 이유로, `ext2_open`도 경로 세그먼트 사이의 "다음 단계로 내려간다"는 개념 없이 평평한 이름 하나만 찾는 건 `open()`을 절반만 구현한 셈이다 — 그래서 같은 53 스코프(open/read를 실제로 쓸 수 있게 만들기) 안에서 함께 처리했다.

`ext2_lookup`을 두 함수로 나눴다:

- `ext2_scan_dir(dir_inode, name, &out_inode)` — 주어진 디렉터리 inode 하나의 엔트리들 사이에서 이름을 찾는다. 예전엔 이 역할을 하는 코드가 루트 inode의 `i_block[0..11]`만 직접 순회했지만, 지금은 `ext2_resolve_block`을 재사용해 디렉터리가 몇 번째 논리 블록에 있든(이론적으로는 indirect 구간까지도) 옳게 찾는다.
- `ext2_resolve_path(path, &out_inode)` — 경로를 `/`로 나눠 세그먼트마다 `ext2_scan_dir`을 반복 호출한다. 루트(inode 2)에서 시작해서, 마지막 세그먼트가 아닌 단계마다 방금 찾은 inode가 `i_mode & 0xF000 == 0x4000`(디렉터리)인지 확인하고 아니면 실패시킨다 — 파일을 디렉터리처럼 파고들려는 경로(`/disk/hello.txt/x`)를 걸러내기 위함이다. 마지막 세그먼트는 파일이든 디렉터리든 타입을 검사하지 않는다(기존 동작 그대로 유지 — `ext2_open`이 여는 대상의 타입을 애초에 검사하지 않았다).

`ext2_open`은 `ext2_lookup` 대신 `ext2_resolve_path`를 호출하도록 한 줄만 바뀌었다. `rootfs/sub/nested.txt`를 새로 추가해 `/disk/sub/nested.txt`를 여는 경로로 실제 검증한다(아래 완료 기준 참고).

## indirect 블록 포인터(single/double/triple) 지원

`ext2_read`가 처음 구현될 때는 `block_index >= EXT2_DIRECT_BLOCKS(12)`면 그 자리에서 멈췄다 — `i_block[12..14]`는 direct 블록 번호가 아니라 "블록 번호가 들어있는 블록"을 가리키는 간접 포인터인데, 그 사실을 무시하고 인덱스 상한만 늘리면 포인터 블록의 raw 바이트를 파일 데이터로 잘못 반환하게 된다. 그래서 `ext2_resolve_block(inode, logical_block, &phys)`을 새로 추가해 논리 블록 번호 하나를 물리 블록 번호로 바꾸는 계산을 전담시켰다:

| 논리 블록 범위 | 경로 |
|----------------|------|
| `0 ~ 11` | `i_block[0..11]` 직접 |
| `12 ~ 267` | `i_block[12]`(single indirect) → 그 블록의 `entries_per_block`(=`block_size/4`=256)개 포인터 중 하나 |
| `268 ~ 65803` | `i_block[13]`(double indirect) → 1단계 블록 → 2단계 블록 |
| `65804 ~` | `i_block[14]`(triple indirect) → 1단계 → 2단계 → 3단계 블록 |

`ext2_indirect_lookup(block_num, index, &out)`이 공통 하부 함수다 — 포인터 블록 하나를 읽어 `index`번째 `u32`를 반환할 뿐이라 single/double/triple이 이 함수를 1~3번 체이닝하는 것으로 전부 표현된다. `ext2_read`는 이제 `i_block[block_index]`를 직접 읽는 대신 매 블록마다 `ext2_resolve_block`을 호출한다. `ext2_lookup`(루트 디렉토리 탐색)은 손대지 않았다 — 루트 디렉토리가 direct 블록 12개를 넘을 일이 없기 때문이다(다음 단계 힌트 참고).

### triple indirect는 구현하되 이번 단계에서 실제 파일로 검증하지 않는다

single indirect(12KB~268KB)와 double indirect(268KB~64MB 시작)는 지금 8MB `disk.img` 안에서 실제 파일(`rootfs/singleindirect.txt` 20KB, `rootfs/doubleindirect.txt` 300KB)로 자연스럽게 도달한다 — mkfs가 평범하게 할당한 블록을 그대로 읽는다.

`ext2_resolve_block`의 triple indirect 분기(`i_block[14]` 경로)는 single/double과 동일한 `ext2_indirect_lookup` 체이닝으로 구현되어 있어 논리 블록 인덱싱 자체는 완결돼 있다. 다만 논리 블록 65804(파일 오프셋 약 64.3MB) 이상부터 쓰이는 이 경로는 지금 8MB `disk.img`에서는 어떤 실제 파일로도 도달하지 않는다 — ext2 포맷이 블록 그룹 하나의 최대 크기를 `block_size × 8`로 강제하기 때문이다(블록 비트맵이 정확히 블록 1개라서, 1024바이트 블록 기준 상한이 8192블록=8MB). triple indirect를 실제 파일로 태우려면 64MB 넘는 디스크가 필요한데, 그러면 블록 그룹이 여러 개로 쪼개져 `ext2_read_inode`가 그룹 0의 inode 테이블(`g_gd`)만 본다는 전제(52부터 유지된 스코프 한계)가 깨진다 — 거대 파일의 inode가 그룹 0이 아닌 곳에 배치되면 완전히 엉뚱한 inode를 읽게 된다.

멀티 그룹 지원은 이번 단계 스코프가 아니므로, 이 경로는 **구현은 완료하되 실제 데이터로는 검증되지 않은 채로 남긴다** — 하드코딩된 fake inode·합성 블록으로 커널 자체 테스트를 만들어 보강하지 않는다. 그런 테스트는 실제 파일 시스템 경로와 무관한 별도 데이터를 프로덕션 드라이버 파일에 얹는 것이라 코드만 비대해지고 실질적인 신뢰를 주지 않는다. 멀티 그룹을 지원하게 되면(다음 단계 힌트 참고) 그때 실제 대용량 파일로 자연스럽게 검증된다.

## 빌드 타임 테스트 데이터 생성: `tools/ext2_testgen.py`

호스트에서 `python3`로 직접 실행하는 작은 스크립트다(Ubuntu 기본 설치, 이 저장소 툴체인 표에 새로 추가됨). `genfiles <rootfs_dir>` 한 가지 모드만 있다 — `singleindirect.txt`(20480바이트)와 `doubleindirect.txt`(307200바이트)를 `byte[i] = i % 256` 패턴으로 생성한다. `mkfs.ext2 -d` 실행 **전에** 호출해 rootfs에 들어가게 한다. 두 파일 다 `.gitignore` 처리되어 저장소엔 생성 코드만 남는다(300KB짜리 생성 파일을 커밋하지 않기 위함).

`Makefile`의 `$(DISKIMG)` 레시피 안에서 `genfiles` → `mkfs.ext2 -d` 순서로 실행한다. 처음엔 C(`gcc`)로 짰다가, "이렇게 단순한 스크립트에 굳이 커널 툴체인 언어를 맞출 필요가 있냐"는 지적을 받고 파이썬으로 교체했다 — 별도 컴파일 단계 없이 `$(PYTHON3) $(TESTGEN) ...`로 바로 실행된다.

## 검증에 쓰인 오프셋

`user/init.c`의 `check_indirect_probe(path, off)`는 `multiblock.txt`처럼 파일 전체를 읽지 않는다 — `doubleindirect.txt`(300KB)를 유저 공간 정적 버퍼로 통째로 읽는 대신, `sys_lseek`으로 특정 오프셋까지 이동한 뒤 32바이트만 읽어 `byte[i] = i % 256` 패턴과 비교한다. 이번에 처음으로 유저 공간에 `sys_lseek`(syscall 8) 래퍼가 추가됐다.

| 파일 | 오프셋 | 검증하는 경로 |
|------|--------|----------------|
| `singleindirect.txt` | 15460 | 논리 블록 15 → single indirect (`i_block[12]`) |
| `doubleindirect.txt` | 286770 | 논리 블록 280 → double indirect (`i_block[13]`) |

## 명령

```bash
make            # build/os.iso, build/disk.img(rootfs/에 생성된 테스트 파일 포함) 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean      # build/ 전체 삭제 (disk.img도 함께 삭제됨 — 다음 make에서 rootfs/로부터 재생성)
```

## 완료 기준

`make run-nogui`에서 `ata: primary master ready` 다음, `$` 프롬프트 직전에 다음이 보이면 성공이다:

```
ata: primary master ready (0x1F0-0x1F7, ctrl=0x3F6)
ext2: superblock magic=0xEF53 rev=1 block_size=1024 blocks=8192 inodes=2048
ext2: group 0: inode_table=5 block_bitmap=3 inode_bitmap=4 free_blocks=7586 free_inodes=2029
initramfs: 13 file(s) found
vfs: initrd mounted at /
vfs: ext2 mounted at /disk/
timer: PIT 100Hz IRQ0 ready
keyboard ready: IRQ1 unmasked
processes: init spawned pid=0
shell: linux-abi ready
shell: ext2 /disk/hello.txt: hello ext2 root fs
shell: ext2 /disk/multiblock.txt: content OK
shell: ext2 /disk/singleindirect.txt: content OK
shell: ext2 /disk/doubleindirect.txt: content OK
shell: ext2 dual-open /disk/hello.txt: OK
shell: ext2 /disk/sub/nested.txt: hello nested dir
$
```

## 이전 단계(52) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/ext2.h` | 수정 | `ext2_probe()` 선언을 `ext2_init`/`ext2_open`/`ext2_read`/`ext2_size`/`ext2_close`로 교체 |
| `boot/ext2.c` | 수정 | 슈퍼블록/그룹 디스크립터를 static 전역에 캐싱하는 `ext2_init`; open-file 슬롯 테이블(`g_ofiles`, `EXT2_MAX_OPEN=8`) 기반 `ext2_open`/`ext2_read`(direct 블록 경계를 넘는 다중 블록 읽기)/`ext2_size`/`ext2_close`; single/double/triple indirect 해석(`ext2_resolve_block`/`ext2_indirect_lookup`); 중첩 경로 탐색(`ext2_resolve_path`가 세그먼트마다 `ext2_scan_dir` 호출, 예전 `ext2_lookup`의 루트 전용 인라인 순회를 대체 — `ext2_find_in_dir_block`은 그대로 재사용); `ext2_ofile_t`에 `inode_num`/`refcount` 추가해 `ext2_open`이 같은 inode를 lookup-or-create로 슬롯 공유, `ext2_close`는 refcount 기반 반납으로 변경 |
| `user/init.c` | 수정 | `sys_open`/`sys_lseek` 래퍼 추가; 셸 루프 진입 전 `/disk/hello.txt` 읽어 출력, `/disk/multiblock.txt` 전체 읽어 패턴 검사, `/disk/singleindirect.txt`·`/disk/doubleindirect.txt`를 `lseek`+짧은 `read`로 깊은 오프셋 패턴 검사, `/disk/sub/nested.txt`를 열어 중첩 경로 탐색 검증, `check_dual_open`으로 같은 ext2 파일을 두 fd로 열어 identity 공유(독립된 `pos`, 한쪽 close 후 나머지 fd 정상 동작)를 검증 |
| `rootfs/multiblock.txt` | 변경 없음 | 2600바이트, direct 블록 3개 경계 읽기 검증용, 53 최초 커밋에서 그대로 |
| `rootfs/sub/nested.txt` | 신규 | 중첩 디렉터리 경로 탐색(`ext2_resolve_path`) 검증용 — `/disk/sub/nested.txt` |
| `rootfs/singleindirect.txt`, `rootfs/doubleindirect.txt` | 신규(생성됨, 미커밋) | `tools/ext2_testgen.py genfiles`가 빌드 타임에 생성 — single/double indirect 경로가 걸리는 크기(20KB/300KB) |
| `rootfs/.gitignore` | 신규 | 생성된 두 테스트 파일을 저장소에서 제외 |
| `tools/ext2_testgen.py` | 신규 | 호스트 파이썬 스크립트 — single/double indirect 검증용 테스트 파일 생성(`genfiles`) |
| `Makefile` | 수정 | `$(DISKIMG)` 레시피가 `python3 tools/ext2_testgen.py genfiles` → `mkfs.ext2 -d` 순서로 실행하도록 확장 |
| `rootfs/hello.txt`, `rootfs/README` | 변경 없음 | 52의 파일 그대로 |
| `boot/ata.c`, `boot/ata.h` | 변경 없음 | 51의 드라이버 그대로 |

## 다음 단계 힌트

- `54-getdents`: `ext2_open`이 루트 디렉토리 안의 "파일 이름 하나"만 찾을 수 있고, 디렉토리 자체를 열어 엔트리 목록을 얻는 경로는 아직 없다 — `getdents` syscall과 `ls`가 이 경로를 필요로 한다. 지금 `ext2_lookup`이 이미 디렉토리 엔트리를 순회하는 로직을 갖고 있으니 그걸 노출하는 형태가 될 것이다.
- **서브디렉토리 지원 완료**: `ext2_resolve_path`가 `/`로 나눈 세그먼트마다 `ext2_scan_dir`을 반복 호출해 중첩 경로를 해석한다(위 "중첩 디렉터리 경로 탐색" 절 참고). `..`/`.`은 디렉터리 엔트리에 그대로 들어있는 특수 이름이라 별도 처리 없이도 `ext2_scan_dir`이 찾아내지만, 셸이나 `sys_getcwd`가 그걸 실제로 활용하는 경로는 아직 없다.
- **블록 그룹 1개, 그룹 0만 지원**: 52부터 유지된 한계. `ext2_resolve_block`의 triple indirect 분기는 구현되어 있지만 지금 8MB `disk.img`로는 어떤 실제 파일도 그 경로를 밟지 않는다 — 실제 64MB+ 파일로 검증하려면 멀티 그룹(그룹별 inode 테이블 조회)을 먼저 구현해야 한다. 멀티 그룹을 지원하게 되면 그때 대용량 파일로 자연스럽게 검증된다.
- **inode identity는 공유되지만 coherency는 아직 없음**: `ext2_open`이 이제 같은 inode 번호면 슬롯을 공유하므로(위 "open-file 테이블과 inode identity 공유" 참고) 여러 fd가 항상 최신 `i_size`/`i_block[]`을 같이 본다 — 슬롯 자체가 하나니까. 하지만 `55-ext2-write`에서 쓰기가 생기면 얘기가 또 하나 남는다: 쓰기가 인메모리 `g_ofiles[i].inode`만 갱신하고 디스크에 flush를 안 하거나, 혹은 여러 슬롯이 여전히 남아있는 경우(예: 슬롯이 가득 차 같은 파일이 서로 다른 슬롯에 중복 캐싱된 상태) 등 55 자신이 새로 신경 써야 할 쓰기 특유의 coherency 문제가 있다는 점을 염두에 둘 것.
