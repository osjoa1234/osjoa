# 61 — io-lock

**목표**: `60-redirect`가 발견하고 미룬 버그 — ext2/ATA 동시 접근에 락이 없어서 두 프로세스가 동시에 디스크 파일을 열면 재현 가능하게 깨지는 문제 — 를 고친다. "파일 리다이렉션"과는 다른 개념이라 60에서는 미루고 여기서 단독으로 다룬다.

**60에서 이어짐**: `60-redirect/CLAUDE.md`가 남긴 힌트: "디스크 I/O 경로(`ata.c`의 PIO 컨트롤러 접근, `ext2.c`의 스캐치 버퍼 공유)에 잠금 또는 직렬화가 필요하다 ... 스핀락이든 '요청을 큐에 넣고 한 번에 하나씩' 방식이든." 후자(요청 직렬화 큐)로 구현했다.

## 원인 재확인

`boot/ext2.c`를 훑어보니 거의 모든 함수가 스택이 아니라 **함수 지역 `static` 배열**을 스캐치 공간으로 쓴다(`ext2_read`의 `block_buf`, `ext2_mkdir`/`ext2_unlink`의 `dir_buf`, `ext2_init`의 `sb_buf`/`gd_buf` 등 20곳 가까이). `static`이라 함수 호출마다 새로 할당되는 게 아니라 프로그램 전체에서 단 하나뿐인 저장소를 공유한다 — 원래 의도는 매 호출마다 최대 4096바이트짜리 배열을 커널 스택에 얹지 않으려는 것이었겠지만, 그 대가로 이 함수가 동시에 두 스레드에서 실행되면 서로의 스캐치 버퍼를 덮어쓴다.

`ata.c`도 마찬가지다 — `ata_read_sector`/`ata_write_sector` 하나가 "드라이브 선택 → 커맨드 발급 → BSY/DRQ 대기 → 256워드 전송"까지 여러 `outb`/`inb` 시퀀스로 이루어지는데, 이 시퀀스 중간에 다른 스레드가 끼어들어 같은 포트(`0x1F0`~`0x1F7`)를 건드리면 컨트롤러 상태 자체가 깨진다.

그런데 `ata.c`를 호출하는 곳은 이 커널 전체에서 `ext2.c`(정확히는 `ext2_read_block`/`ext2_write_block`)뿐이다 — `initrd`는 부팅 시 GRUB 모듈로 이미 메모리에 있어 디스크 I/O가 필요 없고, `pipe`/`console`도 ATA와 무관하다. 그래서 ATA 컨트롤러용 락을 별도로 만들 필요는 없다: **ext2 진입점 하나를 잠그면 그 안에서 호출되는 ATA 접근까지 자동으로 직렬화된다.**

## `ext2_lock` — 요청 직렬화 큐

`boot/ext2.c`에 대기 스레드 큐를 가진 락을 하나 추가했다. `47-signal`/`48-pipe`부터 있던 `wait_queue_t`(`wq_init`/`wq_add`/`wq_wake_all`)와 `thread_park`/`thread_unpark`를 그대로 재사용한다 — `pipe.c`의 `pipe_read`/`pipe_write`가 "버퍼가 비었으면 대기열에 걸고 park, 상대가 wake_all" 하는 흐름과 정확히 같은 패턴이다:

```c
typedef struct {
    u32          locked;
    wait_queue_t waiters;
} ext2_lock_t;

static ext2_lock_t g_ext2_lock;

static void ext2_lock_acquire(void)
{
    while (g_ext2_lock.locked) {
        wq_add(&g_ext2_lock.waiters, thread_current());
        thread_park();
    }
    g_ext2_lock.locked = 1U;
}

static void ext2_lock_release(void)
{
    g_ext2_lock.locked = 0U;
    wq_wake_all(&g_ext2_lock.waiters);
}
```

이름은 "락"이지만 SMP용 스핀락(원자적 test-and-set + 바쁜 대기)이 아니다. 이 커널은 코어가 하나뿐이고 락을 놓고 경합하는 두 스레드는 타이머 IRQ에 의한 선점으로만 교대하므로, 바쁜 대기 대신 `thread_park`로 완전히 양보하는 쪽이 맞다 — 진짜 리눅스도 디스크 I/O처럼 오래 걸릴 수 있는 임계구역은 스핀락이 아니라 뮤텍스/세마포어(잠들 수 있는 락)로 보호한다. 이 프로젝트엔 SMP 계획이 없으므로([[apic-65-x2apic-default]] 메모 참고 — 65의 x2APIC 논의에서도 확인했듯 코어는 계속 하나) `locked` 플래그에 원자적 명령어가 필요 없는 것도 같은 이유다: 플래그를 읽고 쓰는 사이에 끼어들 수 있는 유일한 존재가 이 스레드 자신을 선점하는 타이머 인터럽트뿐인데, `thread_park`/`thread_yield` 자체가 내부적으로 `interrupts_disable`로 감싸여 있어(`thread.c`) 상태 전이 도중에는 선점이 안 일어난다.

## 적용 범위 — `ext2_ops`에 등록된 13개 진입점 전부

`vfs_ops_t ext2_ops`에 등록된 함수(`open`/`read`/`write`/`size`/`close`/`dup`/`getdents`/`mode`/`mkdir`/`unlink`/`symlink`/`readlink`/`lstat`) 각각을 `_impl`로 이름을 바꾸고, 원래 이름으로 락만 걸었다가 푸는 얇은 래퍼를 그 옆에 추가했다:

```c
static int ext2_open_impl(const char *path, u32 flags) { /* 기존 본문 그대로 */ }

int ext2_open(const char *path, u32 flags)
{
    int ret;
    ext2_lock_acquire();
    ret = ext2_open_impl(path, flags);
    ext2_lock_release();
    return ret;
}
```

13개 전부 이 형태다. `ext2_size`/`ext2_close`/`ext2_dup`/`ext2_mode`처럼 본문이 짧아 경합 가능성이 낮아 보이는 함수도 예외 없이 포함했다 — 이들이 읽고 쓰는 `fs->ofiles[bfd]`가 `ext2_open`/`ext2_write`와 공유하는 바로 그 테이블이라, "이 함수는 짧으니 안전하다"는 판단 자체가 60의 버그를 만든 바로 그 가정이었다. 13개 함수 중 서로를 호출하는 조합이 있는지 먼저 확인했다 — 없다(모두 `ext2_resolve_path`/`ext2_scan_dir` 같은 `static` 헬퍼만 내부적으로 호출하고, 공개 진입점끼리는 서로 부르지 않는다) — 그래서 재진입 걱정 없이 전부 이 패턴으로 감쌀 수 있었다. `ext2_init`은 스케줄러가 시작되기 전 단일 스레드로만 실행되므로 락으로 감싸지 않고, 대신 그 안에서 `ext2_lock_init()`으로 락을 초기화한다.

## 인터페이스 일관성 확인 — 다른 백엔드는?

`vfs_ops_t`의 나머지 세 백엔드(`console_dev`/`initrd`/`pipe`)도 같은 문제가 있는지 짚어봤다. `initrd`는 부팅 시 이미 메모리에 올라온 cpio 이미지를 포인터로만 읽어 함수 지역 스캐치 버퍼가 아예 없고, `console_dev`도 마찬가지다. `pipe`는 애초에 `48-pipe`부터 `wait_queue_t` 기반으로 producer/consumer를 동기화하고 있었다(이번에 만든 `ext2_lock`과 같은 도구, 다른 용도) — 그래서 이번에 손댈 대상은 ext2뿐이었다.

## 검증

**재현 확인**: 고치기 전 60의 버그가 실제로 재현되는지부터 확인했다. 60-redirect에 이번 단계와 동일한 `run_line("cat /disk/hello.txt | cat")` 테스트를 추가해 돌려보면:

```
shell: cat /disk/hello.txt | cat (ext2 concurrency check):
shell: not found
process 2 exited: code=1
```

두 번째 스테이지(`cat`)의 `exec_path`가 `/disk/bin/cat` 심링크를 못 찾아 "not found"로 죽는다 — 60의 CLAUDE.md가 기록한 증상 그대로.

**수정 후**: 이번 단계(`ext2_lock` 적용)에서 같은 테스트를 부팅 시퀀스에 추가했다:

```c
{
    static char line[] = "cat /disk/hello.txt | cat";

    writes("shell: cat /disk/hello.txt | cat (ext2 concurrency check):\n");
    run_line(line);
}
```

`make run-nogui`에서 3회 반복 실행 모두 아래처럼 정상 출력된다(타이밍에 따라 달라지는 버그였으므로 반복 확인이 의미가 있다):

```
shell: cat /disk/hello.txt | cat (ext2 concurrency check):
process 1 exited: code=0
hello ext2 root fs
process 2 exited: code=0
```

**회귀**: 51~60의 모든 ext2 읽기/쓰기/`getdents`/심링크/PATH exec/리다이렉션 검증이 이전과 동일하게 통과했고, `e2fsck -f -n build/disk.img`도 에러 없이 통과했다.

## 완료 기준

`make clean && make run-nogui`에서 60까지의 모든 출력 뒤에 다음이 이어져야 한다:

```
shell: cat /disk/hello.txt | cat (ext2 concurrency check):
process 1 exited: code=0
hello ext2 root fs
process 2 exited: code=0
```

`e2fsck -f -n build/disk.img`가 에러 없이 통과해야 한다.

## 이전 단계(60) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/ext2.c` | 수정 | `wait_queue.h`/`thread.h` include 추가; `ext2_lock_t`/`ext2_lock_init`/`ext2_lock_acquire`/`ext2_lock_release` 신규; `ext2_ops`에 등록된 13개 함수(open/read/write/size/close/dup/getdents/mode/mkdir/unlink/symlink/readlink/lstat)를 전부 `_impl`로 이름을 바꾸고 락으로 감싸는 얇은 래퍼 추가; `ext2_init`에서 `ext2_lock_init()` 호출 |
| `user/init.c` | 수정 | 부팅 시퀀스에 `run_line("cat /disk/hello.txt | cat")` 동시성 검증 블록 추가 |
| 나머지 전부 | 변경 없음 | 60의 파일 그대로 |

## 다음 단계 힌트

- **`<`(입력 리다이렉션) 없음**: 60에서 남긴 힌트가 그대로 유효하다 — `redirect_out`/`redirect_append`와 나란히 `redirect_in` 배열을 추가하고, fork 자식 쪽에서 `sys_open(path, O_RDONLY)` 뒤 fd 0으로 `dup2`하면 된다. 이번 단계로 ext2 동시 접근이 안전해졌으니 `62-redirect-in`은 파이프라인과 자유롭게 섞어 검증해도 된다.
- **`O_TRUNC`의 캐시 일관성 문제는 여전히 남아있다**: 60의 CLAUDE.md가 지적한 대로, 어떤 inode를 이미 열어 `ofiles[i].inode`에 캐시해둔 프로세스가 있는 상태에서 다른 프로세스가 같은 파일을 `O_TRUNC`로 열면 앞의 캐시는 갱신되지 않는다. 이번 단계의 락은 "동시에 두 스레드가 같은 `ofiles[]` 슬롯을 헤집어 커널을 멈추게 하는" 크래시를 막았을 뿐, 이 논리적 불일치(같은 파일을 오래 열어두는 시나리오) 자체는 고치지 않았다 — 필요해지면 재검토.
- **`ext2_lock`은 파일시스템 전체를 잠그는 매우 굵은 락이다**: 지금은 디스크 I/O가 크지 않아 문제되지 않지만, 이후 여러 프로세스가 정말 동시에 서로 다른 파일을 다루는 게 흔해지면(예: 진짜 병렬 빌드 같은 워크로드) 이 락이 병목이 될 수 있다. 그때는 inode 단위 락이나 최소한 read/write를 분리하는 rwlock으로 세분화하는 걸 고려할 것 — 지금은 "크래시를 없앤다"는 이번 단계의 목표에 맞춰 가장 단순한 형태로 끝냈다.
