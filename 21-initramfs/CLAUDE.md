# 21 — initramfs

**목표**: GRUB 모듈로 initramfs(CPIO newc 아카이브)를 메모리에 적재하고, `SYS_OPEN`/`SYS_READ` syscall로 ring 3에서 파일을 읽는다.

**20에서 이어짐**: 20에서 `int 0x80` 기반 syscall gate와 `SYS_WRITE`/`SYS_EXIT`가 동작했다. 21에서는 GRUB이 커널과 함께 initramfs를 메모리에 올려주고, `SYS_OPEN`/`SYS_READ`로 user_task가 파일 내용을 읽는다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
21-initramfs/
├── boot/
│   ├── console.c/h        # 20과 동일
│   ├── entry.asm          # 20과 동일
│   ├── interrupts.asm     # 20과 동일
│   ├── interrupts.c/h     # 20과 동일
│   ├── keyboard.c/h       # 20과 동일
│   ├── kheap.c/h          # 20과 동일
│   ├── monitor.c/h        # 20과 동일
│   ├── paging.c/h         # 20과 동일
│   ├── phys_mem.c/h       # 20과 동일
│   ├── timer.c/h          # 20과 동일
│   ├── context_switch.asm # 20과 동일
│   ├── gdt.h/c/asm        # 20과 동일
│   ├── thread.c/h         # 20과 동일
│   ├── initrd.h           # 신규: initrd_init/open/read 선언
│   ├── initrd.c           # 신규: CPIO newc 파서, 파일 테이블, fd 기반 읽기
│   ├── syscall.h          # 수정: SYS_OPEN(2), SYS_READ(3) 추가
│   ├── syscall.c          # 수정: sys_open, sys_read 추가
│   └── kernel.c           # 수정: multiboot_mod 파싱, initrd_init 호출, user_task 변경
├── initrd/
│   └── hello.txt          # 신규: initramfs에 담기는 샘플 파일
├── mkcpio.py              # 신규: CPIO newc 아카이브 생성 스크립트
├── grub/grub.cfg          # 수정: module /boot/initramfs.cpio 추가
├── linker.ld              # 20과 동일
├── Makefile               # 수정: initrd.o, CPIO 빌드, ISO에 모듈 포함
└── build/
```

## 핵심 개념

### GRUB 모듈과 Multiboot

`grub.cfg`의 `module` 지시자로 GRUB은 커널 외 파일을 메모리에 올려준다. 커널이 받는 Multiboot 정보 구조체에 모듈 목록이 들어온다:

| 필드 | 설명 |
|------|------|
| `flags & (1<<3)` | 모듈 정보 유효 여부 |
| `mods_count` | 모듈 수 |
| `mods_addr` | `struct multiboot_mod` 배열 주소 |

```c
struct multiboot_mod {
    u32 mod_start;  // 모듈 시작 물리 주소
    u32 mod_end;    // 모듈 끝 물리 주소 (exclusive)
    u32 cmdline;
    u32 pad;
};
```

### CPIO newc 포맷

Linux initramfs의 표준 포맷. 각 파일 항목 구조:

```
[110바이트 헤더] [파일명(namesize바이트)] [패딩→4바이트 정렬] [파일 데이터] [패딩→4바이트 정렬]
```

헤더 내 주요 필드 (모두 8자리 16진수 ASCII):
- 오프셋 54: `filesize` (파일 크기)
- 오프셋 94: `namesize` (파일명 길이, null 포함)
- 마지막 항목 파일명: `TRAILER!!!`

데이터 시작 오프셋 = `align4(110 + namesize)`, 다음 항목 오프셋 = 데이터 시작 + `align4(filesize)`

### initrd.c — CPIO 파서

`initrd_init(start, end)`에서 CPIO 아카이브를 순회하며 파일 정보(이름 포인터, 데이터 포인터, 크기)를 최대 16개까지 정적 테이블에 등록한다. 이름 앞의 `./`는 `initrd_open` 시 제거해 비교한다.

`initrd_read`는 파일마다 읽기 위치(`pos`)를 추적하므로 `SYS_READ`를 여러 번 호출하면 이어서 읽힌다.

### mkcpio.py

호스트에 `cpio`가 없는 환경에서도 CPIO newc 아카이브를 생성하기 위해 Python 스크립트로 직접 구현했다. `initrd/` 디렉토리의 파일을 알파벳 순으로 넣고 `TRAILER!!!` 항목으로 닫는다.

### 새 syscall

| 번호 | 이름 | 인자 | 반환 |
|------|------|------|------|
| 2 | `SYS_OPEN` | ebx=파일명 포인터 | fd (≥0) 또는 -1 |
| 3 | `SYS_READ` | ebx=fd, ecx=버퍼, edx=길이 | 읽은 바이트 수 |

user_task는 `SYS_OPEN("hello.txt")` → `SYS_READ(fd, buf, len)` → `SYS_WRITE(buf)` → `SYS_EXIT(0)` 순으로 호출한다.

## 명령

```bash
make            # build/kernel.elf, build/initramfs.cpio, build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 (3초 후 종료)
make clean
```

## 완료 기준

```
module[0]: 0x0012F000-0x0012F140 len=320
initramfs: 1 file(s) found
...
threads: all done -- entering user mode (ring 3), initramfs read via syscall
Hello from initramfs!
This file was loaded by GRUB as a multiboot module.
user task exited: code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | initrd.o 추가; CPIO 빌드 타깃; ISO에 initramfs.cpio 포함 |
| `grub/grub.cfg` | 수정 | `module /boot/initramfs.cpio` 추가 |
| `boot/initrd.h` | 신규 | `initrd_init`, `initrd_open`, `initrd_read`, `initrd_file_count` 선언 |
| `boot/initrd.c` | 신규 | CPIO newc 파서; 정적 파일 테이블; fd별 읽기 위치 추적 |
| `boot/syscall.h` | 수정 | `SYS_OPEN(2)`, `SYS_READ(3)` 추가 |
| `boot/syscall.c` | 수정 | `sys_open`, `sys_read` 구현; `initrd.h` include |
| `boot/kernel.c` | 수정 | `struct multiboot_mod` 추가; 모듈 파싱 및 `initrd_init` 호출; `user_task`에서 SYS_OPEN/SYS_READ 사용; `user_buf` 추가 |
| `initrd/hello.txt` | 신규 | initramfs에 담기는 샘플 텍스트 파일 |
| `mkcpio.py` | 신규 | Python 기반 CPIO newc 아카이브 생성기 |

## 다음 단계 힌트

- `22-elf-loader`: ELF 형식 사용자 프로그램을 initramfs에서 읽어 메모리에 적재하고 실행.
