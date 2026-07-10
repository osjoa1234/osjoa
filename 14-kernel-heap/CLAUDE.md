# 14 — 커널 힙

**목표**: `kmalloc(size)` / `kfree(ptr)`를 구현한다. `page_alloc()`으로 얻은 프레임을 `page_map_frame()`으로 가상 주소에 매핑하고, 그 위에 free-list allocator를 올린다.

**13에서 이어짐**: 13에서 비트맵 물리 페이지 할당기(`page_alloc`/`page_free`)와 가상 주소 매핑(`page_map_frame`)이 준비됐다. 14에서는 그 위에 `kheap.c`를 추가해 byte 단위 동적 할당을 제공한다. kernel.c의 page_alloc 직접 시연 코드는 kmalloc/kfree 시연으로 교체된다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 디렉토리

```text
14-kernel-heap/
├── boot/
│   ├── console.c/h        # 13과 동일
│   ├── entry.asm          # 13과 동일
│   ├── interrupts.asm/c/h # 13과 동일
│   ├── keyboard.c/h       # 13과 동일
│   ├── paging.c/h         # 13과 동일
│   ├── phys_mem.c/h       # 13과 동일
│   ├── kheap.c            # NEW: free-list 힙 할당기
│   ├── kheap.h            # NEW
│   └── kernel.c           # kheap_init() 호출, kmalloc/kfree/reuse 시연
├── grub/grub.cfg
├── linker.ld
├── Makefile               # kheap.o 추가
└── build/
```

## 핵심 개념

- **힙 가상 주소 공간**: `KHEAP_START = 0x400000`(4MB)부터 `KHEAP_MAX = 0x800000`(8MB)까지 사용. 첫 4MB identity map 직후 영역이므로 페이징 켠 상태에서 아직 매핑이 없다. `kheap_init()`이 첫 페이지를 `page_map_frame()`으로 매핑한 뒤 사용한다.

- **`struct block_hdr`**: 각 힙 블록 앞에 12바이트 헤더(`size`, `free`, `*next`)를 둔다. 블록 리스트는 단방향 연결로, 힙 가상 주소 공간 안에서 낮은 주소부터 순서대로 배치된다.

- **`heap_grow()`**: 물리 프레임 하나를 `page_alloc()`으로 얻고 `page_map_frame(heap_top, pa)`로 매핑한다. 마지막 블록이 free면 합쳐 크기를 늘리고, used면 새 free 블록으로 연결한다. `heap_top`은 PAGE_SIZE씩 증가한다.

- **`kmalloc(size)`**: 4바이트 정렬 후 `find_free()`로 충분한 free 블록을 탐색한다. 없으면 `heap_grow()` 한 번 후 재탐색. 찾은 블록이 `size + HDR_SIZE + 4` 이상이면 뒷부분을 새 free 블록으로 분리(splitting)한다.

- **`kfree(ptr)`**: 블록을 free로 표시하고, 바로 다음 블록이 free면 합친다(forward coalescing). 반복해 연속된 free 블록을 하나로 병합한다. 역방향 병합(backward coalescing)은 구현하지 않는다.

- **재사용 확인**: `kfree(b)` 후 `kfree(a)` 순서로 해제하면 `a->next == b`이고 b가 free이므로 kfree(a) 시점에 a와 b가 하나로 병합된다. 이후 `kmalloc(sizeof(u32))`는 a의 위치에서 할당되므로 `d == a`(reused)가 성립한다.

## 명령

```bash
make            # build/kernel.elf, build/os.iso 생성
make run        # QEMU GUI — heap 초기화, kmalloc/kfree/reuse 시연
make run-nogui  # debugcon 로그 확인 후 3초 타임아웃 종료
make clean
```

## 완료 기준

- `heap: init at 0x00400000` 출력
- `kmalloc: a=0x0040000C b=0x0040001C c=0x0040002C` — 헤더 12바이트 간격으로 연속 배치
- `*a=1 *b=2 c="hello"` — 값 정상 읽기
- `kfree b+a: used=8 bytes` — c만 남아 8바이트
- `kmalloc after free: d=0x0040000C *d=42 reused` — a 주소 재사용

## 다음 단계 힌트

- `15-pit-timer`: PIT IRQ0으로 tick 카운터 구현, 간단한 `ksleep(ms)` 추가.
