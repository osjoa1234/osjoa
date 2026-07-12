# 12 — 페이징

**목표**: Multiboot 정보 구조체에서 E820 메모리 맵을 읽어 물리 메모리 레이아웃을 출력하고, 첫 4MB를 identity map하는 페이지 디렉토리/테이블을 설정한 뒤 CR0 PG 비트를 세워 x86 페이징을 활성화한다.

**11에서 이어짐**: 11에서는 IRQ1 키보드 드라이버를 붙이고 interrupt_dispatch 경로를 완성했다. 12에서는 그 위에 `paging.c`를 추가하고, 기존 page fault 예외 핸들러(vector 0x0E)를 확장해 CR2(fault 주소)를 출력한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
12-paging/
├── boot/
│   ├── console.c        # 11과 동일
│   ├── console.h
│   ├── entry.asm        # 11과 동일
│   ├── interrupts.asm   # 11과 동일
│   ├── interrupts.c     # page fault 시 CR2 출력 추가
│   ├── interrupts.h     # 11과 동일
│   ├── keyboard.c       # 11과 동일
│   ├── keyboard.h       # 11과 동일
│   ├── paging.c         # NEW: 페이지 디렉토리/테이블 + CR3/CR0 설정
│   ├── paging.h         # NEW
│   └── kernel.c         # E820 파싱 출력 + paging_init() 호출
├── grub/
│   └── grub.cfg
├── linker.ld
├── Makefile
└── build/
```

## 핵심 개념

- **E820 메모리 맵**: Multiboot flags bit 6이 세워진 경우 `mbi->mmap_addr`부터 `mbi->mmap_length` 바이트에 걸쳐 `multiboot_mmap_entry` 배열이 있다. 각 엔트리 앞에 4바이트 `size` 필드가 있으며, 다음 엔트리로 이동할 때 `offset += entry->size + 4`로 계산한다. type=1이 사용 가능한 RAM, 나머지는 예약/ACPI/불량.

- **identity mapping 4MB**: 페이지 디렉토리 1024 엔트리 중 PDE[0]에 `page_table_0`을 연결하고, `page_table_0`의 1024 엔트리가 물리 주소 0x000000~0x3FFFFF를 1:1 매핑한다. 커널은 1MB(0x100000)에 로드되므로 4MB 범위에 들어온다. PDE[1] 이상은 not-present(0)로 두어 해당 범위 접근 시 page fault가 발생한다.

- **CR3 로드 → CR0 PG 세우기**: `cr3_load(page_directory 물리 주소)` 후 `cr0_set_paging()`으로 CR0의 비트 31(PG)를 세운다. 이 순간부터 모든 메모리 접근이 페이지 테이블을 거친다.

- **CR2 = fault 주소**: page fault(vector 0x0E) 발생 시 CPU가 CR2에 접근하려 했던 가상 주소를 적재한다. `handle_exception`에서 vector==14일 때 `mov %%cr2, %0`으로 읽어 출력한다.

- **정적 페이지 구조체**: `page_directory`, `page_table_0` 모두 `__attribute__((aligned(4096)))`를 붙인 `.bss` 정적 배열. CR3는 물리 주소를 요구하므로, identity map 상태에서는 링커가 배치한 가상 주소 = 물리 주소다.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI — E820 맵, paging 메시지, keyboard echo
make run-nogui  # debugcon 로그 확인 후 3초 타임아웃 종료
make clean
```

## 완료 기준

- `make run-nogui` debugcon에 E820 엔트리(type=1 usable, type=2 reserved 등)가 출력된다.
- `paging enabled: 4MB identity mapped CR3=0x...` 메시지가 보인다.
- 그 이후 `keyboard demo: type to echo keystrokes`가 나오고 3초 후 정상 종료한다.
- `grub-file --is-x86-multiboot build/kernel.elf`가 성공 종료한다.

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `Makefile` | 수정 | paging.c 빌드 대상 추가 |
| `boot/interrupts.c` | 수정 | page fault(0x0E) 처리 시 CR2 출력 추가 |
| `boot/kernel.c` | 수정 | E820 메모리 맵 출력, paging_init() 호출 |
| `boot/paging.c` | 신규 | 페이지 디렉토리/테이블 설정, CR3/CR0 제어 |
| `boot/paging.h` | 신규 | 페이징 헤더 |

## 다음 단계 힌트

- `13-phys-mem`: E820 usable 영역을 비트맵으로 관리하고 `page_alloc()`/`page_free()`를 구현한다. PDE[1] 이상에 새 페이지를 매핑해 4MB 넘어를 사용할 수 있게 된다.
