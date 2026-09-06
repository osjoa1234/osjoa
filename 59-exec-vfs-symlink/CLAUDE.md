# 59 — exec-vfs-symlink

**목표**: `proc_exec`/`proc_spawn`를 initrd 직접 조회에서 VFS 경유로 리팩터링해, 실행 파일을 찾는 경로가 ext2의 심링크 해석을 그대로 통과하게 한다. 그리고 58이 배관만 놓은 `PATH` 환경변수를 셸이 실제로 뒤지게 만들어, `busybox`를 `cat`/`ls`/`sh`/`touch`/`mkdir`/`rm`으로 심링크한 멀티콜 바이너리를 짧은 이름으로 실행할 수 있게 한다.

**58에서 이어짐**: 58의 `proc_exec`/`proc_spawn`는 여전히 `initrd_open(name)`을 직접 호출했다 — `envp`를 커널이 실어 보내는 배관은 완성됐지만, `PATH=/`라는 값 자체는 아무도 뒤지지 않는 장식이었다. 실행 파일 탐색이 ext2(`/disk/...`)를 전혀 몰랐으므로, ext2에 심링크를 아무리 걸어도 그걸 "실행"할 방법이 없었다 — 54(`getdents`)에서 발견한 "ash가 `$PATH`에서 `cat`을 못 찾는" 문제의 진짜 원인이 바로 이것이었다.

## `proc_exec`/`proc_spawn` — `initrd_open` 대신 `vfs_open`

`boot/process.c`에 `exec_load_binary(path, &size)` 헬퍼를 새로 두었다: `vfs_open`으로 열고, `vfs_size`만큼 `kmalloc`한 버퍼에 `vfs_read`를 채워질 때까지 반복해 통째로 읽은 뒤 `vfs_close`한다. `elf_load`/`elf_setup_stack`은 처음부터 "연속된 메모리 버퍼 + 오프셋 포인터 연산"을 전제로 짜여 있었으므로(38부터 그대로), ext2처럼 블록 단위로 흩어진 파일도 커널 버퍼 하나에 전부 모아 넣기만 하면 그 아래 ELF 로더 코드는 한 줄도 바꿀 필요가 없었다.

`proc_spawn`/`proc_exec` 둘 다 `initrd_open(name)` + `initrd_data(fd)` + `initrd_size(fd)` 세 호출을 `exec_load_binary(name, &size)` 한 번으로 바꾸고, `elf_load_process`에 그 버���/사이즈를 넘긴 뒤 곧바로 `kfree`한다. `initrd`는 여전히 VFS 마운트("/") 뒤에서 그대로 서빙되므로 동작은 바뀌지 않고, 대신 이제 `/disk/...` 경로도 `proc_exec`가 그대로 받아들인다 — ext2의 `ext2_resolve_from`은 51~57에서 이미 경로의 모든 세그먼트(마지막 세그먼트 포함)에서 심링크를 따라가도록 짜여 있었으니, exec 경로가 VFS를 타는 순간 "실행 파일 탐색이 심링크를 따라간다"는 요구사항은 새 코드 없이 저절로 충족된다.

이 전환에는 부수 효과가 하나 있다: `vfs_open`은 절대경로(mount 접두사로 시작하는 경로)만 이해하므로, `kernel.c`가 `proc_spawn`을 부르던 자리(`proc_spawn("init")`)를 `proc_spawn("/init")`로 고쳤다 — 실제 리눅스도 initramfs의 첫 프로세스를 찾을 때 상대경로가 아니라 `/init`이라는 절대경로를 하드코딩해 쓰는 것과 같은 이유다.

## `PATH` 값 확장과 `/disk/bin` — 빌드타임에 심는 busybox 멀티콜 심링크

`proc_spawn`이 심는 `envp`가 `PATH=/`에서 `PATH=/disk/bin:/`로 바뀌었다 — ext2 쪽 디렉터리를 initrd보다 먼저 뒤지고, 거기 없으면 initrd 루트(`syscall64`, `busybox` 같은 실습용 바이너리들)로 폴백한다.

`/disk/bin`은 런타임에 `ext2_mkdir`/`ext2_symlink`로 채우지 않고 **빌드타임**에 `rootfs/bin`으로 준비해 `mkfs.ext2 -d`가 그대로 디스크 이미지에 구워 넣는다: `Makefile`의 `$(DISKIMG)` 규칙이 `rootfs/bin/busybox`로 busybox 바이너리를 복사한 뒤 그 옆에 `cat`/`ls`/`sh`/`touch`/`mkdir`/`rm`을 `ln -sf busybox`로 만든다. `mkfs.ext2 -d`는 심링크를 포함해 디렉터리 트리를 있는 그대로 옮기고, 대상 문자열이 60바이트 미만이라 ext2 "fast symlink"(`i_blocks == 0`, 타깃이 `i_block[]`에 인라인)로 저장되는데 — 이건 51~57에서 이미 짜둔 `ext2_read_symlink_target`의 `i_blocks == 0` 분기와 정확히 같은 표현이라 커널 쪽 코드는 한 글자도 안 바꿔도 된다. `rootfs/bin`은 `rootfs/singleindirect.txt`처럼 빌드 산출물이라 `rootfs/.gitignore`에 추가했고, `make clean`이 지운다.

## 셸의 PATH 기반 명령 탐색 — `exec_path`

리눅스에서 `$PATH` 탐색은 커널이 아니라 `execvp`/셸이 유저공간에서 한다 — `execve(2)` 자체는 항상 정확한 경로 하나만 받는다. 이 프로젝트도 그 구조를 그대로 따른다: `proc_exec`는 여전히 "정확히 하나의 경로"만 알고, 탐색은 `user/init.c`의 새 함수 `exec_path(argv, envp)`가 한다.

```c
static void exec_path(char *const argv[], char *const envp[])
{
    /* argv[0]에 '/'가 있으면 탐색 없이 그대로 sys_exec */
    /* 없으면 getenv("PATH")를 ':'로 끊어가며 "dir/argv[0]" 후보를 하나씩 sys_exec 시도 */
}
```

각 `PATH` 항목 뒤에 디렉터리 구분자 `/`를 붙이되, 항목이 이미 `/`로 끝나면(`PATH`의 두 번째 항목이 정확히 `/`인 경우) 또 붙이지 않는다 — 안 그러면 `//syscall64` 같은 이중 슬래시 경로가 되어 `vfs_open`의 마운트 접두사 매칭(`smatch`)이 한 겹 더 벗겨내면서 엉뚱한 이름을 찾게 된다. `sys_exec`는 `execve`처럼 성공하면 절대 돌아오지 않고, 실패하면 다음 `PATH` 항목으로 넘어간다 — 모든 항목이 실패하면 `run_argv`/파이프라인 루프의 기존 폴백(`"shell: not found"` + `exit(1)`)으로 떨어진다. `run_argv`와 파이프라인 실행 루프의 `sys_exec(argv[0], argv, environ)` 호출 두 곳을 전부 `exec_path(argv, environ)`로 바꿨다.

## 발견한 버그 — `sys_exec`/`sys_exit`의 `rax` 클로버 누락

`exec_path`를 처음 짰을 때 `PATH=/disk/bin:/`의 첫 항목(존재하지 않는 `/disk/bin/syscall64`)에서 실패하고 두 번째 항목(`/syscall64`)으로 넘어가야 하는데, 실제로는 절대 넘어가지 못하고 `syscall: unimplemented rax=4294967295`(`0xFFFFFFFF`, 즉 `-1`)이 찍히며 통째로 실패했다.

원인은 `sys_exec`/`sys_exit`의 인라인 asm이 `"a"(59L)`로 `rax`를 **입력**으로만 선언하고 클로버 목록에는 넣지 않았다는 것이었다 — `syscall` 명령은 반드시 `rax`에 반환값을 써서 돌아오므로 이건 GCC 인라인 asm 규칙 위반(입력으로 쓴 레지스터가 asm 안에서 바뀌면 반드시 출력/클로버로 선언해야 함)이다. 58까지는 `sys_exec`/`sys_exit` 호출 직후에 실패 시에도 살아있는 지역변수를 참조하는 코드가 없어서 이 미정의 동작이 우연히 티가 안 났을 뿐이다 — `exec_path`의 `while` 루프가 처음으로 "`sys_exec`가 실패하고 돌아온 뒤에도 루프 변수(`p`)가 온전해야 한다"는 조건에 실제로 의존하는 코드였고, 그 순간 컴파일러가 `rax`에 걸쳐 살아있다고 착각한 값이 깨지면서 다음 반복이 아예 실행되지 않거나 망가진 상태로 실행됐다.

고친 방법은 `sys_write`/`sys_read` 등 이미 값을 돌려받는 다른 래퍼들과 정확히 같은 모양으로 맞추는 것이었다 — `"a"(59L)` 입력 옆에 `"=a"(ret)` 출력을 추가해(반환값 자체는 버림) `rax`가 바뀐다는 사실을 컴파일러에게 정확히 알렸다:

```c
static void sys_exec(const char *name, char *const argv[], char *const envp[])
{
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(59L), "D"(name), "S"(argv), "d"(envp) : "rcx", "r11", "memory");
    (void)ret;
}
```

`sys_exit`도 같은 결함을 갖고 있었다(호출 뒤 살아있는 코드가 없어 지금까지 안 터졌을 뿐) — 형제 syscall 래퍼 사이의 일관성을 맞추기 위해 같은 패턴으로 고쳤다.

## 부팅 시퀀스 — `busybox`를 짧은 이름으로

58까지 `touch`/`mkdir`/`rm`/`ls`는 `argv[0]="busybox", argv[1]="touch", ...` 식으로 항상 busybox의 긴 이름 + 서브커맨드 방식으로 호출됐다. 이번 단계에서 이 네 호출을 `argv[0]="touch", argv[1]="/disk/touched.txt"` 같은 **짧은 이름 하나**로 바꿨다 — `exec_path`가 `PATH=/disk/bin:/`을 뒤져 `/disk/bin/touch`(busybox로의 심링크)를 찾아 실행하고, busybox는 자신의 `argv[0]`가 `"touch"`인 것을 보고 멀티콜 디스패치한다. 여기에 `cat /disk/hello.txt`(busybox를 `cat`으로) 검증과, `sh -c "cat /disk/hello.txt"`(busybox `ash`가 *자기 내부에서* `PATH`를 뒤져 `cat`을 찾아 실행하는지) 검증을 추가했다 — 후자가 54에서 발견한 한계의 진짜 해결 확인이다: 커널의 exec 경로가 VFS/심링크를 따라가고 `PATH`에 실행 파일이 실제로 존재하니, ash 자신의 (musl/busybox가 구현하는) `execvp` 탐색도 별도 커널 지원 없이 그냥 성공한다.

## 검증

1. **VFS 경유 exec + 심링크**: `touch`/`cat`/`mkdir`/`rm`/`ls`를 전부 짧은 이름으로 실행 — 각각 `/disk/bin/<name>` 심링크 → `busybox` ELF 로드 → 멀티콜 디스패치까지 한 번에 통과해야 한다.
2. **`PATH` 값 전달**: `check_getenv("PATH", "/disk/bin:/")`(1홉, 셸 자신), `syscall64 envtest`의 `envp[0] = PATH=/disk/bin:/`(2홉, `execve`로 전달된 자식) — 58의 두 검증 지점을 새 값으로 그대로 재사용.
3. **ash 자체 PATH 탐색**: `sh -c "cat /disk/hello.txt"`가 `hello ext2 root fs`를 출력해야 한다 — busybox `ash`가 커널 도움 없이 스스로 `$PATH`에서 `cat`을 찾아냈다는 증거.
4. **회귀**: 51~58의 ext2 읽기/쓰기/`getdents`/심링크 검증 전부 이전과 동일하게 통과해야 한다 — exec 경로만 바뀌었을 뿐 일반 파일 열기/읽기/쓰기 경로(`sys_open`/`sys_read`/`sys_write`)는 손대지 않았다.
5. **`make run-nogui` 뒤 `e2fsck -f -n build/disk.img`**: 에러 없이 통과.

주의: 이 단계의 검증은 디스크가 매 부팅 초기화된다는 전제로 짜여 있다(`check_mkdir`/`check_symlink`가 "새로 만들기"를 전제) — `build/disk.img`를 지우지 않고 `make run-nogui`를 반복 실행하면 이전 실행이 남긴 파일 때문에 `FAIL`이 뜬다. 이건 실제 디스크가 재부팅 사이에 상태를 유지하는 것과 같은 정상적인 동작이며, 검증은 항상 `make clean && make run-nogui`(또는 최소 `rm -f build/disk.img`)로 한다.

## 완료 기준

`make clean && make run-nogui`에서 `shell: linux-abi ready` 직후 다음이 보여야 한다:

```
shell: linux-abi ready
shell: getenv PATH: OK
shell: syscall64 envtest:
syscall64: argc = 0x0000000000000002
syscall64: argv[0000000000000000] = syscall64
syscall64: argv[0000000000000001] = envtest
syscall64: envp[0000000000000000] = PATH=/disk/bin:/
...
process 1 exited: code=0
```

그 뒤로 57까지의 ext2/심링크 검증이 전부 `OK`/`found`로 이어지고, 다음 네 블록이 모두 짧은 이름으로 실행되며 각각 `process N exited: code=0`로 끝나야 한다:

```
shell: touch /disk/touched.txt:
...
shell: cat /disk/hello.txt:
hello ext2 root fs
...
shell: mkdir /disk/bbdir:
...
shell: rm /disk/touched.txt:
...
shell: ls /disk:
...
bin
...
shell: sh -c 'cat /disk/hello.txt':
hello ext2 root fs
process 1 exited: code=0
```

(`busybox cat`/`ls`/`sh`가 내부적으로 쓰는 `sendfile`/`clock_gettime`/`fcntl`/`ioctl` 같은 최적화용 syscall은 이 커널에 없어 `syscall: unimplemented rax=...`가 찍히지만, busybox가 그 실패를 감지하고 폴백하므로 최종 출력과 종료 코드에는 영향이 없다 — 51~58에서도 이미 나타나던 것과 같은 성격의 메시지다.)

`ls /disk:` 출력에 새로 생긴 `bin` 디렉터리가 보여야 하고, `build/disk.img`에 대해 `e2fsck -f -n`이 에러 없이 통과해야 한다.

## 이전 단계(58) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/process.c` | 수정 | `exec_load_binary()` 신규(VFS로 열고 통째로 `kmalloc` 버퍼에 읽어 `elf_load_process`에 넘김); `proc_spawn`/`proc_exec`가 `initrd_open`/`initrd_data`/`initrd_size` 대신 이 헬퍼 사용; `proc_spawn`의 기본 `envp`가 `PATH=/disk/bin:/`로 확장 |
| `boot/kernel.c` | 수정 | `proc_spawn("init")` → `proc_spawn("/init")`(VFS는 절대경로만 해석) |
| `user/init.c` | 수정 | `exec_path()` 신규(PATH 환경변수를 뒤지는 명령 탐색, `run_argv`와 파이프라인 실행 루프가 이걸 사용하도록 교체); `sys_exec`/`sys_exit`의 인라인 asm에 `"=a"` 출력 추가(누락된 `rax` 클로버 버그 수정); `check_getenv` 기대값을 `/disk/bin:/`로 갱신; 부팅 시퀀스의 `touch`/`mkdir`/`rm`/`ls` 호출을 `busybox <sub>` 긴 형태에서 짧은 이름으로 교체, `cat`/`sh -c` 검증 블록 신규 |
| `Makefile` | 수정 | `$(DISKIMG)` 규칙이 `rootfs/bin/busybox`를 복사하고 `cat`/`ls`/`sh`/`touch`/`mkdir`/`rm` 심링크를 만든 뒤 `mkfs.ext2 -d`로 굽도록 확장; 디스크 이미지 크기 8MB→16MB; `run-nogui` 타임아웃 5s→15s(busybox exec 여러 번 + 심링크 검증으로 부팅 시퀀스가 길어짐); `clean`이 `rootfs/bin` 삭제; `PROCESSOBJ` 의존성에서 더 이상 쓰지 않는 `boot/initrd.h` 제거 |
| `rootfs/.gitignore` | 수정 | 빌드타임에 생기는 `bin/` 추가 |
| 나머지 전부 | 변경 없음 | 58의 파일 그대로 |

## 다음 단계 힌트

- **셸에 `cd`/상대경로 개념 없음**: `exec_path`는 `PATH` 항목이 빈 문자열인 경우(POSIX에서는 "현재 디렉터리"로 취급)를 구현하지 않는다 — 이 커널에는 아직 프로세스별 cwd가 없으므로 자연스러운 스코프 제외다. cwd/`chdir`이 프로세스 상태로 들어오면 그때 같이 다뤄야 한다.
- **`PATH_CAND_MAX`(128바이트) 상한**: 지금 값(`/disk/bin` + 파일명)으로는 여유롭지만, 더 깊은 디렉터리 계층이 생기면 재검토 필요.
- **`exec_load_binary`가 매 실행마다 전체 파일을 커널 힙에 복사**: initrd는 원래 포인터 하나로 됐을 걸 이제 항상 한 번 복사한다 — busybox(1.1MB)를 자주 실행하면 그때마다 커널 힙(4MB)에서 같은 크기만큼 빌렸다 반납한다. 지금 부팅 시퀀스 정도(직렬 실행, 매번 `kfree`로 반납)에서는 문제가 없지만, 나중에 데모섹 페이지 캐시나 mmap 기반 실행 파일 매핑이 들어오면 이 복사를 없앨 여지가 있다.
- **`rax` 클로버 누락 패턴이 다른 syscall 래퍼에도 있을 수 있음**: 이번에 `sys_exec`/`sys_exit`에서 발견한 건 "값을 안 돌려받는" 래퍼들에 공통된 패턴이다 — 앞으로 반환값을 안 쓰는 새 syscall 래퍼를 추가할 때는 처음부터 `"=a"(ret)` 출력을 넣는 걸 기본으로 한다.
