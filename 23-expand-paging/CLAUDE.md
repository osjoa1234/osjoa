# 23 — expand-paging

**목표**: e820 전체 RAM을 direct map으로 커버하는 higher-half 커널로 전환한다. 커널 가상 주소를 0xC0000000+ 로 올리고, 물리 주소 X는 항상 X + 0xC0000000 으로 접근한다.

**22에서 이어짐**: 22까지는 커널과 프로세스가 같은 낮은 가상 주소 대역(0x100000+)에 공존했고, paging_init은 4MB 고정 identity map 하나만 세팅했다. 여기서는 가상 주소 공간을 구조적으로 분리하고(0~3GB 유저 / 0xC0000000+ 커널), e820 기반 동적 direct map으로 전체 RAM을 커널에서 접근 가능하게 한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### Higher-half 커널 메모리 맵

```
가상 주소                          물리 주소
────────────────────────           ────────────────────────
0x00000000                         0x00000000
│  유저 공간                       ├─ BIOS/reserved
│  (프로세스별 독립 매핑)           0x00100000
│                                  ├─ .boot (bootstrap)
│                                  0x00101000
0xBFFFFFFF                         ├─ 커널 코드/데이터/BSS
│                                  0x00400000
0xC0000000 ════════════════════════► 0x00000000  (direct map 시작)
0xC0101000 ════════════════════════► 0x00101000
├─ 커널 코드/데이터
├─ BSS (page_tables[], stack 등)
0xC0400000 ════════════════════════► 0x00400000
├─ 커널 힙                         ├─ free RAM
0xC07FFFFF                         │  (힙·프로세스 모두 여기서)
0xC0800000 ════════════════════════► 0x00800000
├─ direct map (free RAM)
0xC7FFFFFF ════════════════════════► 0x07FFFFFF
```

규칙: `virt = phys + 0xC0000000`

### 부트 순서

1. GRUB → `start` (물리 0x100000, `.boot` 섹션 VMA=LMA)
2. `start`: 임시 boot_pd 구성 — PDE[0]·PDE[768] 모두 물리 0-4MB로 매핑
3. CR3 = boot_pd(물리), CR0.PG = 1 → 페이징 ON
4. `jmp higher_half` (VMA 0xC01xxxxx, PDE[768]로 도달)
5. ESP += KERNEL_OFFSET, 정식 스택으로 교체
6. `kernel_main(magic, phys_mbi)`

### paging_init

- `page_directory[768+i]` 에 direct map 세팅 (PDE[768] = 0xC0000000 시작)
- page_tables[i] 물리 주소 = `(u32)page_tables[i] - KERNEL_OFFSET`
- CR3 교체 시 boot_pd 대신 새 page_directory 로드 → PDE[0] 자동 제거
- 힙 범위 PDE(769, 0xC0400000-0xC07FFFFF) 제외 → page_map_frame이 동적 할당

### page_map_frame

PDE 없을 때 `page_alloc()` 으로 PT 물리 프레임 획득, `pt_phys + KERNEL_OFFSET` 로 접근해 초기화. 유저 공간(0x00000000+)과 커널 힙(0xC0400000+) 모두 처리.

### ELF 적재

유저 ELF 세그먼트(p_vaddr = 0x00300000)는 커널 page_directory에 매핑이 없으므로 elf_load가 각 페이지를 `page_map_frame(vpage, page_alloc())`으로 직접 연결한 뒤 데이터를 복사한다.

### 링커 스크립트

`.boot` 섹션(VMA=LMA=0x100000)만 물리 주소에 직접 배치. 나머지는 VMA 0xC0101000+, LMA = VMA - KERNEL_OFFSET. GRUB는 ELF LMA 기준으로 적재하고 e_entry(0x100000) 로 점프.

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
Hello from ELF init!
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/console.h` | 수정 | `KERNEL_OFFSET 0xC0000000U` 추가 |
| `boot/console.c` | 수정 | VGA 버퍼 주소 `0xB8000 + KERNEL_OFFSET` |
| `linker.ld` | 수정 | `.boot` VMA=LMA=0x100000; 나머지 VMA=0xC0101000+, LMA=VMA-KERNEL_OFFSET |
| `boot/entry.asm` | 수정 | `.boot` 섹션에 bootstrap; boot_pd/boot_pt로 임시 매핑; higher_half 점프 |
| `boot/paging.c` | 수정 | direct map PDE[768+i]; PT 물리 주소 변환; page_map_frame 가상 접근 |
| `boot/paging.h` | 수정 | `paging_init(u32 mmap_vaddr, u32 mmap_length)` 서명 변경; `paging_mapped_mb()` 추가 |
| `boot/kheap.h` | 수정 | `KHEAP_START=0xC0400000`, `KHEAP_MAX=0xC0800000` |
| `boot/kheap.c` | 수정 | `KHEAP_MAX` 로컬 define 제거 (kheap.h로 이동) |
| `boot/phys_mem.h` | 수정 | `phys_mem_init` 인자명 변경 (mmap_vaddr, kernel_phys_end) |
| `boot/phys_mem.c` | 수정 | virtual mmap_vaddr 수신; physical kernel_phys_end로 커널 범위 마킹 |
| `boot/kernel.c` | 수정 | mbi 가상 변환; paging_init·phys_mem_init 호출 시 KERNEL_OFFSET 적용; mod 가상 접근 |
| `boot/elf.c` | 수정 | PT_LOAD 세그먼트 쓰기 전 page_map_frame으로 물리 프레임 연결 |

## 다음 단계 힌트

- `24-proc-addr-space`: per-process 페이지 디렉토리 클론, ELF 격리 적재 — 이제 커널/유저 가상 주소 공간이 분리되어 있으므로 프로세스별 PDE[0..767] 독립 관리가 자연스럽게 이어진다.
