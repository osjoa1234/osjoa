# 42 — mmap

**목표**: 익명 `mmap2(192)`을 구현하고 `mprotect(125)`/`fstat(108)`을 stub으로 채워, musl mallocng가 큰 청크를 얻어올 때 쓰는 mmap 기반 경로의 전제조건을 만든다.

**41에서 이어짐**: 41은 ELF가 끝나는 지점 위쪽으로 `brk`가 온디맨드 페이지를 매핑하는 길을 냈다. musl mallocng는 작은 할당은 `brk`로 키운 힙에서 채우지만, 일정 크기 이상의 청크나 슬랩은 `mmap`으로 별도 매핑을 받아온다. 이번 단계는 `brk`와 겹치지 않는 별도 주소 구간에서 익명 페이지를 내주는 `mmap2`를 추가한다. `mprotect`/`fstat`은 mallocng와 musl 초기화 경로가 호출은 하되(권한 변경, stdio 디스크립터 판별) 이 단계에서 완전히 구현할 필요는 없어 성공만 돌려주는 stub으로 둔다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### mmap_next — 스택 바로 아래에서 아래쪽으로 자라는 별도 커서

`heap_start`/`heap_end`는 ELF 끝에서 위로 자라는 반면, mmap 영역은 그 반대편 끝 — 스택 바로 아래(`PROC_MMAP_TOP = PROC_USTACK_TOP - 0x1000`, 스택 한 페이지를 뺀 지점)에서 아래로 자란다. `process_t`에 `mmap_next`(u64) 하나를 추가해 "다음 mmap을 배치할 상한"으로 쓴다. 매 `mmap` 호출마다 요청 길이를 페이지 경계로 올림한 만큼 `mmap_next`를 그대로 낮추고, `[새 mmap_next, 이전 mmap_next)` 구간에 `page_alloc` + `paging_map_user_page`로 즉시(온디맨드가 아니라 eager) 페이지를 채운다 — `proc_brk`의 매핑 루프와 동일한 패턴(할당 후 0으로 지우고 매핑)을 그대로 재사용했다. `brk`처럼 언맵/재사용 로직은 없다 — 축소나 해제 요청(`munmap`)은 이번 단계 범위 밖이라 `mmap_next`는 프로세스 생애주기 동안 단조 감소만 한다.

### 왜 스택 아래인가 — brk 힙과 물리적으로 분리된 주소 구간

`heap_end`는 ELF 로드 후 위치(대략 0x301000~)에서 시작해 위로 자라고, `mmap_next`는 0x3FF000에서 시작해 아래로 자란다. 두 커서가 서로 다른 방향에서 자라 마주치기 전까지는 별도 관리 없이 겹치지 않는다 — 실제 Linux의 힙/mmap 영역 배치와 같은 모양이다. 프로세스 주소공간이 4MB(`PROC_USTACK_TOP`)로 작기 때문에 두 영역이 실제로 부딪힐 수 있지만, 이번 단계 검증에는 문제되지 않아 충돌 감지는 다루지 않는다.

### 익명 매핑만 지원 — MAP_ANONYMOUS 없으면 실패

`vfs_ops_t`를 통한 파일 기반 매핑은 아직 없으므로 `flags`에 `MAP_ANONYMOUS`(0x20)가 없으면 곧바로 `(u64)-1`(MAP_FAILED)을 돌려준다. `addr`(첫 인자, 보통 0)과 `fd`/`pgoffset`은 아예 읽지 않는다 — 항상 커널이 주소를 고르는 경로만 지원하므로 `MAP_FIXED` 같은 고정 주소 요청은 무시하고 커서 기반 주소를 돌려준다. `prot`도 저장하지 않는다 — 매핑된 페이지는 항상 유저 RW(`PTE_US|PTE_RW`)이고, 읽기 전용 요청이라도 실제 권한 축소는 하지 않는다.

### mprotect/fstat — 이번 단계는 성공만 보장하는 stub

`mprotect`는 인자를 검증도 사용도 하지 않고 항상 `0`(성공)을 돌려준다 — 페이지 권한을 실제로 좁히는 기능은 없다. `fstat`은 `process_t.fds[fd]`가 열려 있는지만 확인하고, `struct stat` 자리에 커널이 정의한 최소 필드(`st_dev/st_ino/st_mode/st_nlink/st_size/st_blksize`)만 0으로 채운 뒤 `st_mode = S_IFCHR|0666`, `st_nlink = 1`, `st_blksize = 512`를 넣어 반환한다. **이 `stat_t` 레이아웃은 이 커널만의 자체 정의이며 실제 Linux ABI의 `struct stat`과 무관하다** — 44(`musl-hello`)에서 실제 musl 바이너리를 붙일 때 musl이 기대하는 필드 순서/크기로 다시 맞춰야 할 수 있다.

### user/mmap.c — mmap 주소 출력, 전체 쓰기로 매핑 검증, mprotect/fstat 결과 출력

`user/brk.c`와 같은 구조: `sys_mmap2(0, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1)`로 8KB(2페이지)를 매핑받아 주소를 16진수로 출력하고, 반환된 영역 전체에 값을 채워써서 두 페이지 모두 실제로 매핑됐는지 검증한다(매핑이 안 됐다면 `interrupt 0x0E`로 죽어 `process N exited`가 안 찍힌다). 이어서 `sys_mprotect`/`sys_fstat(1, ...)`를 호출해 반환값과 `st_mode`를 출력한다. `init` 셸에서 `mmap`이라고 입력하면 실행된다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 41과 동일하게 `$` 프롬프트에서 멈춘다(`initramfs: 7 file(s) found`로 파일 수만 6→7). GUI(`make run`)에서 `mmap`을 입력하면:

```
$ mmap
mmap: addr = 0x00000000003FD000
mmap: mprotect rc = 0x0000000000000000
mmap: fstat rc = 0x0000000000000000
mmap: fstat st_mode = 0x00000000000021B6
process 1 exited: code=0
$
```

`brk`/`hello`/`hello2`도 그대로 동작해야 한다(회귀 없음).

## 이전 단계(41) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/process.h` | 수정 | `PROC_MMAP_TOP`/`MAP_ANONYMOUS` 매크로, `process_t.mmap_next` 필드, `proc_mmap` 선언 추가 |
| `boot/process.c` | 수정 | `proc_spawn`/`proc_exec`가 `mmap_next`를 `PROC_MMAP_TOP`으로 초기화, `proc_fork`가 부모 `mmap_next` 복사, `proc_mmap` 구현(eager 페이지 매핑 + 커서 하강) |
| `boot/syscall.h` | 수정 | `SYS_FSTAT=108`, `SYS_MPROTECT=125`, `SYS_MMAP2=192` 추가 |
| `boot/syscall.c` | 수정 | `sys_mprotect`(항상 성공 stub), `stat_t`/`sys_fstat`(fd 유효성만 확인 후 최소 필드 채움) 추가, `SYS_FSTAT`/`SYS_MPROTECT`/`SYS_MMAP2` 디스패치 케이스 추가 |
| `user/mmap.c` | 신규 | `sys_mmap2`/`sys_mprotect`/`sys_fstat`로 매핑 주소·반환값·`st_mode`를 출력하고 8KB 전체에 조용히 쓰기 — 검증용 유저 프로그램 |
| `Makefile` | 수정 | `user/mmap.c` 빌드/링크, initrd에 `mmap` 포함 |
| `initrd/.gitignore` | 수정 | 빌드 산출물 `brk`/`mmap` 패턴 추가(41에서 `brk`가 gitignore 누락으로 커밋된 것은 이번 단계에서 함께 손대지 않음) |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `43-tls`: `arch_prctl(172)` `ARCH_SET_FS`로 FS.base MSR을 설정하고 `getpid`/`getuid`/`uname` stub을 추가한다. `mprotect`/`fstat`과 마찬가지로 처음엔 "크래시 없이 그럴듯한 값을 돌려주는" 수준으로 시작해도 된다 — 실제 검증은 44에서 musl 바이너리가 실행될 때 이뤄진다.
- `stat_t` 레이아웃과 `mmap_next`/`heap_end` 충돌(4MB 주소공간에서 두 영역이 마주칠 가능성)은 44에서 실제 문제가 되면 그때 다시 다룬다 — 지금 미리 다루면 검증되지 않은 코드가 된다.
