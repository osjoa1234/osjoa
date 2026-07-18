# 23-2 — proc-addr-space

**목표**: 프로세스별 페이지 디렉토리를 클론하고, ELF를 전용 물리 프레임에 적재하여 격리된 주소 공간에서 사용자 코드를 실행한다.

**23-1에서 이어짐**: 23-1에서 전체 물리 RAM을 identity map했다. 따라서 `page_alloc()`이 돌려준 어느 프레임이든 물리 주소 == 가상 주소로 직접 접근할 수 있다. 여기서는 그 특성을 활용해, 각 프로세스가 PDE[0]만 자기 전용 page_table을 갖고 나머지 커널 PDEs는 공유하는 per-process 페이지 디렉토리를 구현한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### per-process 페이지 디렉토리 레이아웃

커널 페이지 디렉토리(`page_directory`)를 그대로 복사하되, PDE[0]만 새로 할당한 page_table로 교체한다.

| PDE 인덱스 | 가상 주소 범위 | 내용 |
|------------|--------------|------|
| 0 | 0x000000–0x3FFFFF | 프로세스 전용 page_table (커널+사용자 코드 혼재) |
| 1–31 | 0x400000–0x7FFFFFF | 커널 공유 page_tables (identity map) |
| 32–1023 | 0x8000000+ | 0 (미사용) |

PDE[0] 전용 page_table에서 사용자 코드 영역(0x300000)의 PTE를 프로세스 전용 물리 프레임으로 교체한다. 커널 영역(0x100000–0x2FFFFF)은 그대로 복사되어 syscall 중에도 커널 코드·데이터에 접근 가능하다.

### paging_clone_dir()

```
1. page_alloc() → 새 page_directory 프레임 (pd_phys)
2. page_alloc() → 새 PDE[0] page_table 프레임 (pt0_phys)
3. pt0_phys에 kernel page_tables[0] 복사 (커널 커버리지 유지)
4. new_pd[0] = pt0_phys | 0x07
5. new_pd[1..1023] = page_directory[1..1023] (커널 공유)
```

반환값 `pd_phys`는 프로세스의 CR3에 적재할 물리 주소.

### elf_load_process(data, size, pd_phys)

PT_LOAD 세그먼트마다 페이지 단위로:
1. `page_alloc()` → 전용 물리 프레임 (identity map으로 직접 접근 가능)
2. 프레임을 0으로 초기화
3. filesz 범위 내 ELF 데이터를 프레임에 복사
4. `paging_map_user_page(pd_phys, vaddr + pg_offset, frame)` — 프로세스 pd의 PTE 갱신

사용자 스택 프레임(0x3FF000)도 동일하게 할당·매핑한다.

### paging_set_dir / paging_restore_kernel_dir

- `paging_set_dir(pd_phys)`: `mov pd_phys, %cr3` — 프로세스 주소 공간으로 전환
- `paging_restore_kernel_dir()`: 저장해둔 `kernel_pd_phys`로 CR3 복원

커널이 `paging_set_dir`을 호출한 뒤 `enter_user_mode`로 진입하면, 사용자 코드는 프로세스 전용 주소 공간에서 실행된다. syscall(int 0x80)로 커널로 돌아왔을 때도 같은 페이지 디렉토리가 유지되지만 커널 코드는 PDE[1-31](공유)로 접근 가능하다.

### 23-1 identity map 덕분에 가능한 것

23-1 이전에는 4MB 밖의 프레임을 `page_alloc()`이 반환하면 커널이 그 프레임을 직접 쓸 수 없었다. 23-1에서 128MB를 identity map했으므로 어느 프레임이든 `(u8 *)frame` 포인터로 직접 초기화·복사가 가능하다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 (3초 후 종료)
make clean
```

## 완료 기준

```
elf: loaded init into isolated address space (entry=0x00300000)
threads: all done -- entering user mode (ring 3), ELF init
Hello from ELF init!
user task exited: code=0
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/paging.h` | 수정 | `paging_clone_dir`, `paging_set_dir`, `paging_restore_kernel_dir`, `paging_map_user_page` 선언 추가 |
| `boot/paging.c` | 수정 | `kernel_pd_phys` 저장; 위 4개 함수 구현 |
| `boot/elf.h` | 수정 | `elf_load_process(data, size, pd_phys)` 선언 추가 |
| `boot/elf.c` | 수정 | `elf_load_process` 구현; `paging.h`, `phys_mem.h` include; `PROC_USTACK_TOP` 정의 |
| `boot/kernel.c` | 수정 | `user_stack` 제거; `paging_clone_dir` → `elf_load_process` → `paging_set_dir` → `enter_user_mode` |
| `Makefile` | 수정 | `elf.c` 빌드 의존성에 `paging.h`, `phys_mem.h` 추가 |

## 다음 단계 힌트

- `23-3-proc-spawn-exit`: `process_t`, proc_table, `proc_spawn`, `proc_exit`, `SYS_SPAWN` — 프로세스 테이블과 생애 관리
