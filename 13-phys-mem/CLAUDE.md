# 13 — 물리 메모리 관리

**목표**: E820 usable 영역을 비트맵으로 관리하는 물리 페이지 프레임 할당기를 구현하고, `page_alloc()`으로 얻은 프레임을 4MB 이상 가상 주소에 매핑해 사용한다.

**12에서 이어짐**: 12에서는 정적 PDE[0]+PT 하나로 첫 4MB를 identity map하고 페이징을 켰다. 13에서는 그 위에 `phys_mem.c`를 추가하고, `page_map_frame()`으로 PDE[1] 이상 영역에도 동적으로 페이지를 매핑할 수 있게 확장한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
13-phys-mem/
├── boot/
│   ├── console.c/h      # 12와 동일
│   ├── entry.asm        # 12와 동일
│   ├── interrupts.asm/c/h # 12와 동일
│   ├── keyboard.c/h     # 12와 동일
│   ├── paging.c         # page_map_frame() 추가
│   ├── paging.h         # page_map_frame() 선언 추가
│   ├── phys_mem.c       # NEW: 비트맵 프레임 할당기
│   ├── phys_mem.h       # NEW
│   └── kernel.c         # phys_mem_init() 호출, alloc/map/free 시연
├── grub/grub.cfg
├── linker.ld            # kernel_start / kernel_end 심볼 추가
├── Makefile             # phys_mem.o 추가
└── build/
```

## 핵심 개념

- **비트맵 할당기**: 4GB를 4KB 페이지로 나누면 1M 페이지. 이를 32비트 워드 32768개(128KB)로 표현한다. 비트=1이 free. `.bss`에 정적으로 배치되므로 별도 초기화 코드 없이 0으로 시작한다.

- **초기화 순서**: ① 비트맵 전체를 used로 시작 → ② E820 type=1 영역을 페이지 단위로 정렬해 free로 표시 → ③ page 0(null 보호)과 커널 이미지 전체(`0x100000`~`kernel_end`)를 다시 used로 예약. `kernel_end`는 linker.ld의 심볼로 비트맵 자체도 포함한다.

- **`kernel_end` 심볼**: linker.ld에서 `.bss` 섹션 끝에 `kernel_end = .;`를 두면 커널 코드+데이터+BSS(비트맵 포함) 전체의 끝 주소를 C에서 `extern u32 kernel_end;`로 참조할 수 있다. `(u32)&kernel_end`가 실제 주소다.

- **`page_map_frame(vaddr, paddr)`**: `vaddr >> 22`로 PDE 인덱스를 구하고, PDE가 not-present면 `page_alloc()`으로 새 페이지 테이블 프레임을 할당해 연결한다. 이후 `(vaddr >> 12) & 0x3FF`로 PTE 인덱스를 구해 `paddr | 0x03`을 쓴다. 마지막에 CR3를 재로드해 TLB를 플러시한다.

- **identity map 범위 내 PT 할당**: 첫 4MB가 이미 identity map돼 있으므로, `page_alloc()`이 반환하는 프레임(비트맵 뒤 첫 free 페이지, 주로 저주소 영역)은 가상=물리 주소로 직접 접근 가능하다. 따라서 새 PT를 그 주소에 zero-fill하는 것이 안전하다.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI — E820 맵, phys mem 통계, alloc/map/free 시연
make run-nogui  # debugcon 로그 확인 후 3초 타임아웃 종료
make clean
```

## 완료 기준

- `phys mem: N free pages (MMB usable)` 출력 (QEMU 128MB 기준 약 32596 페이지 / 127MB)
- `page_alloc: 0x... 0x... 0x...` — 세 프레임 주소 출력
- `map virt=0x400000 -> phys=0x... write=0xDEADBEEF read=0xDEADBEEF OK`
- `after page_free x2: N free pages` — 2 증가 확인

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | phys_mem.c 빌드 대상 추가 |
| `boot/kernel.c` | 수정 | phys_mem_init() 호출, alloc/map/free 시연 |
| `boot/paging.c` | 수정 | page_map_frame() 추가 |
| `boot/paging.h` | 수정 | page_map_frame() 선언 추가 |
| `boot/phys_mem.c` | 신규 | E820 기반 비트맵 물리 페이지 프레임 할당기 |
| `boot/phys_mem.h` | 신규 | 물리 메모리 헤더 |
| `linker.ld` | 수정 | kernel_start/kernel_end 심볼 추가 |

## 다음 단계 힌트

- `14-kernel-heap`: `kmalloc(size)` / `kfree(ptr)` 구현. `page_alloc()`으로 얻은 프레임을 가상 주소에 매핑하고 그 위에 free-list 또는 bump allocator를 올린다.
