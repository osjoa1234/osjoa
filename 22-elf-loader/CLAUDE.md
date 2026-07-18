# 22 — elf-loader

**목표**: initramfs에서 ELF32 실행 파일(`init`)을 읽어 메모리에 적재하고, ring 3에서 진입점으로 점프한다.

**21에서 이어짐**: 21에서 ring 3 코드는 커널 이미지에 포함된 `user_task` 함수 포인터로 진입했다. 22에서는 별도로 컴파일된 ELF 바이너리를 initramfs에서 꺼내 메모리에 올린 뒤, ELF 헤더가 지정하는 진입점(`e_entry`)으로 진입한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
22-elf-loader/
├── boot/
│   ├── ...                # 21과 동일
│   ├── initrd.h           # 수정: initrd_data, initrd_size 추가
│   ├── initrd.c           # 수정: initrd_data, initrd_size 구현
│   ├── elf.h              # 신규: Elf32_Ehdr, Elf32_Phdr, elf_load 선언
│   ├── elf.c              # 신규: ELF32 파서 및 세그먼트 적재
│   └── kernel.c           # 수정: user_task/user_buf 제거; elf_load 호출로 교체
├── user/
│   ├── init.c             # 신규: ring 3 사용자 프로그램 (SYS_WRITE + SYS_EXIT)
│   └── user.ld            # 신규: 사용자 ELF 링커 스크립트 (base=0x300000)
├── initrd/                # 빌드 시 init 바이너리가 복사됨
├── grub/grub.cfg
├── linker.ld
├── Makefile               # 수정: user ELF 빌드 타깃; elf.o 추가; initramfs에 init 포함
└── build/
```

## 핵심 개념

### ELF32 포맷

ELF 실행 파일은 헤더(`Elf32_Ehdr`)와 프로그램 헤더 배열(`Elf32_Phdr`)로 구성된다.

| 필드 | 설명 |
|------|------|
| `e_ident[0..3]` | 매직 `\x7fELF` |
| `e_type` | `2` = ET_EXEC (실행 파일) |
| `e_machine` | `3` = EM_386 (x86 32비트) |
| `e_entry` | 진입점 가상 주소 |
| `e_phoff` | 프로그램 헤더 배열 시작 오프셋 |
| `e_phnum` | 프로그램 헤더 개수 |
| `e_phentsize` | 프로그램 헤더 하나의 크기 |

프로그램 헤더 중 `p_type == PT_LOAD (1)` 인 세그먼트를 `p_vaddr` 주소에 적재한다:

- 파일에서 `p_filesz` 바이트를 `p_vaddr`로 복사
- 나머지 `p_memsz - p_filesz` 바이트를 0으로 초기화 (.bss 등)

### elf_load

`elf_load(data, size)` 는 CPIO 아카이브 내 ELF 데이터를 직접 파싱해 세그먼트를 목적지에 복사하고, `e_entry`를 반환한다. 유효하지 않은 ELF면 0을 반환한다.

### initrd_data / initrd_size

ELF 파일은 CPIO 아카이브 안에 이미 메모리에 올라와 있다. `initrd_data(fd)` 와 `initrd_size(fd)` 로 복사 없이 해당 데이터 포인터와 크기를 얻어 `elf_load`에 바로 넘긴다.

### 사용자 프로그램 메모리 레이아웃

페이지 테이블(`page_table_0`)은 첫 4MB(0x000000~0x3FFFFF)를 user 비트(`0x07`)로 identity mapping한다. 사용자 ELF를 0x300000에 링크하면 별도 페이지 추가 없이 커널과 사용자 코드가 같은 매핑 안에 공존한다.

| 주소 | 내용 |
|------|------|
| 0x100000 | 커널 이미지 시작 |
| 0x130000 | GRUB이 올린 initramfs |
| 0x300000 | elf_load가 복사한 init ELF |
| 0x400000 | 커널 힙(KHEAP_START) |

### 사용자 프로그램 빌드

`user/init.c`는 커널과 동일한 `-ffreestanding -fno-builtin -fno-pic -fno-pie` 플래그로 컴파일한다. `user/user.ld`가 링크 주소를 0x300000으로 고정하고, `_start`를 진입점으로 지정한다. `make`가 빌드 후 `initrd/init`으로 복사하고 CPIO 아카이브를 생성한다.

## 명령

```bash
make            # build/kernel.elf, build/init, build/initramfs.cpio, build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 (3초 후 종료)
make clean      # build/ 삭제 및 initrd/init 제거
```

## 완료 기준

```
module[0]: 0x00130000-0x00131400 len=5120
initramfs: 1 file(s) found
...
elf: loaded init (entry=0x00300000)
threads: all done -- entering user mode (ring 3), ELF init
Hello from ELF init!
user task exited: code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/elf.h` | 신규 | `Elf32_Ehdr`, `Elf32_Phdr` 타입 정의; `elf_load` 선언 |
| `boot/elf.c` | 신규 | ELF32 헤더 검증; PT_LOAD 세그먼트를 가상 주소에 복사 |
| `boot/initrd.h` | 수정 | `initrd_data(fd)`, `initrd_size(fd)` 추가 |
| `boot/initrd.c` | 수정 | `initrd_data`, `initrd_size` 구현 |
| `boot/kernel.c` | 수정 | `user_task`/`user_buf` 제거; `elf_load` + `initrd_data/size`로 교체; `elf.h` include |
| `user/init.c` | 신규 | ring 3 사용자 프로그램; `SYS_WRITE`로 메시지 출력 후 `SYS_EXIT` |
| `user/user.ld` | 신규 | 사용자 ELF 링커 스크립트 (base=0x300000, entry=_start) |
| `Makefile` | 수정 | `elf.o` 추가; user ELF 빌드 타깃(`init_user.o` → `build/init`); initramfs에 `init` 포함 |

## 다음 단계 힌트

- `23-processes`: 주소 공간 + PID + `spawn/exit/wait`를 갖는 프로세스 실행 모델.
