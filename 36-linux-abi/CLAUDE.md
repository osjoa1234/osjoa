# 36 — linux-abi

**목표**: syscall 번호를 Linux i386 ABI에 맞게 정렬 — 향후 musl-static 바이너리 실행을 위한 첫 걸음.

**35에서 이어짐**: 35까지는 커스텀 번호(write=0, fork=7, exec=6 등)를 사용했다. 여기서는 커널과 유저스페이스 양쪽의 번호를 Linux i386 표준에 맞추되, Linux에 없는 커스텀 syscall은 충돌 없는 높은 번호(200번대)로 이동시킨다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### 번호 매핑

| 이름 | 35 (커스텀) | 36 (Linux i386) |
|------|-------------|-----------------|
| exit | 1 | 1 (동일) |
| fork | 7 | 2 |
| read | 3 | 3 (동일) |
| write | 0 | 4 |
| open | 2 | 5 |
| close | 10 | 6 |
| waitpid | 5 | 7 |
| execve | 6 | 11 |
| lseek | 11 | 19 |
| clone | 8 | 120 |
| spawn | 4 | 200 (커스텀 유지) |
| thread_exit | 9 | 201 (커스텀 유지) |

### 변경 범위

- **커널 측** (`boot/syscall.h`): enum 상수 이름과 번호 변경 (`SYS_WAIT` → `SYS_WAITPID`, `SYS_EXEC` → `SYS_EXECVE` 등)
- **커널 측** (`boot/syscall.c`): switch case 레이블을 새 상수 이름으로 업데이트
- **유저 측** (`user/init.c`, `user/hello.c`, `user/hello2.c`): inline asm의 하드코딩 번호 변경
- **유저 측** (`user/clone.asm`): `mov eax, 8` → `120`, `mov eax, 9` → `201`

### spawn / thread_exit를 200번대로 보낸 이유

Linux에 존재하지 않는 커스텀 syscall이다. 0–199 범위에 두면 향후 Linux ABI 번호와 충돌한다. 37단계(brk=45), 38단계(set_thread_area=243) 등이 그 범위를 채울 예정이므로 미리 비워둔다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI (키보드 입력 가능)
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`:

```
Hello world -- protected mode (32-bit), C kernel, linux-abi
...
processes: init spawned pid=0
shell: linux-abi ready
$ (대기 — 정상)
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/syscall.h` | 수정 | syscall 번호를 Linux i386 ABI로 정렬; SYS_WAIT→SYS_WAITPID, SYS_EXEC→SYS_EXECVE |
| `boot/syscall.c` | 수정 | switch case 레이블을 새 상수 이름으로 업데이트 |
| `boot/kernel.c` | 수정 | 부팅 메시지 "linux-abi"로 업데이트 |
| `user/init.c` | 수정 | inline asm 번호 변경 (write:0→4, fork:7→2, wait:5→7, exec:6→11) |
| `user/hello.c` | 수정 | sys_write 번호 0→4 |
| `user/hello2.c` | 수정 | sys_write 번호 0→4 |
| `user/clone.asm` | 수정 | clone:8→120, thread_exit:9→201 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `37-brk`: `process_t`에 heap_end 추가, `sys_brk(45)` 구현 — musl malloc 전제조건
