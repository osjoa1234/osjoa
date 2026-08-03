# 35 — user-shell

**목표**: 유저 공간에서 동작하는 미니 셸 — 프롬프트 출력 → 명령어 입력 → fork+exec → wait → 반복.

**34에서 이어짐**: 34까지는 `init`이 고정 시나리오(파일 읽기, seek, stdin 한 번)를 수행하고 종료했다. 여기서는 `init`을 인터랙티브 셸로 교체해, 사용자가 이름을 입력하면 initrd 안의 ELF를 찾아 실행하는 루프를 만든다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### 셸 실행 흐름

```
shell loop:
  write("$ ")
  read(0, buf, 63)         ← 한 줄 입력 (키보드 블로킹)
  strip trailing '\n'
  if buf == "exit" → sys_exit(0)
  pid = sys_fork()
  if pid == 0:             ← 자식
      sys_exec(buf)        ← SYS_EXEC: 현재 이미지를 initrd ELF로 교체
      sys_exit(1)          ← exec 실패 시 도달
  sys_wait(pid, &code)     ← 부모: 자식 종료 대기
  반복
```

### fd 상속

`proc_fork`가 부모 fds를 `vfs_dup`으로 복사하므로, 자식(exec된 프로그램)은 셸의 fd 0/1/2를 그대로 쓴다. `proc_exec`는 fd 3 이상만 닫고 0/1/2는 유지한다. hello/hello2가 fd 1에 write하면 콘솔로 출력된다.

### 명령어 프로그램 구조

```
user/hello.c  → build/hello  → initrd/hello
user/hello2.c → build/hello2 → initrd/hello2
user/init.c   → build/init   → initrd/init   (셸)
```

셸이 `hello`를 입력받으면 자식이 `sys_exec("hello")`를 호출 → 커널의 `proc_exec("hello")` → `initrd_open("hello")` → ELF 로드 → 유저 모드 재진입.

### sys_exec 래퍼

`SYS_EXEC = 6`. 성공하면 리턴하지 않는다(프로세스 이미지가 교체됨). 실패(파일 없음)하면 반환 — 셸은 "not found" 출력 후 `sys_exit(1)`.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI (키보드로 명령 입력 가능)
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui` (키보드 입력 없음, 5초 타임아웃):

```
...
processes: init spawned pid=0
shell: user-shell ready
$ (대기 — 정상)
```

`make run` (QEMU GUI, 키보드 입력):

```
shell: user-shell ready
$ hello
hello: Hello from hello!
process 1 exited: code=0
$ hello2
hello2: Hello from hello2!
process 2 exited: code=0
$ notexist
shell: not found
process 3 exited: code=1
$ exit
shell: bye
process 0 exited: code=0
processes: init exited code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `user/init.c` | 수정 | 고정 시나리오 init → 인터랙티브 셸: 프롬프트·read·fork+exec·wait 루프 |
| `user/hello.c` | 수정 | 구형 단일인자 sys_write → fd 기반 sys_write(fd, buf, len)로 교체, "hello: Hello from hello!" |
| `user/hello2.c` | 수정 | 동일, "hello2: Hello from hello2!" |
| `Makefile` | 수정 | hello/hello2 ELF 빌드 타겟 추가; initrd에 세 파일(init/hello/hello2) 포함 |
| `boot/kernel.c` | 수정 | 부팅 메시지 "user-shell"로 업데이트 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `36-linux-abi`: syscall 번호를 Linux i386 ABI에 맞게 정렬 — musl-static 바이너리 실행을 향한 첫 걸음
