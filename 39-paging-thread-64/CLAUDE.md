# 39 — paging-thread-64

**목표**: `paging.c`(4단계 페이지 테이블) + `kheap.h`(주소 재배치) + `thread.h`/`thread.c`(u64 전환) 포팅 — 커널 쓰레드만으로 검증, 유저모드는 아직 손대지 않음. 64비트 전환 3/4.

**38에서 이어짐**: 37~38은 진입(entry.asm)·IDT·인터럽트 프레임만 64비트로 바꿨을 뿐, `paging.c`/`kheap.c`/`thread.c`는 36 시절 32비트 코드가 한 글자도 안 바뀐 채(`kernel_main`이 `paging_init`/`kheap_init`/`threads_init`을 아예 호출하지 않았으므로) 죽은 코드로 컴파일만 되고 있었다. 이번 단계에서 이 세 파일을 64비트 주소 폭에 맞게 다시 쓰고, `kernel_main`에서 실제로 켜서 **커널 쓰레드 두 개가 스케줄러로 번갈아 실행되는 것**까지 확인한다. `process.c`/`elf.c`/`syscall.c`/`initrd.c`/`gdt.asm`은 여전히 32비트 타입 그대로 두고 죽은 코드로 남긴다 — 유저모드/프로세스는 40에서 다룬다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### entry.asm의 boot_pml4를 그대로 재사용

37의 `entry.asm`은 부팅 시 `boot_pml4`/`boot_pdpt_id`/`boot_pdpt_hi`/`boot_pd`라는 정적 4-레벨 테이블을 만들어 물리 0–1GB를 identity(저번지)와 `KERNEL_OFFSET`(-2GB) 양쪽에 2MB 페이지로 매핑한 뒤 `cr3`에 로드했다. `paging.c`를 처음부터 다시 만드는 대신, `entry.asm`에 `global boot_pml4`만 추가하고 C에서 그 물리주소(`(u64)boot_pml4 - KERNEL_OFFSET`)를 그대로 커널 최상위 테이블로 채택했다 — 32비트 시절 `paging_init`이 자기 소유의 `page_directory`를 새로 만들어 `cr3`를 갈아엎던 것과 달리, 이번엔 **엔트리가 이미 만들어 둔 테이블을 확장**하는 쪽을 택했다. 부트 시퀀스를 다시 구현하지 않아 위험이 적고, "이전 단계에서 이어짐"의 취지에도 더 맞는다.

### 4단계 주소 분해와 PML4[511] 공유

x86-64 4-레벨 페이징에서 가상주소는 다음과 같이 쪼개진다(우리가 쓰는 범위는 PT까지만, 2MB huge page는 PD에서 끝난다):

| 비트 | 47:39 | 38:30 | 29:21 | 20:12 | 11:0 |
|------|-------|-------|-------|-------|------|
| 의미 | PML4 idx | PDPT idx | PD idx | PT idx | 페이지 내 offset |
| 예시(KHEAP_START=0xFFFFFFFFC0400000) | 511 | 511 | 2 | 0 | 0 |

`boot_pml4`가 이미 채워둔 항목은 인덱스 0(identity, 저번지 0-1GB)과 511(커널 -2GB~-1GB, `boot_pdpt_hi[510]`)뿐이다. `paging_clone_dir()`(40에서 프로세스가 쓰기 시작한다)로 받는 새 PML4는 511번만 커널 테이블과 **공유**(포인터 복사, 페이지 통째로 공유)하고 나머지 510개는 0으로 비우도록 이미 설계해 뒀다 — 커널 코드/힙은 모든 주소공간에서 항상 보이고, 유저 페이지는 프로세스마다 완전히 독립이게 하기 위해서다.

```c
new_pml4[511] = kpml4[511];   // 커널 절반 공유, 나머지는 0
```

### PTE 플래그와 4단계 전부에 U 비트 필요

```
bit 0  P  (present)
bit 1  RW (read/write)
bit 2  US (user/supervisor)
```

유저 페이지는 **PML4E·PDPTE·PDE·PTE 네 단계 전부**에 U=1이 서 있어야 링3에서 접근 가능하다(중간 단계 하나라도 U=0이면 CPU가 감독자 전용으로 취급). `paging.c`의 `next_level()`이 매 단계마다 `user` 인자를 받아 새로 만드는 테이블마다 US를 세팅하고, 이미 존재하는 테이블이면 `table[index] |= PTE_US`로 뒤늦게라도 승격시킨다 — 이 경로는 40에서 유저 페이지를 실제로 매핑할 때부터 쓰인다.

### page_map_frame / paging_map_user_page — 공용 4단계 워커

```c
static u64 *next_level(u64 *table, u32 index, int user, int create);  // 없으면 page_alloc()로 새로 만들고, 있으면 재사용
static u64 *walk_pt(u64 *pml4, u64 vaddr, int user, int create);      // PML4->PDPT->PD까지 내려가 PT를 반환
```

커널 힙(`page_map_frame`, `user=0`)과 유저 페이지(`paging_map_user_page`, `user=1`, 40에서 사용)가 이 두 함수를 공유한다. `paging_free_user_pages`/`paging_copy_user_pages`(역시 40부터 실제로 호출됨)는 PML4 0~510번을 전부 순회하며 present 비트가 선 리프만 처리하는 4중 루프로 새로 짰다 — 32비트 시절엔 유저 공간이 PDE 0번 슬롯 하나(4MB) 뿐이라 하드코딩할 수 있었지만, 4단계에서는 인덱스가 훨씬 깊어져 제너릭 워커가 더 안전하다.

### kheap 재배치 — 3GB 스플릿에서 PDPT_hi[511]로

| 항목 | 32비트(~38) | 64비트(39) |
|------|-------------|------------|
| KHEAP_START | `0xC0400000` | `0xFFFFFFFFC0400000` |
| KHEAP_MAX | `0xC0800000` | `0xFFFFFFFFC0800000` |
| 소속 | 3GB 유저/커널 스플릿의 PDE 768+ 구간 | `boot_pdpt_hi`의 511번 슬롯(커널 -1GB 구간), PML4[511]은 커널과 동일 |

낮은 32비트 숫자(`C0400000`/`C0800000`)를 그대로 남기고 canonical 상위 비트(`0xFFFFFFFF`)만 얹은 것은 의도적이다 — 32비트 시절 값을 알아볼 수 있게 남기면서, `page_map_frame`의 `vaddr` 매개변수를 `u32`에서 `u64`로 넓혀야 한다는 사실도 이름에서 드러난다. `boot_pdpt_hi[510]`(커널 자신, entry.asm이 미리 채움)과 `[511]`(힙, `walk_pt`가 최초 접근 시 새 PD를 `page_alloc()`으로 만듦)은 같은 PDPT 안의 이웃 슬롯이다.

### thread_t: esp/kstack_top를 u64로, 초기 스택 레이아웃 재설계

`context_switch.asm`은 37에서 이미 64비트로 바뀌어 있었지만(`pushfq`+`rbx`+`rbp`+`r12`+`r13`+`r14`+`r15` = 7개), `thread.c`의 `thread_create_with_data`는 여전히 32비트 `pusha` 시절 스택(11개 슬롯)을 흉내내고 있었다. 실제 push 순서에 맞춰 9슬롯으로 다시 짰다:

| 스택 위치(낮은 주소부터, `switch_context`의 `pop` 순서) | 값 |
|---|---|
| r15 | 0 |
| r14 | 0 |
| r13 | 0 |
| r12 | 0 |
| rbp | 0 |
| rbx | 0 |
| rflags | `0x202` |
| **switch_context의 `ret`가 여기로 점프** | `fn` |
| fn이 끝까지 실행되고 `ret`하면 여기로 | `thread_exit` |

`0x202`는 이 코드베이스에서 원래도 "새로 시작하는 실행 흐름의 기본 플래그"로 쓰이던 상수다(IF=1, 예약비트만 서 있음) — 40에서 `enter_user_mode`가 유저 프로세스에 심는 초기 rflags도 동일한 값을 쓴다.

`THREAD_STACK_SIZE`는 4096(32비트 시절 값)에서 16384로 올렸다. 64비트 레지스터/포인터는 32비트보다 콜 프레임이 커지므로, 같은 호출 체인이라도 커널 스택을 더 많이 먹는다.

### kernel_main 배선 — 커널 쓰레드만으로 검증

`paging_init` → `phys_mem_init` → `interrupts_init` → `kheap_init` → `timer_init`/`keyboard_init` → `interrupts_enable` 순으로 켠 뒤(38의 `timer_sleep(200)` IRQ0 데모는 그대로 유지), `threads_init()`으로 스케줄러를 켜고 `thread_create()`로 커널 쓰레드 두 개(`thread_a`, `thread_b`)를 만든다. 각 쓰레드는 메시지를 세 번 찍고 `thread_yield()`를 부르다 함수가 끝나면 자동으로 `thread_exit`로 빠진다(위 스택 레이아웃 표의 마지막 줄). `kernel_main`은 `thread_any_runnable()`이 거짓이 될 때까지 `hlt`로 대기한다 — 타이머 IRQ가 `scheduler_tick()`을 통해 idle(=kernel_main 자신)에서 두 쓰레드로, 두 쓰레드끼리는 `thread_yield()`로 서로 넘나든다.

이 데모는 **의도적으로 유저모드/시스템콜을 전혀 쓰지 않는다** — 순수 커널-대-커널 컨텍스트 스위치만으로 스케줄러가 64비트에서 정상 동작하는지 확인하는 것이 39의 범위다. 시스템 콜 핸들러 한복판(`int 0x80` 트랩 안)에서 중첩 타이머 인터럽트가 스케줄러를 건드리는 조합은 40에서 유저 프로세스가 뜨고 나서야 나타난다.

### `process.c`/`elf.c`/`syscall.c`/`initrd.c`/`gdt.asm`은 그대로 둠

이 파일들은 `paging.h`의 새 시그니처(`u64 vaddr`)를 받는 함수를 호출하지만, 인자로 넘기는 값 자체는 여전히 `u32`라 C의 암묵적 확장(zero-extend)으로 경고 없이 컴파일된다. `-Wall -Wextra`로 빌드하면 `initrd.c`/`elf.c`에서 "cast to/from pointer of different size" 경고가 남는데, 이건 회귀가 아니라 **아직 포팅하지 않은 부분이 정확히 어디인지 보여주는 표시**다 — 40에서 이 두 파일을 포팅하면 사라진다.

## 명령

```bash
make            # build/os.iso 생성
make run        # QEMU GUI
make run-nogui  # debugcon 출력 확인 — 5초 후 종료 (정상)
make clean
```

## 완료 기준

`make run-nogui`:

```
Hello world -- long mode (64-bit), threads
GDT ready: null/kcode64/kdata/ucode64/udata/TSS(0x28)
paging: 4-level PAE active, 2MB huge pages, 1GB identity+kernel
phys mem: 32588 free pages (127MB usable)
IDT ready: 256 entries (16-byte gates) PIC=0x20/0x28 syscall=0x80(DPL=3)
heap: kernel dir adopted, window at 0xFFFFFFFFC0400000 (mapped=128MB)
timer: PIT 100Hz IRQ0 ready
keyboard ready: IRQ1 unmasked
sleep 200ms: ticks before=1
sleep done: ticks after=21 (delta=20)
threads: 2 kernel threads created
thread A: tick 0
thread B: tick 0
thread A: tick 1
thread B: tick 1
thread A: tick 2
thread B: tick 2
long mode: paging+kheap+thread port complete (usermode in 40)
```

(free page 수, tick 수는 빌드마다 달라질 수 있다.)

## 이전 단계(38) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/entry.asm` | 수정 | `boot_pml4`에 `global` 추가 — C에서 물리주소를 얻어 재사용 |
| `boot/paging.h` | 수정 | `paging_init`이 `u64 mmap_addr`를 받도록, `page_map_frame`/`paging_map_user_page`가 `u64 vaddr`를 받도록 시그니처 변경; `pd_phys`→`pml4_phys`로 개명(4단계 전환 후 실제로 가리키는 게 PML4임을 이름에 반영) |
| `boot/paging.c` | 전면 재작성 | 2-레벨 32비트 `page_directory`/`page_tables` 폐기, `boot_pml4` 기반 4단계 워커(`next_level`/`walk_pt`)로 clone/map/free/copy 재구현; `kernel_pd_phys`/`pd_phys` 계열 변수명도 `pml4_phys` 계열로 개명 |
| `boot/kheap.h` | 수정 | `KHEAP_START`/`KHEAP_MAX`를 `0xFFFFFFFFC04/800000` canonical 주소로 재배치 |
| `boot/kheap.c` | 수정 | `heap_top`을 `u64`로 |
| `boot/thread.h` | 수정 | `esp`→`rsp`(u64), `kstack_top`을 u64로, `threads_init` 매개변수 u64 |
| `boot/thread.c` | 수정 | `switch_context` 프로토타입 u64화; `thread_create_with_data`의 초기 스택을 7레지스터+fn+thread_exit 9슬롯으로 재설계; `THREAD_STACK_SIZE` 4096→16384 |
| `boot/kernel.c` | 수정 | `paging_init`/`kheap_init`/`threads_init` 호출 추가; 커널 쓰레드 2개(`thread_a`/`thread_b`) 생성 후 `thread_any_runnable()`로 대기하는 데모 추가; 준비 메시지 갱신 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `40-usermode-64`: `gdt.asm`의 `enter_user_mode`/`enter_user_mode_fork`를 `iretq` 기반으로 실제 구현, `elf.c`를 ELF64로, `process.c`/`syscall.c`의 `fork_resume_t`를 u64 9필드로, `initrd.c`를 u64 주소로 포팅하고 `kernel_main`에서 `proc_spawn("init")`까지 이어붙여 유저 셸을 실제로 띄운다(64비트 전환 4/4).
- `paging_free_user_pages`/`paging_copy_user_pages`는 지금 PML4 0~510번 전체를 매번 순회한다(유저 공간이 4MB 이내라 실질 비용은 작다) — 유저 주소공간이 커지는 이후 단계(43-mmap 등)에서 필요하면 사용 중인 PML4 인덱스 범위만 추적하는 최적화를 고려.
