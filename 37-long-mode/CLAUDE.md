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
| `boot/gdt.c` | 수정 | struct gdt_descriptor(u16+u64 packed); struct tss64(rsp0); gdt[7]; gdt_init(u64); tss_set_desc 16바이트 인코딩 |
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
