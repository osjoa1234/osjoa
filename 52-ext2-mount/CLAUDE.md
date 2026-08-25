# 52 — ext2-mount

**목표**: ext2 파일시스템을 읽기 전용으로 마운트한다. 슈퍼블록과 블록 그룹 디스크립터를 파싱하고, 루트 디렉토리(inode 2)의 엔트리를 나열해 커널 로그로 검증한다. FS 파싱 결과를 기존 VFS(`vfs_ops_t`)에 연결하는 일은 `53-vfs-ext2-read`로 미룬다 — 이번 단계는 온디스크 구조를 올바르게 읽어내는 것 자체가 목표다.

**51에서 이어짐**: `51-ata-pio`가 `ata_read_sector`/`ata_write_sector`로 섹터 하나를 정확히 읽고 쓸 수 있음을 확인했다. `52`는 그 위에 처음으로 "섹터들의 나열"을 ext2가 정의한 온디스크 구조(슈퍼블록 → 블록 그룹 디스크립터 → inode 테이블 → 디렉토리 엔트리)로 해석하는 계층을 얹는다. `disk.img`는 이제 빈 8MB가 아니라 `mkfs.ext2`로 실제 포맷된 이미지다 — 51에서 임의로 LBA 5에 쓰던 테스트 패턴은 이 포맷된 레이아웃과 겹쳐 깨질 수 있으므로 제거했고(`kernel.c`), ATA 드라이버 자체(`ata.c`/`ata.h`)는 51에서 그대로 가져와 손대지 않았다.

## disk.img가 이제 실제 ext2 이미지다

`Makefile`의 `$(DISKIMG)` 규칙이 바뀌었다: 8MB `dd` 이미지를 만든 뒤 곧바로 `mkfs.ext2 -d rootfs`로 포맷한다. `-d rootfs`는 e2fsprogs가 지원하는 옵션으로, 호스트 쪽 `rootfs/` 디렉토리(`hello.txt`, `README`)를 이미지의 루트 디렉토리에 그대로 넣어준다 — 커널이 나열할 대상이 빈 디렉토리가 아니라 실제 파일 2개(+`mkfs`가 항상 만드는 `lost+found`)가 되게 하기 위함이다.

`mkfs.ext2` 옵션 선택 근거:

| 옵션 | 값 | 이유 |
|------|-----|------|
| `-b 1024` | 블록 크기 1024바이트 | 슈퍼블록이 바이트 오프셋 1024(=LBA 2)에 바로 오도록. 블록 크기가 크면(기본 4096) 슈퍼블록이 블록 0 안에 묻히지만 부팅 초반 학습에는 "블록 1 = 슈퍼블록"이 더 직관적 |
| `-I 128` | inode 128바이트(rev0 레이아웃) | 256바이트 inode의 확장 필드(`i_extra_isize` 등)를 다룰 필요 없이 고전적인 12개 direct 블록 포인터 구조만 다루기 위함 |
| `-O ...^resize_inode,^dir_index,^ext_attr,...` | 여러 rev1 기능 비활성화 | `resize_inode`(온라인 리사이즈용 예약 GDT 블록), `dir_index`(HTree 해시 디렉토리), `ext_attr`(확장 속성 블록) 모두 이번 단계가 파싱하지 않는 부가 구조라 꺼서 디스크 레이아웃을 최소 형태로 유지 |
| `filetype` (유지) | 디렉토리 엔트리에 파일 타입 바이트 포함 | 이 기능이 꺼지면 `name_len`이 16비트로 확장되어 엔트리 구조 해석이 달라진다 — 켜둔 채로 8비트 `name_len` + 8비트 `file_type` 고정 레이아웃을 가정 |

8MB 이미지 + 1024바이트 블록이면 `blocks_per_group`(8192) 하나로 전체 블록(8192)을 커버해 블록 그룹이 정확히 1개다 — 그룹이 여러 개로 나뉘는 경우(더 큰 디스크)는 스코프 밖이다.

## ext2 온디스크 구조 (이번 단계가 읽는 범위)

**슈퍼블록** (`EXT2_SB_LBA=2`, 즉 바이트 오프셋 1024, 1024바이트 = 섹터 2개):

| 필드 | 오프셋(슈퍼블록 기준) | 의미 |
|------|------|------|
| `s_blocks_count` | 0x04 | 전체 블록 수 |
| `s_first_data_block` | 0x14 | 블록 크기 1024면 1(블록 0=부트블록), 그 이상이면 0 |
| `s_log_block_size` | 0x18 | 블록 크기 = `1024 << s_log_block_size` |
| `s_blocks_per_group` | 0x20 | 그룹당 블록 수 → `ceil(blocks_count / blocks_per_group)`로 그룹 개수 계산 |
| `s_inodes_per_group` | 0x28 | inode 테이블 인덱싱에 사용 |
| `s_magic` | 0x38 | `0xEF53` — 이 값이 아니면 마운트 중단 |
| `s_inode_size` | 0x58 | rev1에서만 유효, 이번 이미지는 128 |

**블록 그룹 디스크립터 테이블**: 슈퍼블록 바로 다음 블록(`s_first_data_block + 1`)에서 시작한다. 그룹당 32바이트 엔트리 하나(`bg_block_bitmap`, `bg_inode_bitmap`, `bg_inode_table`, `bg_free_*_count`). 이번 이미지는 그룹이 1개라 이 테이블도 32바이트뿐이지만, 코드는 그룹 개수만큼 순회하지 않고 그룹 0만 읽는다 — 여러 그룹을 순회하는 일반화는 이번 스코프가 아니다(8MB 디스크에서는 애초에 필요 없다).

**inode 조회**: `inode_num`에서 인덱스 `(inode_num-1) % inodes_per_group`를 구하고, `bg_inode_table` 블록부터 `index * inode_size` 바이트만큼 떨어진 위치를 읽는다. 루트 디렉토리는 항상 inode 2(`EXT2_ROOT_INO`)다. inode 구조체 중 이번 단계가 쓰는 필드는 `i_mode`, `i_size`, `i_block[0..11]`(direct 블록 포인터 12개)뿐이다 — indirect/double-indirect 블록 포인터(`i_block[12..14]`)는 읽지 않는다(아래 "다음 단계 힌트" 참고).

**디렉토리 엔트리** (`ext2_dirent_t`, 8바이트 헤더 + 가변 길이 이름): `inode`(4) + `rec_len`(2) + `name_len`(1) + `file_type`(1) + `name[name_len]`. `rec_len`만큼 다음 엔트리로 건너뛰면서 블록 끝까지 순회한다. 이름은 널 종료가 아니므로 `console_printf("%s", ...)`에 넘기기 전에 로컬 버퍼에 복사하고 널을 붙인다.

## 검증: 루트 디렉토리 나열만, VFS 연결 없음

`kernel_main`이 `ata_init()` 직후(51의 raw sector dump 자리를 대체) `ext2_mount()` 하나만 호출한다. 이 함수는:

1. 슈퍼블록을 읽고 매직 넘버를 확인한다.
2. 블록 그룹 디스크립터(그룹 0)를 읽는다.
3. 루트 inode(2)를 읽는다.
4. 루트 inode의 direct 블록들(`i_size`만큼)을 순회하며 디렉토리 엔트리를 로그로 찍는다.

전 과정이 읽기 전용이다 — `ata_write_sector`는 이번 단계 코드에서 한 번도 호출되지 않는다(`make run`을 여러 번 돌려도 `disk.img`가 바뀌지 않음을 `md5sum`으로 확인함). `vfs_mount`는 여전히 `initrd_ops`(`/`)만 등록하고, ext2는 VFS 테이블에 전혀 나타나지 않는다.

## 명령

```bash
make            # build/os.iso, build/disk.img(mkfs.ext2로 포맷, rootfs/ 내용 포함) 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean      # build/ 전체 삭제 (disk.img도 함께 삭제됨 — 다음 make에서 rootfs/로부터 재생성)
```

## 완료 기준

`make run-nogui`에서 `ata: primary master ready` 다음, `initramfs` 로그 이전에 다음이 보이면 성공이다(이후 `$` 프롬프트까지는 51과 동일):

```
ata: primary master ready (0x1F0-0x1F7, ctrl=0x3F6)
ext2: superblock magic=0xEF53 rev=1 block_size=1024 blocks=8192 inodes=2048 groups=1
ext2: group 0: inode_table=5 block_bitmap=3 inode_bitmap=4 free_blocks=7916 free_inodes=2035
ext2: root inode=2 mode=0x41ED size=1024 blocks=2
ext2: root dir: inode=2 type=DIR name=.
ext2: root dir: inode=2 type=DIR name=..
ext2: root dir: inode=11 type=DIR name=lost+found
ext2: root dir: inode=12 type=REG name=README
ext2: root dir: inode=13 type=REG name=hello.txt
ext2: mount OK
initramfs: 13 file(s) found
```

## 이전 단계(51) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/ext2.h` | 신규 | `ext2_mount()` 선언 |
| `boot/ext2.c` | 신규 | 슈퍼블록/그룹 디스크립터/inode/디렉토리 엔트리 구조체(packed) + `ext2_read_block`(블록→섹터 변환) + `ext2_read_inode` + `ext2_print_dir_block` + `ext2_mount` |
| `boot/kernel.c` | 수정 | `ext2.h` include; `ata_init()` 이후의 LBA 0 read dump + LBA 5 write/read 검증 블록을 `ext2_mount()` 호출로 교체 |
| `rootfs/hello.txt`, `rootfs/README` | 신규 | `mkfs.ext2 -d`가 이미지 루트에 넣을 테스트 파일 |
| `Makefile` | 수정 | `EXT2OBJ` 컴파일·링크 반영; `$(DISKIMG)` 규칙이 `dd` 뒤에 `mkfs.ext2 -d $(ROOTFSDIR)` 실행하도록 변경 |
| `CLAUDE.md` | 신규 | 이 문서 |
| `boot/ata.c`, `boot/ata.h` | 변경 없음 | 51의 드라이버를 그대로 사용 |

## 다음 단계 힌트

- `53-vfs-ext2-read`가 지금 확정한 `ext2_mount()`(및 그 안의 슈퍼블록/그룹/inode 파싱 로직)를 재사용 가능한 형태로 쪼개서 `vfs_ops_t.open`/`read`/`close`에 연결한다 — 지금은 `ext2_mount()`가 로그만 찍고 끝나지만, 다음 단계에서는 inode 번호로 파일을 찾아 열고 데이터 블록을 읽는 경로가 필요하다.
- **indirect 블록 포인터(`i_block[12..14]`)는 이번 단계가 다루지 않는다** — 루트 디렉토리(1024바이트, direct 블록 1개)와 `lost+found`(12288바이트, direct 블록 12개로 정확히 커버됨)가 우연히 모두 direct 블록만으로 표현되는 크기라 필요가 없었다. `53`에서 12개 direct 블록을 넘는 파일(단일 indirect 블록, 블록 크기 1024면 파일 크기 약 12KB+256KB까지)을 다루게 되면 그때 확장할 지점이다.
- 블록 그룹이 1개보다 많은 경우(더 큰 디스크 이미지)는 여전히 스코프 밖이다 — `ext2_mount()`는 그룹 0만 읽는다.
- `s_feature_incompat`/`s_feature_ro_compat` 비트를 읽고는 있지만 실제로 검사하지는 않는다 — 예를 들어 이미지가 `filetype` 기능 없이 포맷되면 디렉토리 엔트리 해석이 조용히 틀어진다. 지금은 `Makefile`이 항상 같은 옵션으로 `mkfs.ext2`를 돌리므로 문제가 없지만, 다른 소스의 ext2 이미지를 마운트하게 되면 이 필드들을 실제로 검사해야 한다.
