# 41 — brk

**목표**: `process_t`에 `heap_start`/`heap_end`를 추가하고 `sys_brk(45)`를 구현해, 유저 프로세스가 ELF 로드 직후 영역(BSS 끝) 위쪽으로 힙을 늘릴 수 있게 한다. musl malloc이 초기 청크를 얻어오는 전제조건.

**40에서 이어짐**: 40은 `proc_spawn`/`proc_fork`/`proc_exec`가 프로세스 하나를 실제로 띄우고 `fork`+`exec`+`wait`까지 검증했지만, 유저 주소공간에서 동적으로 자라는 영역은 없었다 — `elf_load_process`가 매핑하는 건 PT_LOAD 세그먼트와 스택 한 페이지(`PROC_USTACK_TOP - 0x1000`)뿐이다. 이번 단계는 그 사이, ELF가 끝나는 지점부터 스택 아래까지의 빈 주소공간에 `brk`로 온디맨드 페이지를 매핑하는 길을 낸다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### heap_start 계산 — elf_load_process가 세그먼트 순회 중 얻는 부산물

`elf_load_process`는 이미 각 `PT_LOAD` phdr을 순회하며 `phdr->p_vaddr + phdr->p_memsz`만큼 페이지를 매핑하고 있었다. 이 루프 안에서 `p_vaddr + p_memsz`의 최댓값(`brk_end`)을 같이 추적하고, 루프가 끝난 뒤 페이지 경계로 올림(`(brk_end + 0xFFF) & ~0xFFF`)한 값을 새 출력 인자 `u64 *out_brk_start`로 돌려준다. 이 값이 라운드업되는 이유는 BSS 끝이 페이지 중간에서 끝나기 때문 — 이미 그 페이지는 세그먼트 로딩 루프가 매핑해뒀으므로, `brk`는 그 다음 미매핑 페이지부터 새로 만들어야 한다. `elf_load`(프로세스 없는 단순 로더, 현재 미사용 죽은 코드)는 이 인자가 필요 없어 건드리지 않았다.

### proc_brk — 이미 매핑된 상한(round_up(heap_end))만큼만 새로 매핑

```
old_top = round_up(heap_end)   // 이전에 실제로 매핑해둔 페이지 상한
new_top = round_up(new_brk)    // 이번에 필요한 페이지 상한
[old_top, new_top) 구간만 page_alloc + paging_map_user_page
heap_end = new_brk             // 페이지 경계에 맞추지 않은 정확한 값 그대로 저장
```

`heap_end`는 항상 요청받은 정확한 바이트 값(페이지 정렬 전)을 저장하고, 다음 호출에서 `round_up(heap_end)`로 "이미 매핑된 페이지 상한"을 재계산한다 — 별도 필드 없이 `heap_end` 하나로 충분한 이유는 `round_up(new_brk)`가 이번 호출이 끝난 뒤의 "실제 매핑 상한"과 항상 일치하기 때문이다(다음 호출의 `old_top`이 정확히 그 값이 되어 재매핑 없이 이어진다). `new_brk < heap_start`인 요청(축소 요청, 그리고 관례상 조회용 `brk(0)`도 여기 포함된다 — 0은 항상 `heap_start`보다 작으므로 별도 분기 없이 "현재 `heap_end` 그대로 반환"으로 처리된다)은 페이지를 매핑하지 않고 `heap_end`를 그대로 반환한다.

### brk 축소 — paging_unmap_user_page로 실제 언맵

`paging.c`에는 프로세스 전체를 죽일 때 쓰는 `paging_free_user_pages`(전체 순회)만 있었고, 임의의 유저 페이지 한 장을 언맵하는 함수가 없었다. `brk(2)`는 축소 요청을 실제로 이행해 페이지를 해제하는 게 커널의 몫이지 malloc의 몫이 아니므로(malloc은 "언제 축소를 요청할지"라는 정책만 결정할 뿐, 축소 요청이 왔을 때 이를 이행하는 건 syscall 구현의 책임이다), `walk_pt`(`create=0`)로 PTE를 찾아 `page_free` 후 0으로 지우는 `paging_unmap_user_page(pml4_phys, vaddr)`를 추가했다. `proc_brk`는 `[old_top, new_top)` 매핑 루프 뒤에 `[new_top, old_top)` 언맵 루프를 대칭으로 둔다 — 둘 중 성장/축소에 따라 한쪽만 실제로 순회한다.

### fork/exec에서의 heap 필드 전파

`proc_fork`는 자식이 부모의 힙 페이지를 통째로 복사받으므로(`paging_copy_user_pages`) `heap_start`/`heap_end`도 그대로 복사한다. `proc_exec`는 `paging_free_user_pages`로 이전 주소공간을 통째로 버리고 새 ELF를 로드하므로, 새 바이너리의 `elf_load_process`가 돌려주는 `brk_start`로 `heap_start`/`heap_end`를 다시 초기화한다. `proc_clone`(같은 프로세스 안의 추가 쓰레드)은 `process_t`를 공유하므로 별도 처리가 필요 없다.

### user/brk.c — 힙 주소를 출력하고, 두 페이지에 걸친 쓰기로 매핑을 검증

리눅스 `sbrk()` 데모처럼 `sys_brk(0)`로 얻은 현재 힙 시작 주소와 `sys_brk(base+8192)` 이후의 새 브레이크 주소를 16진수로 출력한다(`write_hex` — 유저 쪽엔 `printf`가 없어 직접 구현). 주소 출력 자체는 매핑 여부를 증명하지 않으므로, 그 뒤 8192바이트(4KB 페이지 두 장) 전체에 값을 쓰는 루프를 조용히 실행한다 — 두 번째 페이지가 실제로 매핑되지 않았다면 이 쓰기 도중 `interrupt 0x0E`(페이지 폴트)로 죽어 `process N exited`가 찍히지 않는다. `process 1 exited: code=0`까지 나오는 것 자체가 두 페이지 모두 매핑됐다는 증거다. `hello`/`hello2`처럼 `init` 셸에서 `brk`라고 입력하면 실행된다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 확인 — $ 프롬프트에서 멈춤 (정상)
make clean
```

## 완료 기준

`make run-nogui`는 40과 동일하게 `$` 프롬프트에서 멈춘다(`initramfs: 6 file(s) found`로 파일 수만 5→6). GUI(`make run`)에서 `brk`를 입력하면:

```
$ brk
brk: heap start = 0x0000000000301000
brk: heap end   = 0x0000000000303000
process 1 exited: code=0
$
```

## 이전 단계(40) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/elf.h` | 수정 | `elf_load_process`에 `u64 *out_brk_start` 출력 인자 추가 |
| `boot/elf.c` | 수정 | `PT_LOAD` 순회 중 `p_vaddr+p_memsz` 최댓값을 추적해 페이지 정렬 후 `out_brk_start`로 반환 |
| `boot/process.h` | 수정 | `process_t`에 `heap_start`/`heap_end`(u64) 추가; `proc_brk` 선언 |
| `boot/process.c` | 수정 | `proc_spawn`/`proc_exec`가 `elf_load_process`의 `brk_start`로 힙 필드 초기화; `proc_fork`가 부모 힙 필드 복사; `proc_brk` 구현(온디맨드 페이지 매핑 + 축소 시 언맵) |
| `boot/paging.h` | 수정 | `paging_unmap_user_page` 선언 추가 |
| `boot/paging.c` | 수정 | `paging_unmap_user_page` 구현 — `walk_pt(create=0)`로 PTE를 찾아 `page_free` 후 클리어 |
| `boot/syscall.h` | 수정 | `SYS_BRK = 45` 추가 |
| `boot/syscall.c` | 수정 | `SYS_BRK` 케이스에서 `proc_brk(frame->rbx)` 호출 |
| `user/brk.c` | 신규 | `sys_brk`로 힙 시작/끝 주소를 출력하고 8KB(2페이지) 전체에 조용히 쓰기 — 검증용 유저 프로그램 |
| `Makefile` | 수정 | `user/brk.c` 빌드/링크, initrd에 `brk` 포함 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `42-mmap`: 익명 `mmap2(192)` + `mprotect`/`fstat` stub. `proc_brk`가 `paging_map_user_page`/`paging_unmap_user_page`로 온디맨드 매핑·언맵하는 패턴을 그대로 재사용할 수 있다 — 다만 mmap은 브레이크 힙과 겹치지 않는 별도 주소 구간(스택 아래쪽 등)을 스스로 골라야 하므로, `process_t`에 "다음 mmap 배치 주소" 같은 커서를 하나 더 둘 필요가 있을 것.
