# 37 — long-mode

**목표**: PAE + Long Mode 진입, 4-레벨 페이지 테이블, 64비트 GDT/TSS — 커널 64비트 전환 1/3.

**36에서 이어짐**: 36까지는 32비트 보호 모드 커널이었다. 여기서 64비트 long mode로 전환하고, 기존 C 코드들을 64비트용으로 조정한다. IDT/인터럽트·프로세스 코드는 38/39에서 포팅 완료한다.

상위 컨텍스트는 부모 디렉토리의 `CLAUDE.md` 참고.

## 핵심 개념

### BITS 32 → BITS 64 전환 순서 (entry.asm)

1. **CR3 설정**: 물리 주소로 boot_pml4 로드
2. **CR4.PAE 활성화**: `or eax, (1 << 5)` — PAE 없이는 4-레벨 페이지 테이블 불가
3. **EFER.LME 활성화**: MSR `0xC0000080` bit 8 — Long Mode Enable
4. **CR0.PG 설정**: 페이징 ON과 동시에 Long Mode Active(LMA) 전환
5. **lgdt + far jump**: 64비트 코드 세그먼트(L=1)로 분기해야 실제 64비트 모드 진입

### 4-레벨 페이지 테이블 구조

- PML4[0]   → boot_pdpt_id[0]  → boot_pd: identity 매핑 (VA 0x00000000–0x3FFFFFFF)
- PML4[511] → boot_pdpt_hi[510] → boot_pd: 커널 매핑 (VA 0xFFFFFFFF80000000–0xFFFFFFFFBFFFFFFF)
- 두 PDPT가 동일한 boot_pd를 가리킨다 → 물리 0–1GB를 2MB huge page 512개로 커버
- VA 0xFFFFFFFF80000000 분해: PML4[511], PDPT[510], PD[0] (bit 47 이상 모두 1)
- boot_pml4/pdpt_id/pdpt_hi/pd는 entry.asm .bss에서 물리 주소(`symbol - KERNEL_OFFSET`)로 접근

### 64비트 GDT

| 인덱스 | 셀렉터 | 값 | 설명 |
|--------|--------|-----|------|
| 0 | 0x00 | 0 | null |
| 1 | 0x08 | 0x00AF9A000000FFFF | 커널 코드 64 (L=1, D=0) |
| 2 | 0x10 | 0x00CF92000000FFFF | 커널 데이터 |
| 3 | 0x18 | 0x00AFFA000000FFFF | 유저 코드 64 |
| 4 | 0x20 | 0x00CFF2000000FFFF | 유저 데이터 |
| 5–6 | 0x28 | 16바이트 TSS 디스크립터 | 두 슬롯 사용 |

64비트 코드 세그먼트: L=1(bit 53), D=0(bit 54) 필수. 32비트와 달리 G/Limit 필드는 무시된다.

### 64비트 TSS

64비트 TSS 디스크립터는 16바이트(두 GDT 슬롯)를 사용한다. 구조체에 `rsp0` 필드가 있으며, ring 3 → ring 0 진입 시 사용할 커널 스택 주소를 담는다.

### CS 재로드 — retfq 방식

`lgdt` 이후 CS를 새 세그먼트로 교체할 때:
```asm
pop  rax               ; 리턴 주소 저장
push qword 0x08        ; 새 CS
push rax               ; RIP
retfq                  ; CS:RIP far return
```
`jmp far` 문법 대신 스택을 통해 far return하는 관용구. `retfq`는 64비트 far return (`rex.W lret`).

### RIP-relative 주소 참조

`.text` 섹션에서 `.bss` 심볼을 참조할 때 반드시 `[rel symbol]` 형태를 써야 한다. 일반 참조(`[symbol]`)는 R_X86_64_32S 재배치를 생성하는데, VA가 0xC0000000 이상이면 signed 32비트 범위를 초과해 링커 에러가 발생한다.

### -mcmodel=kernel 사용 이유

커널 VA가 0xFFFFFFFF80000000 이상(canonical upper half, 상위 2GB)에 위치한다. `kernel` 모델은 모든 커널 심볼이 최상위 2GB 안에 있음을 가정하며, RIP-relative 또는 부호 확장 32비트 절대 주소로 참조한다. `large` 모델 대비 코드가 더 간결하고 이것이 표준 64비트 커널 레이아웃이다.

### -mno-sse / -mno-sse2 / -mno-mmx 필수

GCC `-O2`는 정적 배열 초기화에 SSE `movaps`/`movdqa` 명령어를 생성한다. `movaps`는 **16바이트 정렬**을 요구하는데, 커널 정적 배열은 기본적으로 8바이트 정렬이므로 `#GP` fault로 triple fault가 발생한다. 커널 CFLAGS에 이 세 플래그를 추가하면 SSE 코드 생성이 억제된다.

### BSS alignb vs align (NASM)

NASM `.bss` 섹션에서는 `align`이 아닌 `alignb`를 써야 한다. `align`은 패딩 바이트를 0으로 초기화하려 해서 "attempt to initialize memory in BSS section" 경고가 발생한다.

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
Hello world -- long mode (64-bit)
GDT ready: null/kcode64/kdata/ucode64/udata/TSS(0x28)
paging: 4-level PAE active, 2MB huge pages, 1GB identity+kernel
phys mem: 32555 free pages (127MB usable)
long mode: kernel ready (IDT/processes in 38/39)
```

## 이전 단계(36) 대비 변경 파일

| 파일 | 상태 | 설명 |
|------|------|------|
| `boot/entry.asm` | 수정 | BITS 32/64 혼합; 4-레벨 페이지 테이블 초기화(PML4[0]/[511], PDPT_id[0], PDPT_hi[510]); PAE/EFER/CR0 설정; lgdt+jmp 64비트; BSS 4테이블(pml4/pdpt_id/pdpt_hi/pd) |
| `boot/gdt.asm` | 수정 | BITS 64; gdt_flush는 lgdt+세그먼트 재로드+retfq; tss_flush는 ltr |
| `boot/gdt.c` | 수정 | 36의 `gdt_set_entry`/`gdt_entry` 구조 그대로 유지; `gdt[7]`(TSS가 16바이트라 슬롯 1개 추가); code 세그먼트 gran 0xCF→0xAF(L=1); `tss_entry`(rsp0/ist1-7, 64비트 TSS 레이아웃); `tss_init`이 `gdt_set_entry(5,...)`로 base 하위 32비트를 넣고 `gdt[6]`에 base 상위 32비트를 한 줄 추가로 기록; `gdt_pointer.base`/`gdt_init` 인자 u64 |
| `boot/gdt.h` | 수정 | gdt_init/gdt_set_kernel_stack 시그니처를 u64 인자로 변경 |
| `boot/console.h` | 수정 | u64 typedef; KERNEL_OFFSET 0xFFFFFFFF80000000ULL (canonical upper half) |
| `boot/phys_mem.h` | 수정 | phys_mem_init의 mmap_addr 타입 u32→u64 |
| `boot/phys_mem.c` | 수정 | phys_mem_init 시그니처 u64; 포인터 캐스트 u64 offset 적용 |
| `boot/kernel.c` | 수정 | kend_phys 연산 (u32)((u64)kernel_end - KERNEL_OFFSET) 수정; kernel_main 단순화 |
| `boot/context_switch.asm` | 수정 | BITS 64; AMD64 ABI; 칼리-세이브 레지스터 저장/복원; stub (39에서 실제 사용) |
| `boot/interrupts.asm` | 수정 | BITS 64; push qword; iretq; 테이블 엔트리 dq로 변경 |
| `boot/interrupts.c` | 수정 | read_cr2()의 cr2 변수를 u64로 변경 |
| `boot/paging.c` | 수정 | flush_tlb/paging_init 등 CR3 관련 u64 캐스트 |
| `linker.ld` | 수정 | KERNEL_OFFSET = 0xFFFFFFFF80000000 (canonical upper half) |
| `Makefile` | 수정 | qemu-system-x86_64; CFLAGS -m64/-mcmodel=kernel/-mno-sse/-mno-sse2/-mno-mmx/-mno-red-zone; LDFLAGS -m elf_x86_64; NASM -f elf64 |
| `CLAUDE.md` | 신규 | 이 문서 |

## 다음 단계 힌트

- `38-idt-64`: 64비트 IDT 재구성 (16바이트 게이트 디스크립터), `syscall` MSR 기반 시스템 콜 진입

### 38에서 반드시 같이 손볼 것

37은 `interrupts.asm`/`context_switch.asm`을 `BITS 64`로만 바꿔서 빌드가 되게 해뒀을 뿐, 인터럽트 프레임 설계 자체는 아직 32비트 시절 그대로다. `kernel_main`이 인터럽트를 켜지 않아 지금은 조용하지만, 38에서 IDT를 살리는 순간 바로 드러난다.

- `interrupts.h`의 `struct interrupt_frame`이 전부 `u32`다. 그런데 `interrupts.asm`의 `interrupt_common`은 `push rax/rcx/rdx/rbx/rbp/rsi/rdi` — 전부 8바이트 push고, 개수도 옛날 `pusha`(8개, esp 포함)보다 하나 적다(7개, esp 없음). 필드 폭(4바이트 vs 8바이트)과 순서가 둘 다 어긋나 있어서, 지금 그대로 인터럽트를 걸면 `frame->vector`가 실제로는 `rdx`의 하위 4바이트를 읽는 식으로 완전히 엉뚱한 값이 나온다. 게이트를 16바이트로 바꾸는 이 단계에서 프레임 struct도 실제 push 순서(rdi,rsi,rbp,rbx,rdx,rcx,rax,vector,error_code,hw-iret 5개: rip,cs,rflags,rsp,ss)에 맞춰 `u64` 필드로 다시 설계해야 한다.
- `interrupts.c`의 `struct idt_entry`(8바이트)와 `idt_set_entry(u8, u32 handler, ...)`도 옛날 32비트 게이트 포맷이다. 핸들러 주소가 이제 canonical 상위 주소(`0xFFFFFFFF80xxxxxx`)라 `u32`로 받으면 그대로 잘린다. 16바이트 게이트 + `u64` 핸들러로 교체.
- `syscall.c`의 `frame->eax/ebx/ecx/edx/edi/esi/ebp/user_esp/eflags` 접근부는 프레임 struct가 바뀌면 자동으로 같이 갱신해야 한다.
- **39와의 접점**: `process.c`의 `fork_resume_t`(`process.h`, 현재 `u32` 8개 필드)는 이 인터럽트 프레임에서 값을 그대로 옮겨 채운다(`syscall.c`의 `SYS_FORK`/`SYS_CLONE` 케이스). 프레임을 `u64`로 새로 설계할 때 `fork_resume_t`의 폭도 같이 맞춰서 39에서 또 뜯어고치는 일이 없게 할 것.

- `39-port-64`: process/thread/ELF/context_switch 64비트 포팅 완료 — 64비트 전환 3/3

### 39에서 반드시 같이 손볼 것 (로드맵 제목엔 안 보이지만 빠지면 안 되는 것들)

- **`paging.c`/`paging.h` 전체 재작성이 필요하다.** 지금은 `entry.asm`이 세운 4-레벨 PAE 테이블(PML4→PDPT→PD, 2MB huge page)과는 완전히 다른, 옛날 non-PAE 2-레벨 모델(`page_directory[1024]`, `page_tables[MAX_PT][1024]`, 전부 4바이트 엔트리, PDE=22비트/PTE=12비트 시프트)을 그대로 갖고 있다. `kernel_main`이 `paging_init()`을 더 이상 안 불러서 지금은 죽은 코드지만, `process.c`(spawn/fork/exec)·`elf.c`(`elf_load_process`)·`kheap.c`(`heap_grow`→`page_map_frame`)가 전부 이 API(`paging_clone_dir`/`paging_map_user_page`/`paging_free_user_pages`/`paging_copy_user_pages`/`page_map_frame`)에 의존한다. 타입 캐스팅 수준이 아니라 PML4/PDPT/PD/PT 4-레벨 워크 자체를 새로 짜야 한다.
- **`thread.h`의 `thread_t.esp`/`kstack_top`/`pd`가 여전히 `u32`다.** 커널 스택·힙 주소가 이제 canonical 상위 주소라 `u32`에는 애초에 담을 수 없다. `u64`로 교체.
- **`thread.c`의 `thread_create_with_data`가 새 스레드 스택을 옛날 방식(8개 `u32` 레지스터 + eflags + fn + thread_exit)으로 조작한다.** 그런데 실제 `context_switch.asm`은 `pushfq` + `rbx/rbp/r12/r13/r14/r15`(6개, 전부 qword) 순서로 저장/복원한다. 개수(8개 vs 6개)와 폭(4바이트 vs 8바이트)이 둘 다 다르므로, 실제 저장/복원 순서에 맞춰 qword 8개(rflags, rbx, rbp, r12, r13, r14, r15, entry_fn) 레이아웃으로 다시 짜야 한다.
- **`kheap.h`의 `KHEAP_START`(`0xC0400000`)/`KHEAP_MAX`(`0xC0800000`)가 옛 3GB-split(`KERNEL_OFFSET=0xC0000000`) 시절 주소다.** 지금 `entry.asm`이 매핑해둔 범위(0~1GB identity, `0xFFFFFFFF80000000`~ 커널 상위 별칭) 어디에도 안 걸린다. 새 커널 VA 맵을 그릴 때 kheap이 들어갈 대역을 다시 정해야 한다(예: 커널 상위 1GB 안에서 코드/데이터 뒤쪽에 배정하거나, 별도 PDPT 슬롯 추가).
- **`elf.c`의 Elf32→Elf64 전환**(로드맵에 이미 명시).
- **`user/*.c`, `user/user.ld`가 지금 `-m32 -march=i386` / `-m elf_i386`으로 빌드된다.** 커널만 64비트로 포팅해도 유저 바이너리가 32비트 그대로면 실행이 안 되므로, 유저 빌드도 64비트(`-m64`, `elf_x86_64`)로 같이 바꿔야 한다.
