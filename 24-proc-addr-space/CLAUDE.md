# 24 — proc-addr-space

**목표**: 프로세스별 페이지 디렉토리를 클론하고, ELF를 전용 물리 프레임에 격리 적재하여 독립된 주소 공간에서 사용자 코드를 실행한다.

**23에서 이어짐**: 23에서 higher-half 커널과 e820 전체 RAM direct map을 구축했다. `page_alloc()`이 돌려준 물리 프레임 X는 `X + KERNEL_OFFSET`으로 커널에서 직접 접근할 수 있다. 여기서는 그 특성을 활용해, 커널 PDE[768..1023]는 공유하고 PDE[0]만 프로세스 전용 page_table을 갖는 per-process 페이지 디렉토리를 구현한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### per-process 페이지 디렉토리 레이아웃

| PDE 인덱스 | 가상 주소 범위 | 내용 |
|------------|--------------|------|
| 0 | 0x00000000–0x003FFFFF | 프로세스 전용 page_table (초기 비어 있음) |
| 1–767 | 0x00400000–0xBFFFFFFF | 0 (미사용) |
| 768–1023 | 0xC0000000– | 커널 공유 (direct map + 힙) |

`paging_clone_dir()`이 새 프로세스 PD를 만들 때 PDE[0]에 빈 page_table을 붙이고, PDE[768..]는 커널 `page_directory`에서 그대로 복사한다. 이로써 syscall 중에도 커널 코드(0xC0100000+, PDE[768])가 접근 가능하다.

### paging_clone_dir()

```
1. page_alloc() → pd_phys    (새 페이지 디렉토리 프레임)
2. page_alloc() → pt0_phys   (PDE[0] 전용 page_table 프레임)
3. new_pt0 = (u32*)(pt0_phys + KERNEL_OFFSET) — 접근 후 전부 0으로 초기화
4. new_pd[0]     = pt0_phys | 0x07
5. new_pd[1..767] = 0
6. new_pd[768..1023] = page_directory[768..1023]  (커널 공유)
```

반환값 `pd_phys`는 프로세스의 CR3에 적재할 물리 주소.

### elf_load_process(data, size, pd_phys)

PT_LOAD 세그먼트마다 페이지 단위로:
1. `page_alloc()` → `frame` (전용 물리 프레임)
2. `(u8*)(frame + KERNEL_OFFSET)` 로 접근해 프레임 초기화
3. filesz 범위 내 ELF 데이터를 프레임에 복사
4. `paging_map_user_page(pd_phys, vaddr + pg, frame)` — 프로세스 PD의 PTE 설정

사용자 스택 프레임(`PROC_USTACK_TOP - 0x1000 = 0x3FF000`)도 동일하게 할당·매핑한다.

### paging_map_user_page(pd_phys, vaddr, paddr)

`pd = (u32*)(pd_phys + KERNEL_OFFSET)`로 프로세스 PD에 접근한 뒤, `pd[pde_idx] & ~0xFFF`로 PT 물리 주소를 얻어 `(u32*)(pt_phys + KERNEL_OFFSET)`로 PTE를 설정한다. 모든 프레임 접근에 direct map(`+ KERNEL_OFFSET`)을 사용한다.

### paging_set_dir / paging_restore_kernel_dir

- `paging_set_dir(pd_phys)`: CR3 = pd_phys — 프로세스 주소 공간으로 전환
- `paging_restore_kernel_dir()`: CR3 = `kernel_pd_phys` — 커널 PD로 복원

`paging_set_dir` 후 `enter_user_mode`로 진입하면 프로세스 전용 공간에서 사용자 코드가 실행된다. syscall(int 0x80) 복귀 시에도 PDE[768..] 공유로 커널 코드·데이터에 접근 가능하다.

### 커널 유지 흐름

```
paging_clone_dir()          → pd 생성 (커널 PD 사용 중)
elf_load_process(…, pd)     → 프레임 할당·복사 (KERNEL_OFFSET 접근, 커널 PD 사용 중)
paging_set_dir(pd)          → CR3 = pd (프로세스 PD로 전환)
enter_user_mode(entry, esp) → ring 3 진입
```

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 (3초 후 종료)
make clean
```

## 완료 기준

```
paging: 128MB direct mapped CR3=0x0012D000
heap: init at 0xC0400000
elf: loaded init into isolated address space (entry=0x00300000)
threads: all done -- entering user mode (ring 3), ELF init
Hello from ELF init!
user task exited: code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `linker.ld` | 수정 | 23의 higher-half 링커 스크립트 유지 |
| `boot/entry.asm` | 수정 | 23의 higher-half bootstrap 유지 |
| `boot/console.h` | 수정 | `KERNEL_OFFSET 0xC0000000U` (23에서 이어짐) |
| `boot/console.c` | 수정 | VGA 버퍼 `0xB8000 + KERNEL_OFFSET` (23에서 이어짐) |
| `boot/phys_mem.h` | 수정 | `phys_mem_init(mmap_vaddr, …, kernel_phys_end)` 서명 (23에서 이어짐) |
| `boot/phys_mem.c` | 수정 | virtual mmap_vaddr 수신 (23에서 이어짐) |
| `boot/kheap.h` | 수정 | `KHEAP_START=0xC0400000`, `KHEAP_MAX=0xC0800000` (23에서 이어짐) |
| `boot/kheap.c` | 수정 | local `KHEAP_MAX` 제거 (kheap.h 사용) (23에서 이어짐) |
| `boot/paging.h` | 수정 | `paging_clone_dir`, `paging_set_dir`, `paging_restore_kernel_dir`, `paging_map_user_page` 선언 추가 |
| `boot/paging.c` | 수정 | `kernel_pd_phys` 저장; 위 4개 함수 구현 (모든 프레임 접근에 `+ KERNEL_OFFSET`) |
| `boot/elf.h` | 수정 | `PROC_USTACK_TOP 0x00400000U` 정의; `elf_load_process` 선언 추가 |
| `boot/elf.c` | 수정 | `elf_load_process` 구현 (`frame + KERNEL_OFFSET` 접근) |
| `boot/kernel.c` | 수정 | `user_stack` 제거; `paging_clone_dir` → `elf_load_process` → `paging_set_dir` → `enter_user_mode` |

## 다음 단계 힌트

- `25-proc-spawn-exit`: `process_t`, proc_table, `proc_spawn`, `proc_exit`, `SYS_SPAWN` — 프로세스 테이블과 생애 관리
