# 62 — redirect-in

**목표**: 셸에 `<`(입력 리다이렉션)을 추가한다. `60-redirect`가 `>`/`>>`를 넣으며 남긴 힌트 — "`redirect_out`/`redirect_append`와 나란히 `redirect_in`을 추가하고, fork 자식에서 `O_RDONLY` open 후 fd 0으로 `dup2`" — 를 그대로 구현한다. `61-io-lock`에서 ext2 동시 접근이 안전해졌으니 파이프라인과 자유롭게 섞어 검증할 수 있다.

## 구현 — `redirect_out`과 대칭

`user/init.c`의 `split_pipeline`이 `>`/`>>` 토큰을 만나면 `redirect_out[nstages]`를 채우던 것과 완전히 같은 자리에 `<` 토큰 처리를 추가했다. 인자 하나(경로) 소비, 스테이지당 최대 1개, 파이프 `|` 경계에서 리셋 — 세 규칙 모두 `redirect_out`을 그대로 복사했다:

```c
if (streq(argv[i], "<")) {
    if (scount == 0U || i + 1U >= argc || redirect_in[nstages]) return 0U;
    redirect_in[nstages] = argv[i + 1U];
    i++;
    continue;
}
```

`run_line`의 fork 자식 쪽 적용 순서는 **파이프 dup2 → `redirect_in` dup2 → `redirect_out` dup2**다:

```c
if (i > 0U)
    sys_dup2((unsigned int)pipefd[i - 1U][0], 0U);
...
if (redirect_in[i]) {
    long ifd = sys_open(redirect_in[i]);
    if (ifd < 0) {
        writes("shell: cannot open file\n");
        sys_exit(1U);
        for (;;) {}
    }
    sys_dup2((unsigned int)ifd, 0U);
    sys_close((unsigned int)ifd);
}
if (redirect_out[i]) { ... }
```

파이프를 먼저 걸고 `redirect_in`이 있으면 그걸로 덮어쓰는 순서다 — 리눅스 셸(bash)도 같은 스테이지에 파이프 입력과 `<`가 동시에 있으면 `<`가 이긴다. 이 프로젝트에선 `stage_argv[0]`(파이프의 첫 스테이지)만 이전 파이프가 없으니 이 충돌이 실제로 관찰 가능하지만, 규칙 자체는 모든 스테이지에 동일하게 적용해뒀다.

`sys_open`은 `59-exec-vfs-symlink`/`60-redirect`부터 이미 `flags=0`(`O_RDONLY`)으로 여는 형태라 새 syscall 래퍼가 필요 없었다 — `sys_creat_trunc`/`sys_creat_append`처럼 플래그만 다른 변형을 또 만들 필요가 없어 기존 `sys_open` 그대로 재사용했다.

## 적용 범위 — `run_line`의 일반 파이프라인 경로만

`60-redirect`가 남긴 `run_argv_redirect`(부팅 시퀀스 안 `>` 데모 전용 헬퍼, `split_pipeline`을 안 거치는 하드코딩된 단일 명령 경로)는 건드리지 않았다. `<`는 `split_pipeline`/`run_line`의 일반 경로에만 추가했고, 검증도 전부 `run_line("cat < ...")` 형태로 실제 셸 파싱을 거치게 했다 — `run_argv_redirect`에 대응하는 `run_argv_redirect_in` 같은 별도 헬퍼를 새로 만들지 않았다(이미 있는 `>` 데모 하나만 예전 방식을 유지하는 것뿐, `<`를 위해 그 패턴을 복제할 이유가 없다).

## 검증

`user/init.c`의 `init_main`에 60/61의 파이프·리다이렉션 데모 뒤로 세 줄을 추가했다:

```c
run_line("cat < /disk/hello.txt");
run_line("cat < /disk/hello.txt | cat");
run_line("cat < /disk/does_not_exist.txt");
```

`make clean && make run-nogui` 결과:

```
shell: cat < /disk/hello.txt:
hello ext2 root fs
process 1 exited: code=0
shell: cat < /disk/hello.txt | cat:
process 1 exited: code=0
hello ext2 root fs
process 2 exited: code=0
shell: cat < /disk/does_not_exist.txt (missing file check):
shell: cannot open file
process 1 exited: code=1
```

세 경우 모두 기대대로다: 단독 `<`, 파이프 첫 스테이지의 `<`(뒤 스테이지로 정상 전달), 존재하지 않는 파일에 대한 에러 처리(`exit 1`, 커널 패닉 없음).

**회귀**: 51~61의 모든 ext2 읽기/쓰기/`getdents`/심링크/PATH exec/`>` 리다이렉션/파이프/동시성 검증이 이전과 동일하게 통과했고, `e2fsck -f -n build/disk.img`도 에러 없이 통과했다.

## 완료 기준

`make clean && make run-nogui`에서 61까지의 모든 출력 뒤에 다음이 이어져야 한다:

```
shell: cat < /disk/hello.txt:
hello ext2 root fs
process 1 exited: code=0
shell: cat < /disk/hello.txt | cat:
process 1 exited: code=0
hello ext2 root fs
process 2 exited: code=0
shell: cat < /disk/does_not_exist.txt (missing file check):
shell: cannot open file
process 1 exited: code=1
```

`e2fsck -f -n build/disk.img`가 에러 없이 통과해야 한다.

## 이전 단계(61) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `user/init.c` | 수정 | `split_pipeline`에 `redirect_in[SHELL_STAGE_MAX]` 파라미터와 `<` 토큰 파싱 추가; `run_line`에 `redirect_in` 배열 선언 + fork 자식에서 파이프 dup2 다음·`redirect_out` dup2 이전에 `sys_open`+`dup2(fd,0)` 삽입; 부팅 시퀀스에 `<` 단독/파이프 결합/누락 파일 검증 3종 추가 |
| 나머지 전부 | 변경 없음 | 61의 파일 그대로 |

## 다음 단계 힌트

- **`O_TRUNC`의 캐시 일관성 문제는 여전히 남아있다**: `60-redirect`가 지적하고 `61-io-lock`도 재확인한 항목 — 이미 열려 `ofiles[i].inode`에 캐시된 파일을 다른 프로세스가 `O_TRUNC`로 열어도 앞의 캐시가 갱신되지 않는다. 이번 단계는 관여하지 않았다.
- **`ext2_lock`은 여전히 파일시스템 전체를 잠그는 굵은 락**: `61-io-lock`이 남긴 대로, inode 단위 락/rwlock 세분화는 필요해지면 재검토.
- **PCI 버스 스캔(`63-pci-enum`)으로 이동**: 셸의 리다이렉션(`>`/`>>`/`<`)과 파이프가 모두 갖춰졌으니, 로드맵상 다음은 하드웨어 확장(PCI → AHCI → APIC)이다.
