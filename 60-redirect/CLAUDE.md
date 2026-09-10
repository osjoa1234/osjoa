# 60 — redirect

**목표**: 셸에 `>`(덮어쓰기)/`>>`(이어쓰기) 파일 리다이렉션을 추가한다. `48-pipe`에서 "쓰기 가능한 파일시스템이 없어서" 미뤘던 기능이고, `55-ext2-write`로 ext2 쓰기가, `59-exec-vfs-symlink`로 `PATH` 기반 실행이 갖춰졌으니 이제 막힘 없이 구현할 수 있다.

**48에서 이어짐**: `48-pipe/CLAUDE.md`가 명시적으로 남긴 다음 단계 힌트: "`>` 파일 리다이렉션은 쓰기 가능한 파일시스템이 생겨야 의미가 있다 — ... 그 단계에서 셸의 `split_pipeline`과 비슷한 자리에 `>` 토큰 처리를 추가하면 된다." 정확히 그 자리에 그대로 추가했다.

## `O_TRUNC` — ext2에 없던 마지막 open 플래그

`>`는 파일이 이미 있으면 덮어쓰기(내용을 비우고 다시 씀)여야 한다. 지금까지 ext2의 `O_CREAT`는 "없으면 만들고, 있으면 그냥 열기"만 했다 — 열려는 파일이 이미 존재하면 기존 내용 위에 앞부분만 덮어써지고 뒤쪽 바이트는 그대로 남는다(`ext2_write`가 `i_size`를 줄이지 않고 늘리기만 함). `boot/vfs.h`에 리눅스 x86 `fcntl.h`와 같은 값으로 `O_TRUNC 0x200U`를 추가하고, `boot/ext2.c`의 `ext2_open`에 분기를 하나 더 넣었다:

```c
if (ext2_resolve_path(fs, path, &inode_num) != 0) {
    if (!(flags & O_CREAT)) return -1;
    inode_num = ext2_create_file(fs, path);
    if (inode_num == 0U) return -1;
} else if (flags & O_TRUNC) {
    /* 기존 정규 파일이면 블록을 전부 반환하고 inode를 크기 0으로 되돌린다 */
}
```

파일을 새로 만드는 경우는 이미 크기 0이라 트렁케이트가 필요 없다. 파일이 이미 있는 경우에만 `unlink`가 55~56에서 이미 써둔 `ext2_free_inode_blocks(fs, &inode)`(direct/indirect/double/triple 블록 전부 반환)를 그대로 재사용해 블록을 반환하고, `i_size`/`i_blocks`/`i_block[15]`를 0으로 되돌린 뒤 `ext2_write_inode`로 디스크에 반영한다 — `unlink`가 이미 검증한 블록 반환 경로를 그대로 타므로 새 버그 표면이 거의 없다.

## `O_APPEND` — 파일 끝에서 이어쓰기

`>>`는 기존 내용을 지우지 않고 그 뒤에 이어 써야 한다는 점에서 `O_TRUNC`와 정반대다. 리눅스와 같은 값으로 `boot/vfs.h`/`user/init.c`에 `O_APPEND 0x400U`를 추가하고, `vfs_open`(`boot/vfs.c`)에 한 줄을 더했다:

```c
f->pos = 0U;
if ((flags & O_APPEND) && f->ops->size) f->pos = f->ops->size(bfd);
```

`ext2_open` 자체는 손대지 않았다 — "파일 끝에서 시작"은 ext2만의 사정이 아니라 VFS 레이어의 일반 규칙이라 `vfs_open`에 넣는 게 리눅스 `do_dentry_open`의 `O_APPEND` 처리와 같은 위치다. `f->ops->size`가 없는 백엔드(console, pipe — 둘 다 `size` 훅이 `0`)는 이 분기를 그냥 건너뛴다: 콘솔이나 파이프에 `O_APPEND`를 준다는 것 자체가 의미가 없으므로(둘 다 "현재 위치에 쓰기"가 이미 유일한 동작이다) 자연스럽게 무시된다. `ext2_size`는 55에서부터 있던 기존 훅(`i_size` 반환)을 그대로 재사용한 것이라 ext2 쪽에 새 코드는 없다.

## `>`/`>>` 파싱과 리다이렉션 배선

`user/init.c`의 `split_pipeline`이 `redirect_out[SHELL_STAGE_MAX]` 옆에 `redirect_append[SHELL_STAGE_MAX]` 출력 배열을 하나 더 받는다. `|`와 같은 자리에서 토큰을 훑다가 `>` 또는 `>>`를 만나면 다음 토큰을 그 스테이지의 리다이렉트 대상 파일명으로 기록하고, `redirect_append[nstages]`를 토큰이 `>>`였는지로 채운 뒤 두 토큰(연산자와 파일명) 모두 `stage_argv`에서 제외한다. 스테이지당 리다이렉트는 최대 1개(파이프라인 문법 단순화 — 리눅스 셸도 사실상 마지막 `>`/`>>`만 유효하지만, 여기서는 아예 두 번째 연산자를 문법 오류로 처리한다).

fork 자식 쪽 배선은 파이프 dup2와 정확히 같은 자리, 같은 패턴으로 넣었다 — pipe fd들을 정리한 **다음**, exec 하기 **직전**:

```c
if (redirect_out[i]) {
    long rfd = redirect_append[i] ? sys_creat_append(redirect_out[i]) : sys_creat_trunc(redirect_out[i]);
    if (rfd < 0) { writes("shell: cannot create file\n"); sys_exit(1U); }
    sys_dup2((unsigned int)rfd, 1U);
    sys_close((unsigned int)rfd);
}
```

`sys_creat_append`는 `sys_creat_trunc` 바로 옆에 `O_CREAT | O_APPEND`로 여는 새 래퍼다. 이 순서라면 파이프라인의 마지막 스테이지가 `>`/`>>`까지 갖고 있는 경우(`cmd1 | cmd2 >> file`)에도 파이프 stdout dup2가 먼저 걸린 뒤 리다이렉트가 그 위에 덮어써서 최종적으로 파일로 나간다 — 리눅스 셸의 실제 동작(마지막에 나열된 리다이렉션이 이긴다)과 같은 순서다.

메인 루프의 fork+dup2+exec 배선 전체는 원래 `init_main`의 `for (;;)` 안에 인라인으로 있었는데, 이번에 `run_line(char *buf)` 함수로 뽑아냈다 — 부팅 시퀀스에서 `echo dd > /disk/redir2.txt` 뒤 `echo ee >> /disk/redir2.txt`로 **같은 파일에 트렁케이트 후 어펜드**가 이어지는 상황을 인터랙티브 프롬프트 없이도 고정 문자열(`run_line("echo dd > /disk/redir2.txt")`)로 검증하기 위해서다. 프롬프트 루프는 이제 `run_line(buf)` 한 줄만 부른다 — 동작은 그대로이고 재사용 가능한 형태로 옮긴 것뿐이다.

부팅 시퀀스에 쓰는 고정 검증 코드용으로 `run_argv`(레디렉션 없음) 옆에 `run_argv_redirect(argv, path, append)`를 추가했다 — fork한 자식이 위와 똑같이 `sys_creat_append`/`sys_creat_trunc` + `dup2` + `close` 뒤 `exec_path`한다. `run_argv`가 매번 인터랙티브 프롬프트 없이 고정 `argv`로 명령 하나를 실행하듯, 이건 그 명령의 stdout을 고정 파일로(트렁케이트 또는 어펜드로) 리다이렉트해 실행한다.

## 발견한 버그 — `ext2_ops`에 빠져 있던 `dup` 훅

`sys_creat_trunc` + `dup2` + `close` 순서로 처음 돌렸을 때 커널이 그대로 멈췄다(입력도 출력도 없이 QEMU가 응답하지 않음 — `run-nogui`가 타임아웃으로만 끝남). 원인을 `boot/vfs.c`의 `vfs_dup`에서 찾았다:

```c
vfs_file_t *vfs_dup(vfs_file_t *f)
{
    ...
    if (n->ops->dup) n->ops->dup(n->backend_fd);
    return n;
}
```

`48-pipe`는 파이프가 `fork`/`dup2`를 거치며 여러 `vfs_file_t`로 늘어날 때 "몇 개가 아직 열려 있는지" 세야 해서 `vfs_ops_t`에 `dup` 훅을 추가하고 `pipe_read_dup`/`pipe_write_dup`로 `read_refs`/`write_refs`를 올렸다(`48-pipe/CLAUDE.md` 참고). 그런데 `53-vfs-ext2-read`가 ext2 백엔드를 붙일 때는 `ext2_ops`의 `dup` 자리를 `console_dev`/`initrd`처럼 `0`(훅 없음)으로 남겨뒀다 — ext2는 자기만의 `ofiles[]` 테이블에서 같은 inode를 다시 `open()`하면 `refcount`를 올리는 dedup 로직이 이미 있었으니, 언뜻 dup까지 신경 쓸 필요가 없어 보였다. 하지만 `dup2`는 `ext2_open`을 다시 부르지 않고 **기존 `vfs_file_t`를 복제**하기만 하므로, 그 dedup 로직을 아예 거치지 않는다.

그 결과 시퀀스는 이랬다: `sys_creat_trunc`가 `ofiles[X]`를 `refcount=1`로 연다 → `dup2(rfd,1)`이 `backend_fd=X`를 공유하는 새 `vfs_file_t`를 fd 1에 복제하지만 `ext2_ops.dup==0`이라 `refcount`는 그대로 1 → `close(rfd)`가 `ext2_close(X)`를 불러 `refcount`를 0으로 내리고 `ofiles[X].used=0`으로 슬롯을 반납 — 그런데 fd 1은 여전히 `backend_fd=X`를 참조하는 채로 살아있다. 곧이어 `exec_path`가 실행 파일(busybox)을 열려고 `ext2_open`을 부르면 방금 반납된 슬롯 X를 재사용해 busybox의 inode로 덮어쓰고, `exec_load_binary`가 다 읽은 뒤 다시 `close`해 X를 또 반납한다. 이제 busybox로 교체된 프로세스가 `write(1, ...)`을 하면 커널은 `ofiles[X]`가 `used=0`인 슬롯을 `ext2_write`로 그대로 밀어붙이며 쓰레기 상태의 inode/블록 포인터를 따라가고, 그 안에서 끝나지 않는 루프에 빠져 멈췄다.

고친 방법은 `pipe.c`와 정확히 같은 패턴이다 — `ext2_close` 바로 아래에 `ext2_dup`을 추가해 `refcount`만 올리게 하고, `kernel.c`의 `ext2_ops` 초기화에서 `dup` 자리(0 이었던)를 `ext2_dup`으로 채웠다:

```c
void ext2_dup(int bfd)
{
    ...
    if (bfd < 0 || (u32)bfd >= EXT2_MAX_OPEN || !fs->ofiles[bfd].used) return;
    fs->ofiles[bfd].refcount++;
}
```

이건 이 프로젝트의 "인터페이스 구현 일관성" 원칙이 정확히 겨냥하는 사례다 — `vfs_ops_t`를 구현하는 네 백엔드(console/initrd/pipe/ext2) 중 `dup2`로 실제 참조 카운트가 필요한 백엔드는 pipe뿐인 줄 알았는데, ext2도 "파일을 열고 그 fd를 dup2한 뒤 원본을 닫는" 패턴(정확히 셸 리다이렉션이 하는 일)에서 똑같이 필요했던 것이다. `dup2`가 pipe에만 쓰이는 동안은 드러나지 않다가, 이번 단계가 ext2 fd에 처음으로 `dup2`를 적용하면서 드러났다.

## 검증

1. **부팅 시퀀스 고정 리다이렉션 테스트**: `echo aaaaaaaaaa > /disk/redir.txt` 뒤 `cat /disk/redir.txt`로 `aaaaaaaaaa` 확인 → `echo bb > /disk/redir.txt`(더 짧은 내용으로 덮어쓰기) 뒤 다시 `cat`으로 `bb`만 나오고 이전 내용의 꼬리(`aaaaaaaa`)가 안 남아있는지 확인 — 이게 `O_TRUNC`가 실제로 블록을 반환했다는 증거다(안 그랬으면 `bb\naaaaaaa\n` 같은 뒤섞인 출력이 나왔을 것).
2. **`O_APPEND` 테스트**: 이어서 `echo cc >> /disk/redir.txt` 뒤 `cat`으로 `bb\ncc\n`(트렁케이트 결과 위에 이어 붙었을 뿐 `bb`가 안 지워짐)을 확인한다 — `>>`가 정말 `f->pos`를 파일 끝으로 옮겨 쓰기 시작했다는 증거다. `run_line("echo dd > /disk/redir2.txt")` → `run_line("echo ee >> /disk/redir2.txt")` → `run_line("cat /disk/redir2.txt")`로 인터랙티브 프롬프트와 동일한 `run_line` 경로(파싱부터 fork/dup2/exec까지)로 트렁케이트 다음 어펜드가 이어지는 것도 `dd\nee\n`로 확인한다.
3. **인터랙티브 `>`/`>>` 파싱**: `make run`(GUI) 또는 QEMU 모니터 `sendkey`로 프롬프트에 직접 `echo ... > /disk/....txt`를 타이핑해 `split_pipeline`의 `>`/`>>` 토큰 처리 자체를 검증한다 — `48-pipe`가 `|`를 검증했던 것과 같은 방식이다(`run-nogui`는 키보드 입력이 없어 고정 시퀀스만 확인하고, 타이핑 경로는 자동화 밖이다). 이번 구현 중 QEMU unix-socket 모니터로 `sendkey`를 스크립트로 넣어 `echo interactive redirect > /disk/itest.txt` → `cat /disk/itest.txt`가 `interactive redirect`를 정확히 출력하는 것까지 직접 확인했다.
4. **회귀**: 51~59의 ext2 읽기/쓰기/`getdents`/심링크/PATH exec 검증 전부 이전과 동일하게 통과. `make run-nogui` 뒤 `e2fsck -f -n build/disk.img` 에러 없음.

## 완료 기준

`make clean && make run-nogui`에서 59까지의 모든 출력 뒤에 다음이 이어져야 한다:

```
shell: echo aaaaaaaaaa > /disk/redir.txt:
process 1 exited: code=0
shell: cat /disk/redir.txt:
aaaaaaaaaa
process 1 exited: code=0
shell: echo bb > /disk/redir.txt (O_TRUNC check):
process 1 exited: code=0
shell: cat /disk/redir.txt:
bb
process 1 exited: code=0
shell: echo cc >> /disk/redir.txt (O_APPEND check):
process 1 exited: code=0
shell: cat /disk/redir.txt:
bb
cc
process 1 exited: code=0
shell: echo dd > /disk/redir2.txt:
process 1 exited: code=0
shell: echo ee >> /disk/redir2.txt:
process 1 exited: code=0
shell: cat /disk/redir2.txt:
dd
ee
process 1 exited: code=0
```

`e2fsck -f -n build/disk.img`가 에러 없이 통과해야 한다.

## 발견했지만 이번 단계에서 고치지 않은 버그 — ext2/ATA 동시 접근에 락이 없음

리다이렉션 기능을 회귀 검증하며 파이프라인(`cmd1 | cmd2`)도 같이 눌러봤는데, `cat /disk/hello.txt | cat`처럼 **두 프로세스가 동시에 살아있으면서 각각 ext2 실행 파일을(`/disk/bin/...` 심링크 경유로) 열려는 조합**이 재현 가능하게 걸린다 — 두 번째로 `exec_path`를 타는 프로세스의 `vfs_open`이 존재하는 경로인데도 실패한다(`ext2_resolve_path`가 못 찾음). `59-exec-vfs-symlink`(수정 전 원본)에서도 동일하게 재현되므로 이번 단계가 만든 버그가 아니라, `48-pipe`의 파이프라인 기능과 `51~59`의 ext2 exec 경로가 이번에 처음으로 함께(동시 프로세스 두 개가 진짜로 겹쳐서) 실행되면서 드러난 기존 결함이다.

원인으로 가장 유력한 것은 `ext2.c`의 여러 함수가 스캐치 공간으로 쓰는 `static u8 block_buf[...]`류 지역 정적 버퍼(예: `ext2_read`)와, `ata.c`의 PIO 컨트롤러 자체가 잠금 없이 전역 공유된다는 점이다 — 지금까지의 모든 검증은 "fork 하나 → 완전히 끝날 때까지 `wait` → 다음 fork"로 철저히 직렬이었어서(파이프라인조차 `48`에서는 initrd 바이너리끼리만, ext2 도입 이전에 검증됐다) 두 프로세스가 실제로 동시에 ext2/ATA 코드 경로를 밟는 상황이 이번이 처음이다. 디스크 I/O 경로에 잠금(또는 최소한 요청 직렬화 큐)을 넣는 건 "파일 리다이렉션"과는 분명히 다른 개념이라 `[[one-concept-per-step]]` 원칙상 여기서 고치지 않았다.

**주의**: 이 결함 때문에 지금 상태에서 `cmd1 | cmd2` 형태의 파이프라인에 ext2 기반 실행 파일(`cat`/`ls`/`sh`/`touch`/`mkdir`/`rm`/`echo` — 전부 `busybox` 멀티콜)을 두 개 이상 동시에 쓰면 안정적으로 동작하지 않는다. 단일 명령(파이프 없음)이나 `sh -c "..."`(자식이 하나뿐이므로 동시성 없음)는 영향 없다 — 이번 단계의 리다이렉션 검증도 전부 단일 스테이지라 이 결함의 영향 밖이다.

## 이전 단계(59) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/vfs.h` | 수정 | `O_TRUNC 0x200U`, `O_APPEND 0x400U` 추가 |
| `boot/vfs.c` | 수정 | `vfs_open`이 `O_APPEND` && `ops->size` 있으면 `f->pos`를 파일 끝으로 이동 |
| `boot/ext2.c` | 수정 | `ext2_open`이 기존 정규 파일 + `O_TRUNC`이면 `ext2_free_inode_blocks`로 블록 반환 후 inode를 크기 0으로 리셋; `ext2_close` 바로 아래 `ext2_dup` 신규(발견한 버그 수정 — `dup2` 시 `refcount` 증가) |
| `boot/ext2.h` | 수정 | `ext2_dup` 선언 추가 |
| `boot/kernel.c` | 수정 | `ext2_ops` 초기화의 `dup` 자리를 `0`에서 `ext2_dup`으로 |
| `user/init.c` | 수정 | `O_TRUNC`/`O_APPEND`/`sys_creat_trunc`/`sys_creat_append` 추가; `split_pipeline`이 `redirect_out[]`과 `redirect_append[]` 출력 인자를 받아 `>`/`>>` 토큰 파싱; 프롬프트 루프의 fork+dup2+exec 배선을 `run_line()`으로 추출; `run_argv_redirect()`가 `append` 인자를 받도록 확장; 부팅 시퀀스에 `echo`/트렁케이트/어펜드 검증 블록 6개 추가 |
| `Makefile` | 수정 | `ROOTFSBINLINKS`에 `echo` 추가(busybox 멀티콜 심링크); `run-nogui` 타임아웃 15s→20s(검증 블록 추가로 부팅 시퀀스가 더 길어짐) |
| 나머지 전부 | 변경 없음 | 59의 파일 그대로 |

## 다음 단계 힌트

- **ext2/ATA 동시 접근 락 없음 (위 "발견했지만 고치지 않은 버그" 참고)**: 두 프로세스가 동시에 ext2 실행 파일을 열면 재현 가능하게 깨진다. 디스크 I/O 경로(`ata.c`의 PIO 컨트롤러 접근, `ext2.c`의 스캐치 버퍼 공유)에 잠금 또는 직렬화가 필요하다 — 스케줄러/선점(`17~18`)이 이미 있으니 스핀락이든 "요청을 큐에 넣고 한 번에 하나씩" 방식이든 이 프로젝트의 다음 동시성 관련 단계에서 다뤄야 한다. 이후 진짜 병렬 셸 사용(`cmd1 | cmd2`에 ext2 바이너리 두 개)이 필요해지기 전에 반드시 해결해야 한다.
- **`<`(입력 리다이렉션) 없음**: `>`/`>>`(출력)는 이번 단계에서 갖췄지만 `<`는 다루지 않았다. 필요해지면 `redirect_out`/`redirect_append`와 나란히 `redirect_in` 배열을 추가하고, fork 자식 쪽에서 `sys_open(path, O_RDONLY)` 뒤 fd 0으로 `dup2`하는 정도로 확장 가능하다.
- **리다이렉트 대상이 같은 inode를 가리키는 동시 오픈에서 `O_TRUNC`의 일관성**: 지금 `ext2_open`의 트렁케이트는 그 순간 디스크상의 inode를 직접 갱신하지만, 그 inode를 이미 다른 fd로 열어 캐시(`ofiles[i].inode`)해 둔 프로세스가 있으면 그 캐시는 갱신되지 않는다(`ext2_open`의 dedup은 "같은 inode면 같은 슬롯 재사용"이라 이 경우엔 해당 없음 — 오히려 서로 다른 슬롯에 각자 캐시를 들고 있을 때 문제). 이번 단계의 사용 패턴(매번 새 프로세스가 열고 쓰고 곧바로 종료)에서는 안 드러나지만, 파일을 오래 열어두는 시나리오가 생기면 재검토 필요.
