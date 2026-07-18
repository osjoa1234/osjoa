# 23-1 — expand-paging

**목표**: e820 usable 범위를 읽어 전체 물리 RAM을 identity map한다. 4MB 고정 매핑을 동적 다중 페이지 테이블로 교체한다.

**22에서 이어짐**: 22에서 `paging_init()`은 page_table 하나(4MB)만 세팅했다. 커널 힙과 사용자 영역이 모두 첫 4MB 안에 몰려 있어 이후 프로세스 주소 공간을 분리할 여지가 없었다. 여기서는 e820 mmap을 파싱해 최고 usable 주소까지 커버하는 page_table 배열을 정적으로 배치하고, 모든 물리 RAM을 identity map한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### 왜 4MB가 한계였나

기존 `paging_init()`은 정적 `page_table_0[1024]` 하나만 세팅했다. page_table 하나는 1024 × 4KB = 4MB를 커버한다. 따라서 가상 주소 0x000000–0x3FFFFF 범위만 커널이 직접 읽고 쓸 수 있었다.

`page_map_frame`은 힙 가상 주소(0x400000+)용 page_table을 `page_alloc()`으로 동적 할당해 연결했다. 이 방식은 작동하지만 page_alloc이 돌려준 물리 프레임은 identity map 밖이라 커널이 그 page_table 자체를 초기화하기 위해 직접 포인터로 접근했다 — 우연히 identity map 밖 프레임을 물리 주소로 직접 쓰는 것이 통했을 뿐이다.

이후 단계에서 각 프로세스가 전용 물리 프레임을 받으면, 커널이 그 프레임에 ELF 데이터를 써야 한다. 프레임이 identity map 밖이면 직접 접근이 불가능하다.

### 해결: 정적 page_table 풀

```
MAX_PT 32개 × 1024 엔트리 × 4B = 128KB (BSS에 배치)
```

`paging_init(mmap_addr, mmap_length)`:
1. e820 type=1(usable) 항목을 스캔해 최고 end 주소 계산
2. 필요한 page_table 수 = ceil(max_addr / 4MB), MAX_PT로 상한
3. 각 page_table을 0x000000부터 순서대로 identity map (flags=0x07)
4. page_directory에 연결

QEMU 기본 128MB RAM이면 e820 usable 최대 주소가 0x07FE0000 ≈ 127.9MB → 32개 page_table(= 128MB).

### 커널 이미지 크기 변화

| 버전 | module 적재 주소 | 변화 |
|------|-----------------|------|
| 22 | 0x00130000 | — |
| 23-1 | 0x00150000 | +0x20000 = +128KB (page_table 풀 BSS 증가) |

### page_map_frame은 그대로 유지

kheap이 가상 주소 0x400000+를 요청할 때 해당 PDE가 이미 설정되어 있으므로 새 page_table 동적 할당이 발생하지 않는다. 기존 static page_table의 PTE만 갱신한다.

### paging_mapped_mb()

커널이 "얼마나 매핑됐는지" 출력할 수 있도록 `paging_init` 내부에서 계산한 MB를 static에 저장하고 `paging_mapped_mb()`로 노출한다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 로그 확인 (3초 후 종료)
make clean
```

## 완료 기준

```
paging: 128MB identity mapped CR3=0x0012A000
phys mem: 32561 free pages (127MB usable)
```

## 이전 단계 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/paging.h` | 수정 | `paging_init(u32, u32)` 서명 변경; `paging_mapped_mb()` 추가 |
| `boot/paging.c` | 수정 | 정적 `page_tables[MAX_PT][1024]` 풀; e820 파싱으로 필요한 수만큼 identity map |
| `boot/kernel.c` | 수정 | `paging_init(mmap_addr, mmap_length)` 호출; 출력 메시지에 MB 표시 |

## 다음 단계 힌트

- `23-2-proc-addr-space`: per-process 페이지 디렉토리 클론, `elf_load_process`, `activate_thread` — 프로세스 주소 공간 분리 인프라
